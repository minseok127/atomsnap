/**
 * @file    atomsnap.c
 * @brief   Implementation of the atomsnap library.
 *
 * This file implements a wait-free/lock-free mechanism for managing shared
 * objects with multiple versions using a handle-based approach.
 *
 * Design Overview:
 * - Handles: 32-bit integers composed of (Thread ID | Arena ID | Slot ID).
 * - Control Block: 64-bit atomic containing [32-bit RefCount | 32-bit Handle].
 * - Memory: Per-thread arenas with wait-free object pooling.
 * - Lifecycle: Thread contexts are recycled (adopted) to handle early exits.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <pthread.h>
#include <assert.h>
#include <inttypes.h>

#include "atomsnap.h"

/*
 * Configuration & Constants
 */
#define MAX_THREADS           (4096)
#define MAX_ARENAS_PER_THREAD (64)
#define SLOTS_PER_ARENA       (16384)
#define PAGE_SIZE             (4096)

/* Bit layout for the 32-bit handle */
#define HANDLE_SLOT_BITS      (14)
#define HANDLE_ARENA_BITS     (6)
#define HANDLE_THREAD_BITS    (12)

/* Special Values */
#define HANDLE_NULL           (0xFFFFFFFF)

/* Control Block (64-bit) */
#define REF_COUNT_INC         (0x0000000100000000ULL)
#define REF_COUNT_MASK        (0xFFFFFFFF00000000ULL)
#define REF_COUNT_SHIFT       (32)
#define HANDLE_MASK_64        (0x00000000FFFFFFFFULL)

/* Error logging macro to match trcache style */
#define errmsg(fmt, ...) \
	fprintf(stderr, "[atomsnap:%d:%s] " fmt, __LINE__, __func__, ##__VA_ARGS__)

/* 
 * 32-bit Handle Union for easier encoding/decoding.
 */
typedef union {
	struct {
		uint32_t slot_idx   : HANDLE_SLOT_BITS;
		uint32_t arena_idx  : HANDLE_ARENA_BITS;
		uint32_t thread_idx : HANDLE_THREAD_BITS;
	};
	uint32_t raw;
} atomsnap_handle_t;

/*
 * atomsnap_version - Internal representation of a version (32 Bytes).
 *
 * This structure is allocated within memory arenas. It contains both the
 * user-facing payload fields and internal management fields.
 *
 * @object:        Public-facing pointer to the user object.
 * @free_context:  User-defined context for the free function.
 * @gate:          Pointer to the gate this version belongs to.
 * @inner_ref_cnt: Internal reference counter for reader tracking.
 * @self_handle:   Handle identifying this version (when allocated).
 * @next_handle:   Handle to the next node in free list (when freed).
 *
 * [ Memory Layout ]
 * 00-08: object (8B)
 * 08-16: free_context (8B)
 * 16-24: gate (8B)
 * 24-28: inner_ref_cnt (4B)
 * 28-32: self_handle / next_handle (4B - Union)
 */
struct atomsnap_version {
	/* Public-facing fields (accessed via getters/setters) */
	void *object;
	void *free_context;
	struct atomsnap_gate *gate;

	/* Internal management fields */
	_Atomic uint32_t inner_ref_cnt;

	/*
	 * Union to save space.
	 * - Allocated: self_handle (to identify itself during release)
	 * - Free: next_handle (to link in the free list)
	 */
	union {
		uint32_t self_handle;
		_Atomic uint32_t next_handle;
	};
};

/*
 * memory_arena - Contiguous block of version slots.
 *
 * @tail_handle: Handle of the last node pushed to the shared list.
 * @slots:       Array of version structures. Slot 0 is the Sentinel.
 */
struct memory_arena {
	_Atomic uint32_t tail_handle;
	struct atomsnap_version slots[SLOTS_PER_ARENA];
};

/*
 * thread_context - Thread-Local Storage (TLS) context.
 *
 * @thread_id:         Assigned global thread ID.
 * @arenas:            Array of pointers to owned arenas.
 * @local_free_head:   Head of the local free list (batch).
 * @local_free_tail:   Tail of the local free list (batch).
 * @current_arena_idx: Index of the arena currently being allocated from.
 */
struct thread_context {
	int thread_id;
	struct memory_arena *arenas[MAX_ARENAS_PER_THREAD];
	
	/* Local Free List (Maintained as a segment/batch) */
	uint32_t local_free_head;
	uint32_t local_free_tail;

	int current_arena_idx;
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
	_Atomic uint64_t control_block;
	atomsnap_free_func free_impl;
	_Atomic uint64_t *extra_control_blocks;
	int num_extra_slots;
};

/*
 * Global Variables
 */
static struct memory_arena *g_arena_table[MAX_THREADS][MAX_ARENAS_PER_THREAD];
static struct thread_context *g_thread_contexts[MAX_THREADS];

/* Thread ID Management */
static pthread_mutex_t g_tid_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool g_tid_used[MAX_THREADS];

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
	
	/* Bounds check could be added here, but omitted for speed */
	arena = g_arena_table[h.thread_idx][h.arena_idx];

	if (__builtin_expect(arena == NULL, 0)) {
		return NULL;
	}

	return &arena->slots[h.slot_idx];
}

static inline void cpu_relax(void)
{
	__asm__ __volatile__("pause");
}

/**
 * @brief   Construct a handle from indices.
 *
 * @param   tid: Thread ID.
 * @param   aid: Arena ID.
 * @param   sid: Slot ID.
 *
 * @return  Combined 32-bit handle.
 */
static inline uint32_t construct_handle(int tid, int aid, int sid)
{
	atomsnap_handle_t h;
	h.thread_idx = tid;
	h.arena_idx = aid;
	h.slot_idx = sid;
	return h.raw;
}

/**
 * @brief   TLS destructor called when a thread exits.
 *
 * We do NOT free the context or arenas here. We only release the Thread ID
 * so that a new thread can adopt this context (and its arenas).
 *
 * @param   arg: Pointer to the thread_context.
 */
static void tls_destructor(void *arg)
{
	struct thread_context *ctx = (struct thread_context *)arg;
	
	if (ctx) {
		pthread_mutex_lock(&g_tid_mutex);
		g_tid_used[ctx->thread_id] = false;
		pthread_mutex_unlock(&g_tid_mutex);
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
	struct thread_context *ctx
		= (struct thread_context *)pthread_getspecific(g_tls_key);

	if (__builtin_expect(ctx == NULL, 0)) {
		/* Lazy initialization */
		pthread_once(&g_init_once, global_init_routine);
		if (atomsnap_thread_init_internal() != 0) {
			return NULL;
		}
		ctx = (struct thread_context *)pthread_getspecific(g_tls_key);
	}
	return ctx;
}

/**
 * @brief   Initialize a new arena.
 *
 * Sets up the Sentinel (slot 0) and links slots 1..N into the local free list.
 *
 * @param   ctx:        Thread context.
 * @param   arena_idx:  Index of the arena to initialize.
 *
 * @return  0 on success, -1 on failure.
 */
static int init_arena(struct thread_context *ctx, int arena_idx)
{
	struct memory_arena *arena;
	int tid = ctx->thread_id;
	uint32_t sentinel_handle, first_handle, last_handle;
	uint32_t curr, next;
	struct atomsnap_version *slot, *last_slot;
	int i;

	arena = aligned_alloc(PAGE_SIZE, sizeof(struct memory_arena));
	if (!arena) {
		return -1;
	}
	memset(arena, 0, sizeof(struct memory_arena));

	g_arena_table[tid][arena_idx] = arena;
	ctx->arenas[arena_idx] = arena;

	/* Setup Sentinel (Slot 0) */
	sentinel_handle = construct_handle(tid, arena_idx, 0);
	atomic_store(&arena->slots[0].next_handle, HANDLE_NULL);

	/* Sentinel is the initial tail */
	atomic_store(&arena->tail_handle, sentinel_handle);

	/*
	 * Link slots 1..N sequentially.
	 * 1 -> 2 -> ... -> N -> (Old Local List)
	 */
	first_handle = construct_handle(tid, arena_idx, 1);
	last_handle = construct_handle(tid, arena_idx, SLOTS_PER_ARENA - 1);

	for (i = 1; i < SLOTS_PER_ARENA - 1; i++) {
		curr = construct_handle(tid, arena_idx, i);
		next = construct_handle(tid, arena_idx, i + 1);
		slot = resolve_handle(curr);
		/* Initially in Free state, so next_handle is used */
		slot->next_handle = next;
	}

	/* Link the last new slot to the existing local list */
	last_slot = resolve_handle(last_handle);
	
	/* If local list was empty, next is NULL */
	last_slot->next_handle = ctx->local_free_head;

	/* If local list was empty, update tail to this new batch's tail */
	if (ctx->local_free_head == HANDLE_NULL) {
		ctx->local_free_tail = last_handle;
	}

	/* Update local head */
	ctx->local_free_head = first_handle;

	return 0;
}

/**
 * @brief   Pop a slot from the local free list (Fast Path).
 *
 * @param   ctx: Thread context.
 *
 * @return  Handle of the allocated slot, or HANDLE_NULL if empty.
 */
static uint32_t pop_local(struct thread_context *ctx)
{
	uint32_t handle, next_handle;
	struct atomsnap_version *slot;

	if (ctx->local_free_head == HANDLE_NULL) {
		return HANDLE_NULL;
	}

	handle = ctx->local_free_head;
	slot = resolve_handle(handle);

	/*
	 * Check if this is the last node in the local list.
	 */
	if (ctx->local_free_head == ctx->local_free_tail) {
		/* List becomes empty */
		ctx->local_free_head = HANDLE_NULL;
		ctx->local_free_tail = HANDLE_NULL;
	} else {
		/*
		 * Not the last node.
		 * Spin-wait if the next pointer is not yet established by the pusher.
		 */
		while ((next_handle = atomic_load(&slot->next_handle)) == HANDLE_NULL) {
			cpu_relax();
		}
		ctx->local_free_head = next_handle;
	}

	/* Restore self_handle for Allocated state */
	slot->self_handle = handle;
	return handle;
}

/**
 * @brief   Allocates a slot handle.
 *
 * Strategy:
 * 1. Try Local List.
 * 2. Try Batch Steal from Arenas.
 * 3. Init New Arena.
 *
 * @param   ctx: Thread context.
 *
 * @return  Handle of the allocated slot, or HANDLE_NULL on failure.
 */
static uint32_t alloc_slot(struct thread_context *ctx)
{
	uint32_t handle;
	struct memory_arena *arena;
	struct atomsnap_version *sentinel;
	uint32_t sentinel_handle, batch_head, batch_tail;
	int i;

	/* 1. Try Local Free List */
	handle = pop_local(ctx);
	if (handle != HANDLE_NULL) {
		return handle;
	}

	/* 2. Try Batch Steal from existing arenas */
	for (i = 0; i <= ctx->current_arena_idx; i++) {
		arena = ctx->arenas[i];
		sentinel = &arena->slots[0];
		sentinel_handle = construct_handle(ctx->thread_id, i, 0);

		/* Check if there are nodes after sentinel (next != -1) */
		if (atomic_load(&sentinel->next_handle) == HANDLE_NULL) {
			continue;
		}

		/*
		 * Found a batch.
		 * A. Detach Head from Sentinel (Atomic Store)
		 * Only THIS thread consumes from its arenas (Single Consumer).
		 */
		batch_head = sentinel->next_handle;
		atomic_store(&sentinel->next_handle, HANDLE_NULL);

		/*
		 * B. Reset Tail to Sentinel (Atomic Exchange)
		 * This closes the batch at the current tail.
		 */
		batch_tail = atomic_exchange(&arena->tail_handle, sentinel_handle);

		/*
		 * C. Adopt the batch as the new local list.
		 * Since local list is empty (checked at step 1), just set it.
		 */
		ctx->local_free_head = batch_head;
		ctx->local_free_tail = batch_tail;

		/* Retry pop from the newly filled local list */
		return pop_local(ctx);
	}

	/* 3. Allocate New Arena */
	if (ctx->current_arena_idx < MAX_ARENAS_PER_THREAD - 1) {
		int new_idx = ctx->current_arena_idx + 1;
		if (init_arena(ctx, new_idx) == 0) {
			ctx->current_arena_idx = new_idx;
			/* Retry pop */
			return pop_local(ctx);
		}
	}

	errmsg("Out of memory (Max arenas reached)\n");
	return HANDLE_NULL;
}

/**
 * @brief   Returns a slot to its arena (Wait-Free Push).
 *
 * @param   slot: Pointer to the version slot to free.
 */
static void free_slot(struct atomsnap_version *slot)
{
	uint32_t my_handle = slot->self_handle;
	atomsnap_handle_t h = { .raw = my_handle };
	struct memory_arena *arena = g_arena_table[h.thread_idx][h.arena_idx];
	uint32_t prev_tail;
	struct atomsnap_version *prev_node;

	/* 1. Set Next to NULL (-1) BEFORE exposing to list */
	atomic_store(&slot->next_handle, HANDLE_NULL);

	/* 2. Swap Tail (Wait-Free Linearization Point) */
	prev_tail = atomic_exchange(&arena->tail_handle, my_handle);

	/* 3. Link Previous Node to Me */
	/* No Branching: prev_node is valid (Sentinel or Normal Node) */
	prev_node = resolve_handle(prev_tail);
	atomic_store(&prev_node->next_handle, my_handle);
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
	int tid = -1;
	int i;

	/* 1. Acquire Thread ID using Global Mutex */
	pthread_mutex_lock(&g_tid_mutex);
	for (i = 0; i < MAX_THREADS; i++) {
		if (!g_tid_used[i]) {
			g_tid_used[i] = true;
			tid = i;
			break;
		}
	}
	pthread_mutex_unlock(&g_tid_mutex);

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
			/* Rollback ID */
			pthread_mutex_lock(&g_tid_mutex);
			g_tid_used[tid] = false;
			pthread_mutex_unlock(&g_tid_mutex);
			return -1;
		}
		ctx->thread_id = tid;
		ctx->current_arena_idx = -1;
		ctx->local_free_head = HANDLE_NULL;
		ctx->local_free_tail = HANDLE_NULL;
		g_thread_contexts[tid] = ctx;
	} else {
		/*
		 * Adoption: Reuse existing context and arenas.
		 */
	}

	/* 3. Set TLS */
	if (pthread_setspecific(g_tls_key, ctx) != 0) {
		errmsg("Failed to set TLS value\n");
		/* Fatal error, but we leave ctx in global table for future use */
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
			sizeof(_Atomic uint64_t));
		
		if (gate->extra_control_blocks == NULL) {
			errmsg("Extra blocks allocation failed\n");
			free(gate);
			return NULL;
		}

		for (int i = 0; i < gate->num_extra_slots; i++) {
			atomic_init(&gate->extra_control_blocks[i], (uint64_t)HANDLE_NULL);
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
	
	atomic_store_explicit(&slot->inner_ref_cnt, 0, memory_order_relaxed);

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
	if (version == NULL) {
		return;
	}

	if (version->gate && version->gate->free_impl) {
		version->gate->free_impl(version->object, version->free_context);
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
		ver->object = object;
		ver->free_context = free_context;
	}
}

/**
 * @brief   Get the user payload object from a version.
 *
 * @param   ver: The version handle.
 *
 * @return  Pointer to the user object.
 */
void *atomsnap_get_object(const struct atomsnap_version *ver)
{
	return ver ? ver->object : NULL;
}

static inline _Atomic uint64_t *get_cb_slot(struct atomsnap_gate *gate, int idx)
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
	_Atomic uint64_t *cb = get_cb_slot(gate, slot_idx);
	uint64_t val;
	uint32_t handle;

	/* Increment Reference Count (Upper 32 bits) */
	val = atomic_fetch_add_explicit(cb, REF_COUNT_INC, memory_order_acquire);

	handle = (uint32_t)(val & HANDLE_MASK_64);

	return resolve_handle(handle);
}

/**
 * @brief   Release a version previously acquired.
 *
 * Increments inner ref count. If 0 (meaning all readers done and writer
 * detached), frees the version.
 *
 * @param   ver: Version to release.
 */
void atomsnap_release_version(struct atomsnap_version *ver)
{
	int32_t rc;

	if (ver == NULL) {
		return;
	}

	/*
	 * Increment inner ref count.
	 * Logic: 
	 * - Writer decrements by N (outer ref count at exchange time).
	 * - Reader increments by 1 when done.
	 * - Sum == 0 means all readers have finished and writer has detached it.
	 */
	rc = atomic_fetch_add_explicit(&ver->inner_ref_cnt, 1,
		memory_order_release) + 1;

	if (rc == 0) {
		/* Invoke user-defined cleanup */
		if (ver->gate && ver->gate->free_impl) {
			ver->gate->free_impl(ver->object, ver->free_context);
		}

		/* Return slot to free list */
		free_slot(ver);
	}
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
	_Atomic uint64_t *cb = get_cb_slot(gate, slot_idx);
	uint64_t old_val;
	uint32_t old_handle, old_refs;
	struct atomsnap_version *old_ver;
	int32_t rc;

	/*
	 * Swap the handle in the control block.
	 * The new value will have `new_handle` and `RefCount = 0` (implicitly).
	 */
	old_val = atomic_exchange_explicit(cb, (uint64_t)new_handle,
		memory_order_acq_rel);

	old_handle = (uint32_t)(old_val & HANDLE_MASK_64);
	old_refs = (uint32_t)((old_val & REF_COUNT_MASK) >> REF_COUNT_SHIFT);

	old_ver = resolve_handle(old_handle);
	if (old_ver) {
		/*
		 * Subtract reader count from inner counter.
		 *
		 * Since both counters are 32-bit:
		 * Remaining = (Inner - Outer)
		 *
		 * Even if Outer has wrapped around (e.g., Outer=5, Inner=UINT32_MAX+6),
		 * the subtraction in 32-bit unsigned arithmetic automatically yields
		 * the correct delta. No manual 'if (wrapped)' check is needed.
		 *
		 * @warning If the bit-widths of Outer and Inner counters are ever
		 * changed to be unequal in the future, you MUST restore the masking
		 * logic (e.g., inner & mask) to ensure they operate in the same domain.
		 */
		rc = atomic_fetch_sub_explicit(&old_ver->inner_ref_cnt, old_refs,
			memory_order_release) - old_refs;

		if (rc == 0) {
			if (gate->free_impl) {
				gate->free_impl(old_ver->object, old_ver->free_context);
			}
			free_slot(old_ver);
		}
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
	_Atomic uint64_t *cb = get_cb_slot(gate, slot_idx);
	uint64_t current_val, next_val;
	uint32_t cur_handle, old_refs;
	struct atomsnap_version *old_ver;
	int32_t rc;

	current_val = atomic_load_explicit(cb, memory_order_acquire);
	cur_handle = (uint32_t)(current_val & HANDLE_MASK_64);

	if (cur_handle != exp_handle) {
		return false;
	}

	/*
	 * CAS Loop:
	 * Retry if RefCount changes but Handle is still expected.
	 *
	 * Note: If atomic_compare_exchange_weak() fails (e.g., because concurrent
	 * readers incremented the refcount), it automatically updates 'current_val'
	 * with the latest value from 'cb'. This ensures the loop adapts to the
	 * new refcount and retries, preventing livelock.
	 */
	while (1) {
		if ((uint32_t)(current_val & HANDLE_MASK_64) != exp_handle) {
			return false;
		}

		next_val = (uint64_t)new_handle;

		if (atomic_compare_exchange_weak_explicit(cb, &current_val, next_val,
				memory_order_acq_rel, memory_order_acquire)) {
			break;
		}
	}

	old_refs = (uint32_t)((current_val & REF_COUNT_MASK) >> REF_COUNT_SHIFT);

	old_ver = resolve_handle(exp_handle);
	if (old_ver) {
		/*
		 * Same logic with atomsnap_exchange_version_slot().
		 */
		rc = atomic_fetch_sub_explicit(&old_ver->inner_ref_cnt, old_refs,
			memory_order_release) - old_refs;

		if (rc == 0) {
			if (gate->free_impl) {
				gate->free_impl(old_ver->object, old_ver->free_context);
			}
			free_slot(old_ver);
		}
	}

	return true;
}
