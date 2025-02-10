#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>

#include <assert.h>

#include "atomic_snapshot.h"

#define OUTER_REF_CNT	(0x0001000000000000ULL)
#define OUTER_REF_MASK	(0xffff000000000000ULL)
#define OUTER_PTR_MASK	(0x0000ffffffffffffULL)
#define OUTER_REF_SHIFT	(16)

#define GET_OUTER_REFCNT(outer) ((outer & OUTER_REF_MASK) >> OUTER_REF_SHIFT)
#define GET_OUTER_PTR(outer)	(outer & OUTER_PTR_MASK)

struct atomic_snapshot {
	_Atomic int64_t inner_refcnt;
	void *obj;
};

struct atomic_snapshot_gate {
	_Atomic uint64_t outer_refcnt_and_ptr;
};

/*
 * Returns pointer to a atomic_snapshot_gate, or NULL on failure.
 */
struct atomic_snapshot_gate *atomic_snapshot_init_gate()
{
	atomic_snapshot_gate *gate = calloc(1, sizeof(atomic_snapshot_gate));

	if (gate == NULL) {
		fprintf(stderr, "atomic_snapshot_init_gate: gate allocation failed\n");
		return NULL;
	}

	return gate;
}

/*
 * Free the atomic_snapshot_gate.
 */
void atomic_snapshot_destroy_gate(struct atomic_snapshot_gate *gate)
{
	if (gate == NULL)
		return;

	free(gate);
}

void *atomic_snapshot_get_object(struct atomic_snapshot *snapshot)
{
	if (snapshot == NULL)
		return NULL;
	return snapshot->object;
}

void atomic_snapshot_set_object(struct atomic_snapshot *snapshot, void *obj)
{
	if (snapshot == NULL)
		return;
	snapshot->object = obj;
}

struct atomic_snapshot *atomic_snapshot_acquire(
	struct atomic_snapshot_gate *gate)
{
	uint64_t outer;

	if (gate == NULL)
		return NULL;

	outer = atomic_fetch_add(&gate->outer_refcnt_and_ptr, OUTER_REF_CNT);
	return (struct atomic_snapshot *)GET_OUTER_PTR(outer);
}

int atomic_snapshot_release(struct atomic_snapshot *snapshot)
{
	int64_t inner_refcnt
		= atomic_fetch_add(&snapshot->inner_refcnt, 1) + 1;

	if (inner_refcnt == 0) {
		return ATOMIC_SNAPSHOT_FREE_SAFE;
	}

	return ATOMIC_SNAPSHOT_FREE_UNSAFE;
}

struct atomic_snapshot *atomic_snapshot_test_and_set(
	struct atomic_snapshot_gate *gate, struct atomic_snapshot *snapshot,
	int *old_snapshot_status)
{
	uint64_t old_outer, old_outer_refcnt;
	struct atomic_snapshot *old_snapshot;
	int64_t old_inner_refcnt;

	old_outer = atomic_exchange(&gate->outer_refcnt_and_ptr, snapshot);
	old_outer_refcnt = GET_OUTER_REFCNT(old_outer);
	old_snapshot = (struct atomic_snapshot *)GET_OUTER_PTR(old_outer);
	
	old_inner_refcnt = atomic_fetch_sub(&prev_snapshot->inner_refcnt,
		old_outer_refcnt) + old_outer_refcnt;
	if (old_inner_refcnt == 0) {
		*old_snapshot_status = ATOMIC_SNAPSHOT_FREE_SAFE;
	} else {
		*old_snapshot_status = ATOMIC_SNAPSHOT_FREE_UNSAFE:
	}

	return old_snapshot;
}

bool atoimc_snapshot_compare_and_exchange(struct atomic_snapshot_gate *gate,
	struct atomic_snapshot *old_snapshot, struct atomic_snapshot *new_snapshot,
	int *old_snapshot_status)
{
	uint64_t old_outer, old_outer_refcnt;
	int64_t old_inner_refcnt;

	old_outer = atomic_load(&gate->outer_refcnt_and_ptr);

	if (old_snapshot != GET_OUTER_PTR(old_outer))
		return false;

	if (!atomic_compare_exchange_weak(&gate->outer_refcnt_and_ptr,
			&old_outer, new_snapshot))
		return false;

	old_outer_refcnt = GET_OUTER_REFCNT(old_outer);
	old_inner_refcnt = atomic_fetch_sub(&old_snapshot->inner_refcnt,
		old_outer_refcnt) + old_outer_refcnt;

	if (old_inner_refcnt == 0) {
		*old_snapshot_status = ATOMIC_SNAPSHOT_FREE_SAFE;
	} else {
		*old_snapshot_status = ATOMIC_SNAPSHOT_FREE_UNSAFE:
	}

	return true;
}
