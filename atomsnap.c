/*
 * atomsnap.c - Atomic multi-version concurrency control library
 *
 * This file implements an mvcc mechanism for managing a pointer to a consistent
 * state along with reference counts. The design packs an outer reference count
 * and a version pointer into a single 64-bit control block stored in the
 * atomsnap_gate structure, while the version itself (atomsnap_version) maintains an
 * inner reference count.
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
 * control block with a new one (using atomic exchange), and the old control block,
 * which contains the previous outer reference count and version pointer, is
 * returned. Because this update is atomic, new readers cannot access the old
 * version anymore. The writer then decrements the old version's inner counter by the
 * old outer reference count.
 *
 * Consequently, if a reader's release operation causes the inner counter to reach 0,
 * this reader is the last user of that version. If the writer's release operation
 * causes the inner counter to reach 0, this writer is the last user of that version.
 * Then the last user (reader or writer) can free the old version.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
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
 * atomsnap_version - version object structure
 * @inner_refcnt: atomic inner reference counter for the version
 * @object: pointer to the actual data.
 *
 * atomsnap_version->inner_refcnt is used to determine when it is safe to free.
 */
struct atomsnap_version {
	_Atomic int64_t inner_refcnt;
	void *object;
};

/*
 * atomsnap_gate - gate for atomic version read/write
 * @outer_refcnt_and_ptr: control block to manage multi-versions
 *
 * Writers use atomsnap_gate to atomically register their object version.
 * Readers also use this gate to get the object and release safely.
 */
struct atomsnap_gate {
	_Atomic uint64_t outer_refcnt_and_ptr;
};

/*
 * Returns pointer to a atomsnap_gate, or NULL on failure.
 */
struct atomsnap_gate *atomsnap_init_gate()
{
	atomsnap_gate *gate = calloc(1, sizeof(atomsnap_gate));

	if (gate == NULL) {
		fprintf(stderr, "atomsnap_init_gate: gate allocation failed\n");
		return NULL;
	}

	return gate;
}

/*
 * Free the atomsnap_gate.
 */
void atomsnap_destroy_gate(struct atomsnap_gate *gate)
{
	if (gate == NULL) {
		return;
	}

	free(gate);
}

/*
 * Get the actual object from the version.
 */
void *atomsnap_get_object(struct atomsnap_version *version)
{
	if (version == NULL) {
		return NULL;
	}

	return version->object;
}

/*
 * Set the object pointer into the given version.
 */
void atomsnap_set_object(struct atomsnap_version *version, void *obj)
{
	if (version == NULL) {
		return;
	}
	
	atomic_store(&version->object, obj);
	atomic_store(&version->inner_refcnt, 0);
}

/*
 * atomsnap_acquire_version - atomically acquire the current version
 * @gate: poinetr of the atomsnap_gate
 *
 * Atomically increments the outer reference counter and get the pointer of the
 * current version.
 */
struct atomsnap_version *atomsnap_acquire_version(struct atomsnap_gate *gate)
{
	uint64_t outer;

	if (gate == NULL) {
		return NULL;
	}

	outer = atomic_fetch_add(&gate->outer_refcnt_and_ptr, OUTER_REF_CNT);

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
 * Return ATOMSNAP_SAFE_FREE if the version is safe to free, ATOMSNAP_UNSAFE_FREE
 * otherwise.
 */
ATOMSNAP_STATUS atomsnap_release_version(struct atomsnap_version *version)
{
	int64_t inner_refcnt;

	if (version == NULL) {
		return ATOMSNAP_UNSAFE_FREE;
	}

	inner_refcnt = atomic_fetch_add(&version->inner_refcnt, 1) + 1;

	if (inner_refcnt == 0) {
		return ATOMSNAP_SAFE_FREE;
	}

	return ATOMSNAP_UNSAFE_FREE;
}

/*
 * atomsnap_test_and_set - atomically repalce the version
 * @gate: pointer to the atomsnap_gate
 * @version: new version to install
 * @old_version_status: pointer to return the old verion's status
 *
 * This function atomically exchanges the current version with a new one using
 * atomic_exchange. It then extracts the outer reference count and substracts
 * the old version's inner reference cuonter. The result indicates whether the
 * old version is safe to free.
 *
 * Returns the old version's pointer.
 */
struct atomsnap_version *atomsnap_test_and_set(
	struct atomsnap_gate *gate, struct atomsnap_version *new_version,
	ATOMSNAP_STATUS *old_version_status)
{
	uint64_t old_outer, old_outer_refcnt;
	struct atomsnap_version *old_version;
	int64_t inner_refcnt;

	old_outer = atomic_exchange(&gate->outer_refcnt_and_ptr, 
		(uint64_t)new_version);
	old_outer_refcnt = GET_OUTER_REFCNT(old_outer);
	old_version = (struct atomsnap_version *)GET_OUTER_PTR(old_outer);
	*old_version_status = ATOMSNAP_UNSAFE_FREE; /* initial status */

	if (old_version == NULL) {
		return NULL;
	}

	/* Consider wrapaound */
	atomic_fetch_and(&old_version->inner_refcnt, WRAPAROUND_MASK);

	/* Decrease inner ref counter, we expect the result is minus */
	inner_refcnt = atomic_fetch_sub(&old_version->inner_refcnt,
		old_outer_refcnt) - old_outer_refcnt;

	/* The outer counter has been wraparouned, adjust inner count */
	if (inner_refcnt > 0) {
		inner_refcnt = atomic_fetch_sub(&old_version->inner_refcnt,
			WRAPAROUND_FACTOR) - WRAPAROUND_FACTOR;
	}
	assert(inner_refcnt <= 0);

	if (inner_refcnt == 0) {
		*old_version_status = ATOMSNAP_SAFE_FREE;
	}

	return old_version;
}

/*
 * atomsnap_compare_and_exchange - conditonally replace the version
 * @gate: pointer to the atomsnap_gate
 * @old_version: expected pointer of current version
 * @new_version: new version to install
 * @old_version_status: pointer to return the old verion's status
 *
 * If the current pointer matches @old_version, replace it to the @new_veresion.
 *
 * On success, it adjusts the inner reference counter of the old version based
 * on the outer reference counter, and set the free-safety status.
 *
 * Return true if the version was successfully replace; false otherwise.
 */
bool atomsnap_compare_and_exchange(struct atomsnap_gate *gate,
	struct atomsnap_version *old_version, struct atomsnap_version *new_version,
	ATOMSNAP_STATUS *old_version_status)
{
	uint64_t old_outer, old_outer_refcnt;
	int64_t inner_refcnt;

	old_outer = atomic_load(&gate->outer_refcnt_and_ptr);
	old_outer_refcnt = GET_OUTER_REFCNT(old_outer);
	*old_version_status = ATOMSNAP_UNSAFE_FREE; /* initial status */

	if (old_version != (struct atomsnap_version *)GET_OUTER_PTR(old_outer)) {
		return false;
	}

	if (!atomic_compare_exchange_weak(&gate->outer_refcnt_and_ptr,
			&old_outer, (uint64_t)new_version)) {
		return false;
	}

	if (old_version == NULL) {
		return true;
	}

	/* Consider wrapaound */
	atomic_fetch_and(&old_version->inner_refcnt, WRAPAROUND_MASK);

	/* Decrease inner ref counter, we expect the result minus */
	inner_refcnt = atomic_fetch_sub(&old_version->inner_refcnt,
		old_outer_refcnt) - old_outer_refcnt;

	/* The outer counter has been wraparouned, adjust inner count */
	if (inner_refcnt > 0) {
		inner_refcnt = atomic_fetch_sub(&old_version->inner_refcnt,
			WRAPAROUND_FACTOR) - WRAPAROUND_FACTOR;
	}
	assert(inner_refcnt <= 0);

	if (inner_refcnt == 0) {
		*old_version_status = ATOMSNAP_SAFE_FREE;
	}

	return true;
}
