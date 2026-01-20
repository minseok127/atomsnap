/**
 * @file    atomsnap.c
 * @brief   Implementation of the atomsnap library.
 *
 * This file implements a lock-free mechanism for managing shared
 * objects with multiple versions using a handle-based approach.
 *
 * Design Overview:
 * - Handles: 32-bit integers composed of (Arena ID | Slot ID).
 * - Control Block: 64-bit atomic containing [32-bit RefCount | 32-bit Handle].
 * - Memory: Global arena table with thread-local vector caching.
 *
 * Reference Counting Logic:
 *
 * When a reader wants to access the current version, it atomically increments
 * the outer reference counter using fetch_add(). The returned 64-bit value has
 * its lower 32-bits representing the pointer of the version whose reference
 * count was increased.
 *
 * After finishing the use of the version, the reader must release it.
 * During release, the reader increments the inner counter.
 * If the resulting counter is 0 and the version is detached, it can be freed.
 *
 * The design uses 32-bit counters for both Outer and Inner counts.
 * We use uint32_t to rely on defined unsigned integer wrap-around behavior.
 *
 * Free List Logic (Stack Based):
 *
 * The free list management uses a "Lock-Free MPSC Stack" approach.
 *
 * - Tagged Pointers: The upper 32 bits of the 64-bit top_handle store the
 * current stack depth. The lower 32 bits store the handle.
 *
 * - Producers (Free): Use a CAS loop to push node onto the arena's
 * 'top_handle'.
 *
 * - Consumer (Alloc): Uses 'atomic_exchange' to detach the entire stack from
 * 'top_handle' (Batch Steal).
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <pthread.h>
#include <assert.h>
#include <inttypes.h>
#include <stdbool.h>
#include <sys/mman.h>

#include "atomsnap.h"

#define PAGE_SIZE             (4096)

/*
 * MAX_THREADS: 1,048,576 (2^20)
 * Kept for global thread ID and context management.
 */
#define MAX_THREADS           (1048576)

/*
 * MAX_ARENAS: 1,048,576 (2^20)
 * Corresponds to the 20-bit Arena Index.
 */
#define MAX_ARENAS            (1048576)

/*
 * SLOTS_PER_ARENA: 3,276
 *
 * We want the memory_arena structure to align perfectly with the page
 * boundaries (32 pages = 131,072 bytes).
 *
 * - atomsnap_version size: 40 bytes
 * - memory_arena header:   8 bytes (top_handle)
 *
 * Size = 8 + (3,276 * 40) = 131,048 bytes.
 * Remaining space = 24 bytes.
 *
 * Indices: 0 to 3275.
 * Slot 0 is Sentinel. Slots 1..3275 are usable.
 */
#define SLOTS_PER_ARENA       (3276)

/* Bit layout for the 32-bit handle */
#define HANDLE_SLOT_BITS      (12)
#define HANDLE_ARENA_BITS     (20)

/* Special Values */
#define HANDLE_NULL           (0xFFFFFFFF) /* 32-bit of 1s */

/*
 * Handle Masking & Tagging (for 64-bit top_handle)
 * [ Depth (32) | Handle (32) ]
 */
#define HANDLE_MASK_32        (0x00000000FFFFFFFFULL)
#define STACK_DEPTH_SHIFT     (32)
#define STACK_DEPTH_MASK      (0xFFFFFFFF00000000ULL)
#define STACK_DEPTH_INC       (1ULL << STACK_DEPTH_SHIFT)

/*
 * Control Block (64-bit)
 * Layout: [ 32-bit RefCount | 32-bit Handle ]
 */
#define REF_COUNT_SHIFT       (32)
#define REF_COUNT_INC         (1ULL << REF_COUNT_SHIFT)
#define REF_COUNT_MASK        (0xFFFFFFFF00000000ULL)
#define HANDLE_MASK_64        (0x00000000FFFFFFFFULL)

/*
 * Inner State (64-bit)
 * Layout: [ 32-bit Counter | 32-bit Flags ]
 *
 * - Upper 32 bits: counter (uint32_t, wraps naturally)
 * - Lower 32 bits: flags (DETACHED / FINALIZED)
 *
 * Readers increment the counter only by adding (1ULL << 32), so flags are
 * never modified by carry/overflow.
 */
#define INNER_CNT_SHIFT       (32)
#define INNER_CNT_INC         (1ULL << INNER_CNT_SHIFT)
#define INNER_FLAGS_MASK      (0x00000000FFFFFFFFULL)

#define INNER_F_DETACHED      (1u << 0)
#define INNER_F_FINALIZED     (1u << 1)

/* Error logging macro */
#define errmsg(fmt, ...) \
	fprintf(stderr, "[atomsnap:%d:%s] " fmt, __LINE__, __func__, ##__VA_ARGS__)

/*
 * 32-bit Handle Union for easier encoding/decoding.
 */
typedef union {
	struct {
		uint32_t slot_idx   : HANDLE_SLOT_BITS;
		uint32_t arena_idx  : HANDLE_ARENA_BITS;
	};
	uint32_t raw;
} atomsnap_handle_t;

static inline uint32_t inner_cnt(uint64_t s)
{
	return (uint32_t)(s >> INNER_CNT_SHIFT);
}

static inline uint32_t inner_flags(uint64_t s)
{
	return (uint32_t)(s & INNER_FLAGS_MASK);
}

/*
 * atomsnap_version - Internal representation of a version.
 *
 * This structure is allocated within memory arenas. It contains both the
 * user-facing payload fields and internal management fields.
 *
 * @object:        Public-facing pointer to the user object.
 * @free_context:  User-defined context for the free function.
 * @gate:          Pointer to the gate this version belongs to.
 * @inner_state:   [32-bit Counter | 32-bit Flags] for reclamation.
 * @self_handle:   Handle identifying this version (when allocated).
 * @next_handle:   Handle to the next node in the stack (when freed).
 *
 * [ Memory Layout ]
 * 00-08: object (8B)
 * 08-16: free_context (8B)
 * 16-24: gate (8B)
 * 24-32: inner_state (8B)
 * 32-36: self_handle / next_handle (4B)
 * 36-40: padding (4B)
 */
struct atomsnap_version {
	_Atomic(void *) object;
	void *free_context;
	struct atomsnap_gate *gate;
	_Atomic(uint64_t) inner_state;
	union {
		uint32_t self_handle;
		_Atomic(uint32_t) next_handle;
	};
};

/*
 * memory_arena - Contiguous block of version slots.
 *
 * @top_handle: Handle of the top node in the shared stack.
 * @slots:      Array of version structures. Slot 0 is the Sentinel.
 */
struct memory_arena {
	_Atomic(uint64_t) top_handle;
	struct atomsnap_version slots[SLOTS_PER_ARENA];
};

/*
 * thread_context - Thread-Local Storage (TLS) context.
 *
 * @thread_id:          Assigned global thread ID.
 * @owned_arenas:       Dynamic array of pointers to owned arenas.
 * @arena_indices:      Dynamic array of indices for owned arenas.
 * @active_arena_count: Index of the arena currently being allocated from.
 * @vector_capacity:    Current allocated capacity of the dynamic arrays.
 * @local_top:          Top of the local free stack.
 * @alloc_count:        Allocation counter to trigger periodic reclamation.
 */
struct thread_context {
	int thread_id;
	struct memory_arena **owned_arenas;
	uint32_t *arena_indices;
	size_t active_arena_count;
	size_t vector_capacity;
	uint32_t local_top;
	uint64_t alloc_count;
};

/*
 * atomsnap_gate - Gate structure.
 *
 * @control_block:        64-bit atomic [RefCnt | Handle].
 * @free_impl:            User callback for object cleanup.
 * @extra_control_blocks: Array for multi-slot gates.
 * @num_extra_slots:      Number of extra slots.
 */
struct atomsnap_gate {
	_Atomic(uint64_t) control_block;
	atomsnap_free_func free_impl;
	_Atomic(uint64_t) *extra_control_blocks;
	int num_extra_slots;
};

/*
 * Global Variables
 */
static struct memory_arena *g_arena_table[MAX_ARENAS];
static _Atomic(size_t) g_global_arena_cnt = 0;

static struct thread_context *g_thread_contexts[MAX_THREADS];
static _Atomic(bool) g_tid_used[MAX_THREADS];

static pthread_key_t g_tls_key;
static pthread_once_t g_init_once = PTHREAD_ONCE_INIT;

/*
 * Forward Declarations
 */
static int atomsnap_thread_init_internal(void);

/**
 * @brief   Convert a raw handle to a version pointer.
 *
 * @param   handle_raw: The 32-bit handle.
 *
 * @return  Pointer to the atomsnap_version, or NULL if invalid.
 */
static inline struct atomsnap_version *resolve_handle(uint32_t handle_raw)
{
	atomsnap_handle_t h;
	struct memory_arena *arena;

	if (__builtin_expect(handle_raw == HANDLE_NULL, 0)) {
		return NULL;
	}

	h.raw = handle_raw;

	/* Bounds check */
	if (__builtin_expect(h.arena_idx >= MAX_ARENAS, 0)) {
		return NULL;
	}

	arena = g_arena_table[h.arena_idx];

	if (__builtin_expect(arena == NULL, 0)) {
		return NULL;
	}

	return &arena->slots[h.slot_idx];
}

/**
 * @brief   Construct a handle from indices.
 *
 * @param   aid: Arena ID.
 * @param   sid: Slot ID.
 *
 * @return  Combined 32-bit handle.
 */
static inline uint32_t construct_handle(int aid, int sid)
{
	atomsnap_handle_t h;

	h.raw = 0;
	h.arena_idx = aid;
	h.slot_idx = sid;
	return h.raw;
}

/**
 * @brief   Check and reclaim the last active arena if it is fully free.
 *
 * This function examines the last arena in the thread's active list.
 * If all slots in that arena have been returned (based on stack depth),
 * the arena is returned to the OS via madvise(), and the active arena count
 * is decremented. The arena remains in the owned_arenas vector for reuse.
 *
 * @param   ctx: Thread context.
 *
 * @return  true if an arena was reclaimed, false otherwise.
 */
static bool reclaim_last_arena_if_empty(struct thread_context *ctx)
{
	struct memory_arena *arena;
	uint64_t curr_top, depth;
	size_t idx;

	if (ctx->active_arena_count == 0) {
		return false;
	}

	idx = ctx->active_arena_count - 1;
	arena = ctx->owned_arenas[idx];

	/* Read top handle to check utilization */
	curr_top = atomic_load(&arena->top_handle);
	depth = (curr_top & STACK_DEPTH_MASK) >> STACK_DEPTH_SHIFT;

	/*
	 * If depth equals (SLOTS - 1), it means all slots (1..N)
	 * have been returned to the arena's stack.
	 */
	if (depth == (SLOTS_PER_ARENA - 1)) {
		madvise(arena, sizeof(struct memory_arena), MADV_DONTNEED);
		ctx->active_arena_count--;
		return true;
	}

	return false;
}

/**
 * @brief   TLS destructor called when a thread exits.
 *
 * Reclaims all fully free arenas from the end of the active list
 * and releases the thread ID.
 *
 * @param   arg: Pointer to the thread_context.
 */
static void tls_destructor(void *arg)
{
	struct thread_context *ctx = (struct thread_context *)arg;

	if (ctx) {
		/*
		 * Attempt to reclaim unused arenas from the end of the active
		 * list. We loop until we hit a busy arena or run out of arenas.
		 */
		while (ctx->active_arena_count > 0) {
			if (!reclaim_last_arena_if_empty(ctx)) {
				/* Found a busy arena, stop reclamation */
				break;
			}
		}

		/*
		 * Release the Thread ID atomically so other threads can adopt
		 * this ctx.
		 */
		atomic_store(&g_tid_used[ctx->thread_id], false);
	}
}

/**
 * @brief   One-time global initialization routine.
 */
static void global_init_routine(void)
{
	if (pthread_key_create(&g_tls_key, tls_destructor) != 0) {
		errmsg("Failed to create pthread key\n");
		exit(EXIT_FAILURE);
	}
	memset(g_arena_table, 0, sizeof(g_arena_table));
	memset(g_thread_contexts, 0, sizeof(g_thread_contexts));
	memset(g_tid_used, 0, sizeof(g_tid_used));
}

/**
 * @brief   Ensure the current thread is registered.
 *
 * Checks for TLS context. If not present, attempts lazy initialization
 * via atomsnap_thread_init_internal().
 *
 * @return  Pointer to the thread_context, or NULL on failure.
 */
static inline struct thread_context *get_or_init_thread_context(void)
{
	struct thread_context *ctx = NULL;

	pthread_once(&g_init_once, global_init_routine);

	ctx = (struct thread_context *)pthread_getspecific(g_tls_key);

	if (__builtin_expect(ctx == NULL, 0)) {
		if (atomsnap_thread_init_internal() != 0) {
			return NULL;
		}
		ctx = (struct thread_context *)pthread_getspecific(g_tls_key);
	}
	return ctx;
}

/**
 * @brief   Ensure the thread-local vector has enough capacity.
 *
 * @param   ctx: Thread context.
 *
 * @return  0 on success, -1 on failure.
 */
static int ensure_vector_capacity(struct thread_context *ctx)
{
	size_t new_cap;
	struct memory_arena **new_arenas;
	uint32_t *new_indices;
	size_t k;

	if (ctx->active_arena_count < ctx->vector_capacity) {
		return 0;
	}

	new_cap = ctx->vector_capacity == 0 ? 4 : ctx->vector_capacity * 2;

	new_arenas = realloc(ctx->owned_arenas,
		new_cap * sizeof(struct memory_arena *));
	new_indices = realloc(ctx->arena_indices,
		new_cap * sizeof(uint32_t));

	if (!new_arenas || !new_indices) {
		errmsg("Failed to expand arena vector\n");
		return -1;
	}

	ctx->owned_arenas = new_arenas;
	ctx->arena_indices = new_indices;

	/* Initialize new slots to NULL */
	for (k = ctx->vector_capacity; k < new_cap; k++) {
		ctx->owned_arenas[k] = NULL;
	}

	ctx->vector_capacity = new_cap;
	return 0;
}

/**
 * @brief   Initialize links and stack for a newly allocated/reused arena.
 *
 * @param   arena:      Pointer to the arena.
 * @param   arena_idx:  Global index of the arena.
 *
 * @return  Handle to the top of the stack (first valid slot).
 */
static uint32_t setup_arena_stack(struct memory_arena *arena, size_t arena_idx)
{
	uint32_t sentinel_handle, curr, next_in_stack;
	struct atomsnap_version *slot;
	int i;

	/* Setup Sentinel (Slot 0) */
	sentinel_handle = construct_handle(arena_idx, 0);

	/* Sentinel points to NULL */
	atomic_store(&arena->slots[0].next_handle, HANDLE_NULL);

	/* Arena Top initially points to Sentinel, Depth 0 */
	atomic_store(&arena->top_handle, (uint64_t)sentinel_handle);

	/*
	 * Link slots 1..N sequentially to form the free list stack.
	 */
	next_in_stack = sentinel_handle;

	for (i = 1; i < SLOTS_PER_ARENA; i++) {
		curr = construct_handle(arena_idx, i);
		slot = &arena->slots[i];
		slot->self_handle = curr;

		atomic_store(&slot->next_handle, next_in_stack);
		next_in_stack = curr;
	}

	return next_in_stack;
}

/**
 * @brief   Initialize a new arena (or reuse a reclaimed one).
 *
 * @param   ctx: Thread context.
 *
 * @return  0 on success, -1 on failure.
 */
static int init_arena(struct thread_context *ctx)
{
	struct memory_arena *arena;
	size_t arena_idx;
	uint32_t next_in_stack;

	/*
	 * Check if we have an existing pointer in the vector beyond the
	 * active_arena_count (which means it was reclaimed).
	 */
	if (ctx->active_arena_count < ctx->vector_capacity &&
		ctx->owned_arenas[ctx->active_arena_count] != NULL) {
		/* Reuse existing arena slot */
		arena = ctx->owned_arenas[ctx->active_arena_count];
		arena_idx = ctx->arena_indices[ctx->active_arena_count];
	} else {
		/* Allocate New Global Arena */
		arena_idx = atomic_fetch_add(&g_global_arena_cnt, 1);
		if (arena_idx >= MAX_ARENAS) {
			errmsg("Max arenas reached\n");
			return -1;
		}

		arena = aligned_alloc(PAGE_SIZE, sizeof(struct memory_arena));
		if (!arena) {
			errmsg("Memory allocation failed for new arena\n");
			return -1;
		}
		memset(arena, 0, sizeof(struct memory_arena));

		/* Register in global table */
		g_arena_table[arena_idx] = arena;

		/* Ensure vector capacity */
		if (ensure_vector_capacity(ctx) != 0) {
			return -1;
		}

		ctx->owned_arenas[ctx->active_arena_count] = arena;
		ctx->arena_indices[ctx->active_arena_count] =
			(uint32_t)arena_idx;
	}

	/* Setup Stack and Links */
	next_in_stack = setup_arena_stack(arena, arena_idx);

	/* Increment active count */
	ctx->active_arena_count++;

	/* Use the new stack */
	ctx->local_top = next_in_stack;

	return 0;
}

/**
 * @brief   Pop a slot from the local free list (Stack Pop).
 *
 * @param   ctx: Thread context.
 *
 * @return  Handle of the allocated slot, or HANDLE_NULL if empty.
 */
static uint32_t pop_local(struct thread_context *ctx)
{
	uint32_t handle_raw;
	struct atomsnap_version *slot;
	atomsnap_handle_t h;

	if (ctx->local_top == HANDLE_NULL) {
		return HANDLE_NULL;
	}

	handle_raw = ctx->local_top;
	h.raw = handle_raw;

	/* Check if the top is the Sentinel (Slot 0) */
	if (h.slot_idx == 0) {
		/* Stack is empty (hit sentinel) */
		ctx->local_top = HANDLE_NULL;
		return HANDLE_NULL;
	}

	slot = resolve_handle(handle_raw);

	/*
	 * Move top to the next node down the stack.
	 */
	ctx->local_top = atomic_load(&slot->next_handle);

	/* Restore self_handle for Allocated state */
	slot->self_handle = h.raw;
	return h.raw;
}

/**
 * @brief   Allocates a slot handle.
 *
 * Strategy:
 * 1. Try Local Stack (pop_local).
 * 2. Try Batch Steal from Arenas (atomic_exchange).
 * 3. Init New Arena (or reuse).
 *
 * @param   ctx: Thread context.
 *
 * @return  Handle of the allocated slot, or HANDLE_NULL on failure.
 */
static uint64_t alloc_slot(struct thread_context *ctx)
{
	uint32_t handle, sentinel_handle;
	struct memory_arena *arena;
	uint64_t top_val, batch_top;
	size_t i;

	ctx->alloc_count++;

	/*
	 * Periodic Reclamation Check.
	 * Check if the last active arena is fully free.
	 */
	if ((ctx->alloc_count % SLOTS_PER_ARENA) == 0) {
		reclaim_last_arena_if_empty(ctx);
	}

	/* 1. Try Local Free Stack */
	handle = pop_local(ctx);
	if (handle != HANDLE_NULL) {
		return handle;
	}

	/* 2. Try Batch Steal from owned active arenas */
	for (i = 0; i < ctx->active_arena_count; i++) {
		arena = ctx->owned_arenas[i];
		sentinel_handle = construct_handle(ctx->arena_indices[i], 0);

		/* Check if empty (optimization) */
		top_val = atomic_load(&arena->top_handle);
		if ((uint32_t)(top_val & HANDLE_MASK_32) == sentinel_handle) {
			continue;
		}

		/*
		 * Batch Steal: Atomically exchange Top with Sentinel.
		 * This detaches the entire stack.
		 */
		batch_top = atomic_exchange(&arena->top_handle,
			(uint64_t)sentinel_handle);

		assert((uint32_t)(batch_top & HANDLE_MASK_32) !=
			sentinel_handle);

		/* Adopt the batch */
		ctx->local_top = (uint32_t)(batch_top & HANDLE_MASK_32);

		return pop_local(ctx);
	}

	/* 3. Allocate New Arena (or reuse inactive) */
	if (init_arena(ctx) == 0) {
		return pop_local(ctx);
	}

	errmsg("Out of memory (Max arenas reached)\n");
	return HANDLE_NULL;
}

/**
 * @brief   Returns a slot to its arena (Stack Push).
 *
 * Uses a CAS loop to push the slot onto the top of the arena's stack.
 *
 * @param   slot: Pointer to the version slot to free.
 */
static void free_slot(struct atomsnap_version *slot)
{
	uint32_t my_handle = slot->self_handle;
	atomsnap_handle_t h = { .raw = my_handle };
	struct memory_arena *arena = g_arena_table[h.arena_idx];
	uint64_t old_top, new_top, depth;

	old_top = atomic_load(&arena->top_handle);
	do {
		/* 1. Extract current stack depth */
		depth = (old_top & STACK_DEPTH_MASK);

		/* 2. Increment depth */
		depth += STACK_DEPTH_INC;

		/* 3. Construct new top handle: [ New Depth | My Handle ] */
		new_top = depth | (uint64_t)my_handle;

		/* Link: Me -> Old Top (Extract 32-bit handle) */
		atomic_store(&slot->next_handle,
			(uint32_t)(old_top & HANDLE_MASK_32));

		/* Attempt to make Me the New Top */
	} while (!atomic_compare_exchange_weak(&arena->top_handle,
		&old_top, new_top));
}

/*
 * Finalize and return the slot to the arena.
 */
static inline void finalize_and_free(struct atomsnap_version *ver)
{
	void *obj;

	obj = atomic_load_explicit(&ver->object, memory_order_relaxed);

	if (ver->gate && ver->gate->free_impl) {
		ver->gate->free_impl(obj, ver->free_context);
	}

	free_slot(ver);
}

/*
 * Attempt to finalize a detached version when the counter reaches zero.
 *
 * A thread claims FINALIZED before reclaiming. If FINALIZED is already
 * set, another thread reclaimed it.
 *
 * Define ATOMSNAP_DISABLE_FINALIZE_CHECK to ignore duplicates.
 */
static inline void try_finalize(struct atomsnap_version *ver, uint64_t state)
{
	uint32_t flags;
	uint64_t old;

	flags = inner_flags(state);
	if (!(flags & INNER_F_DETACHED)) {
		return;
	}

	if (inner_cnt(state) != 0) {
		return;
	}

	old = atomic_fetch_or_explicit(&ver->inner_state,
		(uint64_t)INNER_F_FINALIZED, memory_order_acq_rel);

	if ((uint32_t)old & INNER_F_FINALIZED) {
		errmsg("Double finalize detected\n");
		abort();
	}

	finalize_and_free(ver);
}

/*
 * Atomically detach and subtract outer refs from the inner counter.
 *
 * The DETACHED flag and counter adjustment must be committed together to
 * avoid intermediate states that could lead to premature reclamation.
 */
static inline void detach_and_adjust(struct atomsnap_version *ver,
	uint32_t old_refs)
{
	uint64_t cur, next;
	uint32_t cnt, flags;
	uint32_t new_cnt, new_flags;

	cur = atomic_load_explicit(&ver->inner_state, memory_order_acquire);

	for (;;) {
		cnt = inner_cnt(cur);
		flags = inner_flags(cur);

		new_cnt = (uint32_t)(cnt - old_refs);
		new_flags = flags | INNER_F_DETACHED;

		next = ((uint64_t)new_cnt << INNER_CNT_SHIFT) |
			(uint64_t)new_flags;

		if (atomic_compare_exchange_weak_explicit(&ver->inner_state,
				&cur, next, memory_order_acq_rel,
				memory_order_acquire)) {
			break;
		}
	}

	try_finalize(ver, next);
}

/**
 * @brief   Explicitly initialize the atomsnap library globals.
 *
 * Optional; usually called lazily.
 */
int atomsnap_global_init(void)
{
	pthread_once(&g_init_once, global_init_routine);
	return 0;
}

/**
 * @brief   Internal thread initialization.
 *
 * Acquires a global thread ID and allocates/adopts a thread context.
 *
 * @return  0 on success, -1 on failure.
 */
static int atomsnap_thread_init_internal(void)
{
	struct thread_context *ctx;
	bool expected = false;
	int tid = -1;
	int i;

	/* 1. Acquire Thread ID using CAS */
	for (i = 0; i < MAX_THREADS; i++) {
		if (atomic_load(&g_tid_used[i]) == true) {
			continue;
		}

		expected = false;
		if (atomic_compare_exchange_strong(&g_tid_used[i],
			&expected, true)) {
			tid = i;
			break;
		}
	}

	if (tid == -1) {
		errmsg("Max threads limit reached (%d)\n", MAX_THREADS);
		return -1;
	}

	/* 2. Adoption or New Allocation */
	ctx = g_thread_contexts[tid];
	if (ctx == NULL) {
		/* New Allocation */
		ctx = calloc(1, sizeof(struct thread_context));
		if (ctx == NULL) {
			errmsg("Failed to allocate thread context\n");
			atomic_store(&g_tid_used[tid], false);
			return -1;
		}
		ctx->thread_id = tid;
		ctx->active_arena_count = 0;
		ctx->vector_capacity = 0;
		ctx->local_top = HANDLE_NULL;
		g_thread_contexts[tid] = ctx;
	} else {
		/*
		 * Adoption: Reuse existing context and arenas.
		 */
	}

	/* 3. Set TLS */
	if (pthread_setspecific(g_tls_key, ctx) != 0) {
		errmsg("Failed to set TLS value\n");
		return -1;
	}

	return 0;
}

/**
 * @brief   Create a new atomsnap_gate.
 *
 * @param   ctx: Initialization context containing callback pointers.
 *
 * @return  Pointer to the new gate, or NULL on failure.
 */
struct atomsnap_gate *atomsnap_init_gate(struct atomsnap_init_context *ctx)
{
	struct atomsnap_gate *gate = calloc(1, sizeof(struct atomsnap_gate));
	int i;

	if (gate == NULL) {
		errmsg("Gate allocation failed\n");
		return NULL;
	}

	gate->free_impl = ctx->free_impl;
	gate->num_extra_slots = ctx->num_extra_control_blocks;

	if (gate->free_impl == NULL) {
		errmsg("Invalid free function\n");
		free(gate);
		return NULL;
	}

	if (gate->num_extra_slots > 0) {
		gate->extra_control_blocks = calloc(gate->num_extra_slots,
			sizeof(_Atomic(uint64_t)));

		if (gate->extra_control_blocks == NULL) {
			errmsg("Extra blocks allocation failed\n");
			free(gate);
			return NULL;
		}

		for (i = 0; i < gate->num_extra_slots; i++) {
			atomic_init(&gate->extra_control_blocks[i],
				(uint64_t)HANDLE_NULL);
		}
	}

	atomic_init(&gate->control_block, (uint64_t)HANDLE_NULL);

	return gate;
}

/**
 * @brief   Destroy the atomsnap_gate.
 *
 * @param   gate: Gate to destroy.
 */
void atomsnap_destroy_gate(struct atomsnap_gate *gate)
{
	if (gate == NULL) {
		return;
	}

	if (gate->extra_control_blocks) {
		free(gate->extra_control_blocks);
	}
	free(gate);
}

/**
 * @brief   Allocate memory for an atomsnap_version.
 *
 * Uses the internal memory allocator (arena) to get a version slot.
 *
 * @param   gate: Gate to associate with the version.
 *
 * @return  Pointer to the new version, or NULL on failure.
 */
struct atomsnap_version *atomsnap_make_version(struct atomsnap_gate *gate)
{
	struct thread_context *ctx = get_or_init_thread_context();
	uint32_t handle;
	struct atomsnap_version *slot;

	if (ctx == NULL) {
		return NULL;
	}

	handle = alloc_slot(ctx);
	if (handle == HANDLE_NULL) {
		return NULL;
	}

	slot = resolve_handle(handle);
	assert(slot != NULL);

	/* Initialize slot */
	slot->object = NULL;
	slot->free_context = NULL;
	slot->gate = gate;

	atomic_store_explicit(&slot->inner_state, 0, memory_order_relaxed);

	return slot;
}

/**
 * @brief   Manually free a version that was created but NEVER exchanged.
 *
 * This function is used when a writer creates a version but decides not to
 * publish it (e.g., CAS failure). It invokes the user-defined free callback
 * to clean up the object, and then returns the version slot to the pool.
 *
 * @param   version: The version to free.
 */
void atomsnap_free_version(struct atomsnap_version *version)
{
	void *obj;

	if (version == NULL) {
		return;
	}

	obj = atomic_load_explicit(&version->object, memory_order_relaxed);

	if (version->gate && version->gate->free_impl) {
		version->gate->free_impl(obj, version->free_context);
	}

	free_slot(version);
}

/**
 * @brief   Set the user object and context for a version.
 *
 * @param   ver:          The version.
 * @param   object:       User object pointer.
 * @param   free_context: User free context.
 */
void atomsnap_set_object(struct atomsnap_version *ver, void *object,
	void *free_context)
{
	if (ver) {
		ver->free_context = free_context;
		atomic_store_explicit(&ver->object, object,
			memory_order_release);
	}
}

/**
 * @brief   Get the user payload object from a version.
 *
 * @param   ver: The version pointer.
 *
 * @return  Pointer to the user object.
 */
void *atomsnap_get_object(const struct atomsnap_version *ver)
{
	if (ver) {
		return atomic_load_explicit(&ver->object,
			memory_order_acquire);
	}
	return NULL;
}

static inline _Atomic(uint64_t) *get_cb_slot(struct atomsnap_gate *gate,
	int idx)
{
	return (idx == 0) ? &gate->control_block :
		&gate->extra_control_blocks[idx - 1];
}

/**
 * @brief   Atomically acquire the current version from a slot.
 *
 * Increments the outer reference count.
 *
 * @param   gate:     Target gate.
 * @param   slot_idx: Control block slot index (0 for default).
 *
 * @return  Pointer to the acquired version.
 */
struct atomsnap_version *atomsnap_acquire_version_slot(
	struct atomsnap_gate *gate, int slot_idx)
{
	_Atomic(uint64_t) *cb = get_cb_slot(gate, slot_idx);
	uint64_t val;
	uint32_t handle;

	/* Increment Reference Count (Upper 32 bits) */
	val = atomic_fetch_add_explicit(cb, REF_COUNT_INC,
		memory_order_acquire);

	handle = (uint32_t)(val & HANDLE_MASK_64);

	return resolve_handle(handle);
}

/**
 * @brief   Release a version previously acquired.
 *
 * Increments the inner counter (upper 32 bits only).
 * If DETACHED is set and the counter becomes 0, the version is reclaimed.
 *
 * @param   ver: Version to release.
 */
void atomsnap_release_version(struct atomsnap_version *ver)
{
	uint64_t prev, now;

	if (ver == NULL) {
		return;
	}

	/*
	 * Readers increment only the counter (upper 32 bits). Flags in the
	 * lower 32 bits are never affected by carry/overflow.
	 */
	prev = atomic_fetch_add_explicit(&ver->inner_state, INNER_CNT_INC,
		memory_order_acq_rel);
	now = prev + INNER_CNT_INC;

	try_finalize(ver, now);
}

/**
 * @brief   Replace the version in the given slot unconditionally.
 *
 * @param   gate:     Target gate.
 * @param   slot_idx: Control block slot index.
 * @param   new_ver:  New version to register.
 */
void atomsnap_exchange_version_slot(struct atomsnap_gate *gate, int slot_idx,
	struct atomsnap_version *new_ver)
{
	uint32_t new_handle = new_ver ? new_ver->self_handle : HANDLE_NULL;
	_Atomic(uint64_t) *cb = get_cb_slot(gate, slot_idx);
	uint64_t old_val;
	uint32_t old_handle, old_refs;
	struct atomsnap_version *old_ver;

	/*
	 * Swap the handle in the control block.
	 * The new value will have 'new_handle' and 'RefCount = 0' (implicitly).
	 */
	old_val = atomic_exchange_explicit(cb, (uint64_t)new_handle,
		memory_order_acq_rel);

	old_handle = (uint32_t)(old_val & HANDLE_MASK_64);
	old_refs = (uint32_t)((old_val & REF_COUNT_MASK) >> REF_COUNT_SHIFT);

	old_ver = resolve_handle(old_handle);
	if (old_ver) {
		detach_and_adjust(old_ver, old_refs);
	}
}

/**
 * @brief   Conditionally replace the version if @old_ver matches.
 *
 * @param   gate:     Target gate.
 * @param   slot_idx: Control block slot index.
 * @param   expected: Expected current version.
 * @param   new_ver:  New version to register.
 *
 * @return  true on successful exchange, false otherwise.
 */
bool atomsnap_compare_exchange_version_slot(struct atomsnap_gate *gate,
	int slot_idx, struct atomsnap_version *expected,
	struct atomsnap_version *new_ver)
{
	uint32_t new_handle = new_ver ? new_ver->self_handle : HANDLE_NULL;
	uint32_t exp_handle = expected ? expected->self_handle : HANDLE_NULL;
	_Atomic(uint64_t) *cb = get_cb_slot(gate, slot_idx);
	uint64_t current_val, next_val;
	uint32_t cur_handle, old_refs;
	struct atomsnap_version *old_ver;

	current_val = atomic_load_explicit(cb, memory_order_acquire);
	cur_handle = (uint32_t)(current_val & HANDLE_MASK_64);

	if (cur_handle != exp_handle) {
		return false;
	}

	/*
	 * CAS Loop:
	 * Retry if RefCount changes but Handle is still expected.
	 */
	while (1) {
		if ((uint32_t)(current_val & HANDLE_MASK_64) != exp_handle) {
			return false;
		}

		next_val = (uint64_t)new_handle;

		if (atomic_compare_exchange_weak_explicit(cb, &current_val,
			next_val, memory_order_acq_rel,
			memory_order_acquire)) {
			break;
		}
	}

	old_refs = (uint32_t)((current_val & REF_COUNT_MASK) >> REF_COUNT_SHIFT);

	old_ver = resolve_handle(exp_handle);
	if (old_ver) {
		detach_and_adjust(old_ver, old_refs);
	}

	return true;
}

