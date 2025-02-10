/*
 * atomsnap.c - Atomic snapshot management library
 *
 * This file implements an atomic snapshot mechanism for managing a pointer
 * to a consistent snapshot state along with reference counts. The design
 * packs an outer reference count and a snapshot pointer into a single 64-bit
 * control block stored in the atomsnap_gate structure, while the snapshot
 * itself (atomsnap) maintains an inner reference count.
 *
 * The 8-byte control block in atomsnap_gate is structured as follows:
 *   - Upper 16 bits: outer reference counter.
 *   - Lower 48 bits: index (or pointer) of the current snapshot version.
 *
 * Writers have their own snapshot version and each snapshot can be concurrently
 * read by multiple readers. If a writer were to simply deallocate an existing
 * snapshot to replace it, readers might access wrong memory. To avoid this,
 * multiple versions of the snapshot are maintained.
 * 
 * When a reader wants to access the current snapshot, it atomically increments
 * the outer reference counter using fetch_add(). The returned 64-bit value has
 * its lower 4 bytes representing the index of the snapshot whose reference count
 * was increased. This allows the reader to obtain both the snapshot index and
 * ensure that the reference counter is safely incremented.
 *
 * After finishing the use of a snapshot, a reader must release it. During release,
 * the reader increments the inner reference counter by 1. If the resulting inner
 * counter becomes 0, it indicates that no other threads are referencing that snapshot,
 * so the snapshot can be freed.
 *
 * In the snapshot replacement process, the writer atomically exchanges the 8-byte
 * control block with a new one (using atomic exchange), and the old control block,
 * which contains the previous outer reference count and snapshot pointer, is
 * returned. Because this update is atomic, new readers cannot access the old
 * version. The writer then decrements the old snapshot's inner counter by th
 * eouter reference count. Consequently, if a reader's release operation causes
 * the inner counter to reach 0, this reader is the last user of that snapshot.
 * If the writer's release operation causes the inner counter to reach 0, this
 * writer is the last user of that snapshot. Then the last user can free the old
 * snapshot.
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

/*
 * atomsnap - snapshot object structure
 * @inner_refcnt: atomic inner reference counter for the snapshot
 * @object: pointer to the actual snapshot data.
 *
 * atomsnap->inner_refcnt is used to determine when it is safe to free the
 * snapshot.
 */
struct atomsnap {
	_Atomic int64_t inner_refcnt;
	void *object;
};

/*
 * atomsnap_gate - gate for atomic snapshot read/write
 * @outer_refcnt_and_ptr: control block to manage snapshot versions
 *
 * Writers use atomsnap_gate to atomically register their object snapshot.
 * Readers also use this gate to obtain atomic object safely.
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
	if (gate == NULL)
		return;

	free(gate);
}

/*
 * Get the actual object from the snapshot.
 */
void *atomsnap_get_object(struct atomsnap *snapshot)
{
	if (snapshot == NULL)
		return NULL;
	return snapshot->object;
}

/*
 * Set the object pointer into the given snapshot.
 */
void atomsnap_set_object(struct atomsnap *snapshot, void *obj)
{
	if (snapshot == NULL)
		return;
	snapshot->object = obj;
}

/*
 * atomsnap_acquire - atomically acquire the current version
 * @gate: poinetr of the atomsnap_gate
 *
 * Atomically increments the outer reference counter and get the pointer of the
 * current snapshot version.
 */
struct atomsnap *atomsnap_acquire(
	struct atomsnap_gate *gate)
{
	uint64_t outer;

	if (gate == NULL)
		return NULL;

	outer = atomic_fetch_add(&gate->outer_refcnt_and_ptr, OUTER_REF_CNT);
	return (struct atomsnap *)GET_OUTER_PTR(outer);
}

/*
 * atomsnap_release - release the given snapshot version after usage
 * @snapshot: pointer to atomsnap being released
 *
 * Release the snapshot by incrementing the inner reference count by 1. If the
 * updated inner counter becomes 0, it indicates that no other threads reference
 * this version and it can be safely freed.
 *
 * Return ATOMSNAP_SAFE_FREE if the snapshot is safe to free,
 * ATOMSNAP_FREE_UNSAFE otherwise.
 */
ATOMSNAP_STATUS atomsnap_release(struct atomsnap *snapshot)
{
	int64_t inner_refcnt
		= atomic_fetch_add(&snapshot->inner_refcnt, 1) + 1;

	if (inner_refcnt == 0) {
		return ATOMSNAP_FREE_SAFE;
	}

	return ATOMSNAP_FREE_UNSAFE;
}

/*
 * atomsnap_test_and_set - atomically repalce the version of snapshot
 * @gate: pointer to the atomsnap_gate
 * @snapshot: new snapshot to install
 * @old_snapshot_status: pointer to return the old verion's status
 *
 * This function atomically exchanges the current version of snapshot with a new
 * one using atomic_exchange(). It then extracts the outer reference count and
 * substracts the old snapshot's inner reference counter. The result indicates
 * whether the old snapshot is safe to free.
 *
 * Returns the old atomsnap's pointer.
 */
struct atomsnap *atomsnap_test_and_set(
	struct atomsnap_gate *gate, struct atomsnap *snapshot,
	ATOMSNAP_STATUS *old_snapshot_status)
{
	uint64_t old_outer, old_outer_refcnt;
	struct atomsnap *old_snapshot;
	int64_t old_inner_refcnt;

	old_outer = atomic_exchange(&gate->outer_refcnt_and_ptr, 
		(uint64_t)snapshot);
	old_outer_refcnt = GET_OUTER_REFCNT(old_outer);
	old_snapshot = (struct atomsnap *)GET_OUTER_PTR(old_outer);
	
	old_inner_refcnt = atomic_fetch_sub(&old_snapshot->inner_refcnt,
		old_outer_refcnt) - old_outer_refcnt;
	if (old_inner_refcnt == 0) {
		*old_snapshot_status = ATOMSNAP_FREE_SAFE;
	} else {
		*old_snapshot_status = ATOMSNAP_FREE_UNSAFE;
	}

	return old_snapshot;
}

/*
 * atomsnap_compare_and_exchange - conditonally replace the snapshot
 * @gate: pointer to the atomsnap_gate
 * @old_snapshot: expected pointer of current snapshot
 * @new_snapshot: new snapshot to install
 * @old_snapshot_status: pointer to return the old verion's status
 *
 * If the current pointer matches @old_snapshot, replace it to the
 * @new_snapshot. 
 *
 * On success, it adjusts the inner reference counter of the old snapshot based
 * on the outer reference counter, and set the free-safety status.
 *
 * Return true if the snapshot was successfully replace; false otherwise.
 */
bool atomsnap_compare_and_exchange(struct atomsnap_gate *gate,
	struct atomsnap *old_snapshot, struct atomsnap *new_snapshot,
	ATOMSNAP_STATUS *old_snapshot_status)
{
	uint64_t old_outer, old_outer_refcnt;
	int64_t old_inner_refcnt;

	old_outer = atomic_load(&gate->outer_refcnt_and_ptr);

	if (old_snapshot != (struct atomsnap *)GET_OUTER_PTR(old_outer))
		return false;

	if (!atomic_compare_exchange_weak(&gate->outer_refcnt_and_ptr,
			&old_outer, (uint64_t)new_snapshot))
		return false;

	old_outer_refcnt = GET_OUTER_REFCNT(old_outer);
	old_inner_refcnt = atomic_fetch_sub(&old_snapshot->inner_refcnt,
		old_outer_refcnt) - old_outer_refcnt;

	if (old_inner_refcnt == 0) {
		*old_snapshot_status = ATOMSNAP_FREE_SAFE;
	} else {
		*old_snapshot_status = ATOMSNAP_FREE_UNSAFE;
	}

	return true;
}
