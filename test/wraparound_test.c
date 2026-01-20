#define _GNU_SOURCE
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

/*
 * IMPORTANT:
 * Include the implementation directly so the test can access internal
 * structs and fields (e.g., inner_state) without changing the public API.
 *
 * This is intentional for wrap-around edge-case testing.
 */
#include "../atomsnap.c"

static _Atomic(uint64_t) g_free_calls;

static void test_free_impl(void *obj, void *ctx)
{
	(void)ctx;

	if (obj != NULL) {
		free(obj);
	}

	atomic_fetch_add_explicit(&g_free_calls, 1,
		memory_order_relaxed);
}

static struct atomsnap_gate *make_gate(void)
{
	struct atomsnap_init_context ictx;

	memset(&ictx, 0, sizeof(ictx));
	ictx.free_impl = test_free_impl;
	ictx.num_extra_control_blocks = 0;

	return atomsnap_init_gate(&ictx);
}

static struct atomsnap_version *make_ver(struct atomsnap_gate *g, int v)
{
	struct atomsnap_version *ver;
	int *p;

	ver = atomsnap_make_version(g);
	assert(ver != NULL);

	p = malloc(sizeof(*p));
	assert(p != NULL);
	*p = v;

	atomsnap_set_object(ver, p, NULL);
	return ver;
}

/*
 * Test 1:
 * Even if the inner counter becomes 0 due to wrap-around, a version must
 * not be reclaimed unless it is detached.
 */
static void test_no_detach_no_free_on_wrap(void)
{
	struct atomsnap_gate *g;
	struct atomsnap_version *ver, *r;
	uint64_t before, after;
	uint64_t s;

	fprintf(stderr, "[TEST] no-detach no-free on wrap\n");

	atomic_store_explicit(&g_free_calls, 0, memory_order_relaxed);

	g = make_gate();
	assert(g != NULL);

	ver = make_ver(g, 123);
	atomsnap_exchange_version_slot(g, 0, ver);

	r = atomsnap_acquire_version_slot(g, 0);
	assert(r == ver);

	/*
	 * Force the inner counter to UINT32_MAX. DETACHED is 0.
	 * A single release will wrap the counter to 0.
	 */
	s = ((uint64_t)0xFFFFFFFFu << INNER_CNT_SHIFT) | 0u;
	atomic_store_explicit(&ver->inner_state, s, memory_order_relaxed);

	before = atomic_load_explicit(&g_free_calls, memory_order_relaxed);
	atomsnap_release_version(r);
	after = atomic_load_explicit(&g_free_calls, memory_order_relaxed);

	assert(before == after);

	/*
	 * Ensure the version is still present and can be acquired.
	 */
	r = atomsnap_acquire_version_slot(g, 0);
	assert(r == ver);
	atomsnap_release_version(r);

	/*
	 * Detach. This should make the old version reclaimable when the
	 * counter reaches 0 under DETACHED.
	 */
	atomsnap_exchange_version_slot(g, 0, NULL);

	atomsnap_destroy_gate(g);
}

/*
 * Test 2:
 * If detached and the counter becomes 0, reclamation must happen once.
 *
 * We set counter to 0xFFFFFFFF and DETACHED=1.
 * A single release increments counter to 0 and should finalize/free.
 */
static void test_detach_finalize_once(void)
{
	struct atomsnap_gate *g;
	struct atomsnap_version *ver;
	struct atomsnap_version *r;
	uint64_t s;
	uint64_t after;

	fprintf(stderr, "[TEST] detach finalize once\n");

	atomic_store_explicit(&g_free_calls, 0, memory_order_relaxed);

	g = make_gate();
	assert(g != NULL);

	ver = make_ver(g, 7);
	atomsnap_exchange_version_slot(g, 0, ver);

	r = atomsnap_acquire_version_slot(g, 0);
	assert(r == ver);

	/*
	 * Force: counter=UINT32_MAX, DETACHED=1, FINALIZED=0.
	 * Then one release should reclaim.
	 */
	s = ((uint64_t)0xFFFFFFFFu << INNER_CNT_SHIFT) |
		(uint64_t)INNER_F_DETACHED;
	atomic_store_explicit(&ver->inner_state, s, memory_order_relaxed);

	atomsnap_release_version(r);

	after = atomic_load_explicit(&g_free_calls, memory_order_relaxed);
	assert(after == 1);

	/*
	 * Clear the gate slot. The old version is already reclaimed.
	 */
	atomsnap_exchange_version_slot(g, 0, NULL);

	atomsnap_destroy_gate(g);
}

struct stress_args {
	struct atomsnap_gate *gate;
	_Atomic(bool) stop;
	_Atomic(uint64_t) reader_ops;
	_Atomic(uint64_t) writer_ops;
};

static void *reader_thread(void *arg)
{
	struct stress_args *a = arg;
	struct atomsnap_version *v;
	int *p;

	while (!atomic_load_explicit(&a->stop, memory_order_relaxed)) {
		v = atomsnap_acquire_version_slot(a->gate, 0);
		if (v != NULL) {
			p = atomsnap_get_object(v);
			if (p != NULL) {
				(void)*p;
			}
			atomsnap_release_version(v);
		}
		atomic_fetch_add_explicit(&a->reader_ops, 1,
			memory_order_relaxed);
	}

	return NULL;
}

static void *writer_thread(void *arg)
{
	struct stress_args *a = arg;
	struct atomsnap_version *v;
	uint64_t i;

	for (i = 0; i < 200000; i++) {
		v = make_ver(a->gate, (int)i);
		atomsnap_exchange_version_slot(a->gate, 0, v);
		atomic_fetch_add_explicit(&a->writer_ops, 1,
			memory_order_relaxed);
	}

	atomic_store_explicit(&a->stop, true, memory_order_relaxed);
	return NULL;
}

/*
 * Test 3 (stress):
 * Multiple readers acquire/release while a writer swaps versions.
 * This should not crash or double-free.
 */
static void test_stress(void)
{
	struct stress_args a;
	pthread_t wr;
	pthread_t rd[4];
	int i;
	uint64_t frees;
	uint64_t wops;

	fprintf(stderr, "[TEST] stress\n");

	memset(&a, 0, sizeof(a));

	atomic_store_explicit(&g_free_calls, 0, memory_order_relaxed);

	a.gate = make_gate();
	assert(a.gate != NULL);

	atomic_store_explicit(&a.stop, false, memory_order_relaxed);

	for (i = 0; i < 4; i++) {
		assert(pthread_create(&rd[i], NULL, reader_thread,
			&a) == 0);
	}

	assert(pthread_create(&wr, NULL, writer_thread, &a) == 0);

	assert(pthread_join(wr, NULL) == 0);

	for (i = 0; i < 4; i++) {
		assert(pthread_join(rd[i], NULL) == 0);
	}

	/*
	 * Detach the final version.
	 */
	atomsnap_exchange_version_slot(a.gate, 0, NULL);

	frees = atomic_load_explicit(&g_free_calls, memory_order_relaxed);
	wops = atomic_load_explicit(&a.writer_ops, memory_order_relaxed);

	fprintf(stderr, "writer_ops=%" PRIu64 " free_calls=%" PRIu64 "\n",
		wops, frees);

	/*
	 * frees should be close to writer swaps, but reclamation may lag.
	 * We only assert basic sanity.
	 */
	assert(frees <= wops + 10);

	atomsnap_destroy_gate(a.gate);
}

int main(void)
{
	test_no_detach_no_free_on_wrap();
	test_detach_finalize_once();
	test_stress();

	fprintf(stderr, "ALL TESTS PASSED\n");
	return 0;
}

