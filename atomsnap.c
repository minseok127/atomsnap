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

struct atomsnap {
	_Atomic int64_t inner_refcnt;
	void *object;
};

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

void *atomsnap_get_object(struct atomsnap *snapshot)
{
	if (snapshot == NULL)
		return NULL;
	return snapshot->object;
}

void atomsnap_set_object(struct atomsnap *snapshot, void *obj)
{
	if (snapshot == NULL)
		return;
	snapshot->object = obj;
}

struct atomsnap *atomsnap_acquire(
	struct atomsnap_gate *gate)
{
	uint64_t outer;

	if (gate == NULL)
		return NULL;

	outer = atomic_fetch_add(&gate->outer_refcnt_and_ptr, OUTER_REF_CNT);
	return (struct atomsnap *)GET_OUTER_PTR(outer);
}

int atomsnap_release(struct atomsnap *snapshot)
{
	int64_t inner_refcnt
		= atomic_fetch_add(&snapshot->inner_refcnt, 1) + 1;

	if (inner_refcnt == 0) {
		return ATOMIC_SNAPSHOT_FREE_SAFE;
	}

	return ATOMIC_SNAPSHOT_FREE_UNSAFE;
}

struct atomsnap *atomsnap_test_and_set(
	struct atomsnap_gate *gate, struct atomsnap *snapshot,
	int *old_snapshot_status)
{
	uint64_t old_outer, old_outer_refcnt;
	struct atomsnap *old_snapshot;
	int64_t old_inner_refcnt;

	old_outer = atomic_exchange(&gate->outer_refcnt_and_ptr, 
		(uint64_t)snapshot);
	old_outer_refcnt = GET_OUTER_REFCNT(old_outer);
	old_snapshot = (struct atomsnap *)GET_OUTER_PTR(old_outer);
	
	old_inner_refcnt = atomic_fetch_sub(&old_snapshot->inner_refcnt,
		old_outer_refcnt) + old_outer_refcnt;
	if (old_inner_refcnt == 0) {
		*old_snapshot_status = ATOMIC_SNAPSHOT_FREE_SAFE;
	} else {
		*old_snapshot_status = ATOMIC_SNAPSHOT_FREE_UNSAFE;
	}

	return old_snapshot;
}

bool atoimc_snapshot_compare_and_exchange(struct atomsnap_gate *gate,
	struct atomsnap *old_snapshot, struct atomsnap *new_snapshot,
	int *old_snapshot_status)
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
		old_outer_refcnt) + old_outer_refcnt;

	if (old_inner_refcnt == 0) {
		*old_snapshot_status = ATOMIC_SNAPSHOT_FREE_SAFE;
	} else {
		*old_snapshot_status = ATOMIC_SNAPSHOT_FREE_UNSAFE;
	}

	return true;
}
