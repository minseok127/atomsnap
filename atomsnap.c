/*
 * This file implements a grace-period mechanism for managing a pointer.
 * The design packs an outer reference count and a version pointer into 
 * a single 64-bit control block stored in the atomsnap_gate structure, 
 * while the version itself (atomsnap_version) maintains an inner 
 * reference count.
 *
 * The 8-byte control block in atomsnap_gate is structured as follows:
 *   - Upper 16 bits: outer reference counter.
 *   - Lower 48 bits: pointer of the current version.
 *
 * Writers have their own version and each version can be concurrently read by
 * multiple readers. If a writer simply deallocates an old version to
 * replace it, readers might access wrong memory. To avoid this, multiple
 * versions are maintained.
 * 
 * When a reader wants to access the current version, it atomically increments
 * the outer reference counter using fetch_add(). The returned 64-bit value has
 * its lower 48-bits representing the pointer of the version whose reference
 * count was increased.
 *
 * After finishing the use of the version, the reader must release it. During release,
 * the reader increments the inner reference counter by 1. If the resulting inner
 * counter is 0, it indicates that no other threads are referencing that
 * version, so it can be freed.
 *
 * In the version replacement process, the writer atomically exchanges the 8-byte
 * control block with a new one (using atomic instruction), and the old control block,
 * which contains the previous version's outer reference count and version pointer, 
 * is returned. Because this update is atomic, new readers cannot access the old
 * version anymore. The writer then decrements the old version's inner counter by the
 * old outer reference count.
 *
 * Consequently, if a reader's release operation makes the inner counter to reach 0,
 * this reader is the last user of that version. If the writer's release operation
 * makes the inner counter to reach 0, this writer is the last user of that version.
 * Then the last user (reader or writer) can free the old version.
 *
 * cf) Why we divide the reference counter into two?
 * Because it is only possible to increment the reference counter in an 8-byte 
 * control block, but it is not possible to decrement. The reader wants to decrease
 * the reference count at the end. But the writer may changed the control block 
 * to the other version, so the reader can't use it. So, the reader must notice 
 * to the other reference counter, inner counter.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>

#include <assert.h>

#include "atomsnap.h"

#define OUTER_REF_CNT	(0x0001000000000000ULL)
#define OUTER_REF_MASK	(0xffff000000000000ULL)
#define OUTER_PTR_MASK	(0x0000ffffffffffffULL)
#define OUTER_REF_SHIFT	(48)

#define GET_OUTER_REFCNT(outer) ((outer & OUTER_REF_MASK) >> OUTER_REF_SHIFT)
#define GET_OUTER_PTR(outer)	(outer & OUTER_PTR_MASK)

/* Difference between outer counter and inner counter must be <= 0xffff */
#define WRAPAROUND_FACTOR (0x10000ULL)
#define WRAPAROUND_MASK    (0xffffULL)

/*
 * atomsnap_gate - gate for atomic version read/write
 * @atomsnap_alloc_impl: user-defined memory allocation function
 * @atomsnap_free_impl: user-defined memory free function
 * @control_block: control block to manage multi-versions
 * @extra_control_blocks: array of extra control blocks
 * @num_extra_control_blocks: number of extra control blocks
 *
 * Writers use atomsnap_gate to atomically register their object version.
 * Readers also use this gate to get the object and release safely.
 */
struct atomsnap_gate {
	struct atomsnap_version *(*atomsnap_alloc_impl)(void *alloc_arg);
	void (*atomsnap_free_impl)(struct atomsnap_version *version);
	_Atomic uint64_t control_block;
	_Atomic uint64_t *extra_control_blocks;
	int num_extra_control_blocks;
};

/*
 * Returns pointer to an atomsnap_gate, or NULL on failure.
 */
struct atomsnap_gate *atomsnap_init_gate(struct atomsnap_init_context *ctx)
{
	atomsnap_gate *gate = calloc(1, sizeof(atomsnap_gate));

	if (gate == NULL) {
		fprintf(stderr, "atomsnap_init_gate: gate allocation failed\n");
		return NULL;
	}

	gate->atomsnap_alloc_impl = ctx->atomsnap_alloc_impl;
	gate->atomsnap_free_impl = ctx->atomsnap_free_impl;

	if (gate->atomsnap_alloc_impl == NULL || gate->atomsnap_free_impl == NULL) {
		free(gate);
		fprintf(stderr, "atomsnap_init_gate: invalid alloc/free function\n");
		return NULL;
	}

	gate->num_extra_control_blocks = ctx->num_extra_control_blocks;

	if (gate->num_extra_control_blocks < 0) {
		free(gate);
		fprintf(stderr, "atomsnap_init_gate: invalid num extra_blocks\n");
		return NULL;
	}

	if (gate->num_extra_control_blocks > 0) {
		gate->extra_control_blocks = calloc(gate->num_extra_control_blocks,
			sizeof(_Atomic uint64_t));

		if (gate->extra_control_blocks == NULL) {
			fprintf(stderr, "atomsnap_init_gate: extra block alloc failed\n");
			free(gate);
			return NULL;
		}
	}

	return gate;
}

/*
 * Destroy the atomsnap_gate.
 */
void atomsnap_destroy_gate(struct atomsnap_gate *gate)
{
	if (gate == NULL) {
		return;
	}

	if (gate->extra_control_blocks != NULL) {
		free(gate->extra_control_blocks);
	}

	free(gate);
}

/*
 * atomsnap_make_version - allocate memory for an atomsnap_version
 * @gate: pointer of the atomsnap_gate
 * @alloc_version_arg: argument of the user-defined version allocation function
 *
 * Allocate memory for an atomsnap_version. This function internally calls the
 * user-defined version allocation function with @alloc_arg as an argument.
 *
 * Note that the version's gate and opaque are initialized, but object and
 * free_context are not explicitly initialized within this function, as they may
 * have been set by the user-defined function.
 */
struct atomsnap_version *atomsnap_make_version(struct atomsnap_gate *gate,
	void *alloc_arg)
{
	struct atomsnap_version *new_version = gate->atomsnap_alloc_impl(alloc_arg);

	if (new_version != NULL) {
		atomic_store(&new_version->gate, gate);
		atomic_store((int64_t *)(&new_version->opaque), 0);
	}

	return new_version;
}

static inline _Atomic uint64_t *atomsnap__slot(
	struct atomsnap_gate *gate, int idx)
{
    return idx ? &gate->extra_control_blocks[idx - 1] : &gate->control_block;
}

/*
 * atomsnap_acquire_version_slot - atomically acquire the current version
 * @gate: poinetr of the atomsnap_gate
 * @slot_idx: zero-based index of the control-block slot (0 == default cb)	
 *
 * Atomically increments the outer reference counter and get the pointer of the
 * current version.
 */
struct atomsnap_version *atomsnap_acquire_version_slot(
	struct atomsnap_gate *gate, int slot_idx)
{
	_Atomic uint64_t *cb = atomsnap__slot(gate, slot_idx);
	uint64_t outer = atomic_fetch_add(cb, OUTER_REF_CNT);
	return (struct atomsnap_version *)GET_OUTER_PTR(outer);
}

/*
 * atomsnap_release_version - release the given version after usage
 * @version: pointer to atomsnap_version being released
 *
 * Release the version by incrementing the inner reference count by 1. If the
 * updated inner counter was 0, it indicates that no other threads reference
 * this version and it can be safely freed.
 *
 * If this version can be freed, call the user-defined version free function.
 * Note that we do not explicitly deallocate memory for the version or its
 * object pointer.
 */
void atomsnap_release_version(struct atomsnap_version *version)
{
	struct atomsnap_gate *gate = version->gate;
	int64_t inner_refcnt 
		= atomic_fetch_add((int64_t *)(&version->opaque), 1) + 1;

	if (inner_refcnt == 0) {
		gate->atomsnap_free_impl(version);
	}
}

/*
 * atomsnap_exchange_version_slot - unconditonally replace the version
 * @gate: poinetr of the atomsnap_gate
 * @slot_idx: zero-based index of the control-block slot (0 == default cb)	
 * @new_version: new version to be registered
 *
 * If a writer wants to exchange their version into the latest version
 * unconditonally, the writer should call this function.
 */
void atomsnap_exchange_version_slot(struct atomsnap_gate *gate, int slot_idx,
	struct atomsnap_version *new_version)
{
	uint64_t old_outer, old_outer_refcnt;
	struct atomsnap_version *old_version;
	int64_t inner_refcnt;
	_Atomic uint64_t *cb;

	cb = atomsnap__slot(gate, slot_idx);
	old_outer = atomic_exchange(cb, (uint64_t)new_version);
	old_version = (struct atomsnap_version *)GET_OUTER_PTR(old_outer);

	if (old_version == NULL) {
		return;
	}

	/* Consider wrapaound */
	atomic_fetch_and((int64_t *)(&old_version->opaque), WRAPAROUND_MASK);

	/* Decrease inner ref counter, we expect the result is minus */
	old_outer_refcnt = GET_OUTER_REFCNT(old_outer);
	inner_refcnt = atomic_fetch_sub((int64_t *)(&old_version->opaque),
		old_outer_refcnt) - old_outer_refcnt;

	/* The outer counter has been wraparouned, adjust inner count */
	if (inner_refcnt > 0) {
		inner_refcnt = atomic_fetch_sub((int64_t *)(&old_version->opaque),
			WRAPAROUND_FACTOR) - WRAPAROUND_FACTOR;
	}
	assert(inner_refcnt <= 0);

	if (inner_refcnt == 0) {
		gate->atomsnap_free_impl(old_version);
	}
}

/*
 * atomsnap_compare_exchange_version_slot - conditonally replace the version
 * @gate: poinetr of the atomsnap_gate
 * @slot_idx: zero-based index of the control-block slot (0 == default cb)	
 * @old_version: old version to compare
 * @new_version: new version to be registered
 *
 * If a writer wants to exchange their version into the latest version
 * only when the current latest version is @old_version, the writer should call
 * this function.
 */
bool atomsnap_compare_exchange_version_slot(struct atomsnap_gate *gate,
	int slot_idx, struct atomsnap_version *old_version,
	struct atomsnap_version *new_version)
{
	uint64_t old_outer, old_outer_refcnt;
	int64_t inner_refcnt;
	_Atomic uint64_t *cb;

	cb = atomsnap__slot(gate, slot_idx);
	old_outer = atomic_load(cb);

	if (old_version != (struct atomsnap_version *)GET_OUTER_PTR(old_outer)
			|| !atomic_compare_exchange_weak(cb,
					&old_outer, (uint64_t)new_version)) {
		return false;
	} 

	if (old_version == NULL) {
		return true;
	}

	/* Consider wrapaound */
	atomic_fetch_and((int64_t *)(&old_version->opaque), WRAPAROUND_MASK);

	/* Decrease inner ref counter, we expect the result minus */
	old_outer_refcnt = GET_OUTER_REFCNT(old_outer);
	inner_refcnt = atomic_fetch_sub((int64_t *)(&old_version->opaque),
		old_outer_refcnt) - old_outer_refcnt;

	/* The outer counter has been wraparouned, adjust inner count */
	if (inner_refcnt > 0) {
		inner_refcnt = atomic_fetch_sub((int64_t *)(&old_version->opaque),
			WRAPAROUND_FACTOR) - WRAPAROUND_FACTOR;
	}
	assert(inner_refcnt <= 0);

	if (inner_refcnt == 0) {
		gate->atomsnap_free_impl(old_version);
	}

	return true;
}
