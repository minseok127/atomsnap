/**
 * @file    atomsnap.c
 * @brief   Implementation of the atomsnap library.
 *
 * This file implements a lock-free mechanism for managing shared
 * objects with multiple versions using a handle-based approach.
 *
 * Design Overview:
 * - Handles: 40-bit integers composed of (Thread ID | Arena ID | Slot ID).
 * - Control Block: 64-bit atomic containing [24-bit RefCount | 40-bit Handle].
 * - Memory: Per-thread arenas with #atomsnap_version pooling.
 *
 * Reference Counting Logic:
 *
 * When a reader wants to access the current version, it atomically increments
 * the outer reference counter using fetch_add(). The returned 64-bit value has
 * its lower 40-bits representing the pointer of the version whose reference
 * count was increased.
 *
 * After finishing the use of the version, the reader must release it.
 * During release, the reader increments the inner reference counter by 1.
 * If the resulting inner counter is 0, it indicates that no other threads are 
 * referencing that version, so it can be freed.
 *
 * The design handles a bit-width mismatch between the Outer Reference Count 
 * (24-bit, counts acquires) and the Inner Reference Count (64-bit, counts
 * releases).
 *
 * When a writer exchanges a version, it subtracts the snapshot of "Acquires" 
 * (Outer) from the accumulated "Releases" (Inner). To do this correctly, 
 * the Inner counter is first masked to the same 24-bit domain as the Outer 
 * counter. A wraparound factor is used to correct race conditions where 
 * a reader releases the version during the exchange process.
 *
 * Free List Logic (Stack Based):
 *
 * The free list management uses a "Lock-Free MPSC Stack" approach with 
 * tagged pointers to track stack depth.
 *
 * - Tagged Pointers: The upper 24 bits of the 64-bit handle store the
 *                    current stack depth (count). The lower 40 bits store
 *                    the actual slot handle.
 *
 * - Producers (Free): Use a CAS loop to push node onto the arena's
 *                     'top_handle'.
 *
 * - Consumer (Alloc): Uses 'atomic_exchange' to detach the entire stack from
 *                     'top_handle' (Batch Steal).
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <pthread.h>
#include <assert.h>
#include <inttypes.h>
#include <sys/mman.h>

#include "atomsnap.h"

#define PAGE_SIZE             (4096)

/*
 * Maximum number of memory arenas for each thread.
 */
#define MAX_ARENAS_PER_THREAD (64)

/*
 * MAX_THREADS: 1,048,575 (0xFFFFE + 1)
 *
 * The handle is 40 bits long, and HANDLE_NULL is defined as all 40 bits
 * being 1 (0xFFFFFFFFFF).
 *
 * This corresponds to:
 * - Slot Index:   0x3FFF  (16383)
 * - Arena Index:  0x3F    (63)
 * - Thread Index: 0xFFFFF (1048575)
 *
 * To prevent a valid handle from accidentally matching HANDLE_NULL, we must 
 * ensure that at least one field never reaches its maximum value (all 1s).
 * We restrict the Thread ID so that the maximum valid Thread ID is 0xFFFFE 
 * (1,048,574). Therefore, MAX_THREADS is set to 1,048,575.
 */
#define MAX_THREADS           (1048575)

/*
 * SLOTS_PER_ARENA: 16,383
 *
 * We want the memory_arena structure to align perfectly with the page
 * boundaries to minimize memory fragmentation.
 *
 * - atomsnap_version size: 40 bytes
 * - memory_arena header:   8 bytes (top_handle)
 *
 * Calculation:
 * target_pages = 160 pages
 * target_size  = 160 * 4096 = 655,360 bytes
 *
 * If SLOTS = 16,384:
 * Size = 8 + (16,384 * 40) = 655,368 bytes (Overflows by 8 bytes!)
 * This forces allocation of 161 pages, wasting ~4KB.
 *
 * If SLOTS = 16,383:
 * Size = 8 + (16,383 * 40) = 655,328 bytes
 * Remaining space = 655,360 - 655,328 = 32 bytes (Fits in 160 pages).
 *
 * Thus, 16,383 is the optimal number of slots.
 */
#define SLOTS_PER_ARENA       (16383)

/* Bit layout for the 40-bit handle */
#define HANDLE_SLOT_BITS      (14)
#define HANDLE_ARENA_BITS     (6)
#define HANDLE_THREAD_BITS    (20)

/* Special Values */
#define HANDLE_NULL           (0xFFFFFFFFFFULL) /* 40-bit of 1s */

/* 
 * Handle Masking & Tagging
 * We use the upper 24 bits of the 64-bit integer to store the stack depth.
 */
#define HANDLE_MASK_40        (0x000000FFFFFFFFFFULL)
#define STACK_DEPTH_SHIFT     (40)
#define STACK_DEPTH_MASK      (0xFFFFFF0000000000ULL)
#define STACK_DEPTH_INC       (1ULL << STACK_DEPTH_SHIFT)

/* 
 * Control Block (64-bit) 
 * Layout: [ 24-bit RefCount | 40-bit Handle ]
 */
#define REF_COUNT_SHIFT       (40)
#define REF_COUNT_INC         (1ULL << REF_COUNT_SHIFT)
#define REF_COUNT_MASK        (0xFFFFFF0000000000ULL)
#define HANDLE_MASK_64        (0x000000FFFFFFFFFFULL)

/*
 * Wraparound constants for 24-bit RefCount 
 * Used for correcting Inner RefCount (32-bit) to match Outer (24-bit) domain.
 */
#define WRAPAROUND_FACTOR     (0x1000000ULL)
#define WRAPAROUND_MASK       (0xFFFFFFULL)

/* Error logging macro */
#define errmsg(fmt, ...) \
	fprintf(stderr, "[atomsnap:%d:%s] " fmt, __LINE__, __func__, ##__VA_ARGS__)

/*
 * 40-bit Handle Union for easier encoding/decoding.
 * Uses uint64_t to accommodate 40 bits.
 *
 * Note: The 'padding' field will contain the stack depth tag when present.
 */
typedef union {
	struct {
		uint64_t slot_idx   : HANDLE_SLOT_BITS;
		uint64_t arena_idx  : HANDLE_ARENA_BITS;
		uint64_t thread_idx : HANDLE_THREAD_BITS;
		uint64_t padding    : 24;
	};
	uint64_t raw;
} atomsnap_handle_t;

/*
 * atomsnap_version - Internal representation of a version.
 *
 * This structure is allocated within memory arenas. It contains both the
 * user-facing payload fields and internal management fields.
 *
 * @object:        Public-facing pointer to the user object.
 * @free_context:  User-defined context for the free function.
 * @gate:          Pointer to the gate this version belongs to.
 * @inner_ref_cnt: Internal reference counter for reader tracking.
 * @self_handle:   Handle identifying this version (when allocated).
 * @next_handle:   Handle to the next node in the stack (when freed).
 *
 * [ Memory Layout ]
 * 00-08: object (8B)
 * 08-16: free_context (8B)
 * 16-24: gate (8B)
 * 24-32: inner_ref_cnt (8B, int64_t)
 * 32-40: self_handle / next_handle (8B)
 */
struct atomsnap_version {
	void *object;
	void *free_context;
	struct atomsnap_gate *gate;
	_Atomic int64_t inner_ref_cnt;
	union {
		uint64_t self_handle;
		_Atomic uint64_t next_handle;
	};
};

/*
 * memory_arena - Contiguous block of version slots.
 *
 * @top_handle: Handle of the top node in the shared stack.
 * @slots:      Array of version structures. Slot 0 is the Sentinel.
 */
struct memory_arena {
	_Atomic uint64_t top_handle;
	struct atomsnap_version slots[SLOTS_PER_ARENA];
};

/*
 * thread_context - Thread-Local Storage (TLS) context.
 *
 * @thread_id:          Assigned global thread ID.
 * @arenas:             Array of pointers to owned arenas.
 * @local_top:          Top of the local free stack.
 * @active_arena_count: Index of the arena currently being allocated from.
 * @alloc_count:        Allocation counter to trigger periodic reclamation.
 */
struct thread_context {
	int thread_id;
	struct memory_arena *arenas[MAX_ARENAS_PER_THREAD];
	uint64_t local_top;
	int active_arena_count;
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
static pthread_key_t g_tls_key;
static pthread_once_t g_init_once = PTHREAD_ONCE_INIT;

/* Thread ID Management */
static _Atomic bool g_tid_used[MAX_THREADS];

/*
 * Forward Declarations
 */
static int atomsnap_thread_init_internal(void);

/**
 * @brief   Convert a raw handle to a version pointer.
 *
 * @param   handle_tagged: The 64-bit handle (potentially containing
 *                         depth tags).
 *
 * @return  Pointer to the atomsnap_version, or NULL if invalid.
 */
static inline struct atomsnap_version *resolve_handle(uint64_t handle_tagged)
{
	atomsnap_handle_t h;
	struct memory_arena *arena;
	uint64_t handle_raw = handle_tagged & HANDLE_MASK_40;

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
 * @return  Combined 40-bit handle.
 */
static inline uint64_t construct_handle(int tid, int aid, int sid)
{
	atomsnap_handle_t h;
	h.raw = 0;
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
	struct memory_arena *arena;
	uint64_t curr_top, depth;
	int i;

	if (ctx) {
		/*
		 * Attempt to reclaim unused arenas from the last index.
		 * If we encounter an arena that is NOT fully free, we stop immediately.
		 * This preserves the contiguity of the active_arena_count.
		 */
		for (i = ctx->active_arena_count - 1; i >= 0; i--) {
			arena = ctx->arenas[i];
			curr_top = atomic_load(&arena->top_handle);
			depth = (curr_top & STACK_DEPTH_MASK) >> STACK_DEPTH_SHIFT;

			/* Check if arena is fully free (SLOTS - 1 because of sentinel) */
			if (depth == (SLOTS_PER_ARENA - 1)) {
				madvise(arena, sizeof(struct memory_arena), MADV_DONTNEED);
				ctx->active_arena_count--;
			} else {
				/* Found a busy arena, stop reclamation to avoid holes */
				break;
			}
		}

		/* Release the Thread ID atomically */
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
 * Sets up the Sentinel (slot 0) and links slots 1..N into the local free
 * list as a Stack (LIFO).
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
	uint64_t sentinel_handle, curr, next_in_stack;
	struct atomsnap_version *slot;
	int i;

	/*
	 * Check if the arena pointer already exists (reclaimed/reused).
	 * If so, we reuse the pointer and rely on madvise() having zeroed it,
	 * or simply re-initialize the data structures.
	 */
	if (ctx->arenas[arena_idx] != NULL) {
		arena = ctx->arenas[arena_idx];
	} else {
		arena = aligned_alloc(PAGE_SIZE, sizeof(struct memory_arena));
		if (!arena) {
			return -1;
		}
		memset(arena, 0, sizeof(struct memory_arena));

		g_arena_table[tid][arena_idx] = arena;
		ctx->arenas[arena_idx] = arena;
	}

	/* Setup Sentinel (Slot 0) */
	sentinel_handle = construct_handle(tid, arena_idx, 0);
	
	/* Sentinel points to NULL (Bottom of the stack) */
	atomic_store(&arena->slots[0].next_handle, HANDLE_NULL);

	/* Arena Top initially points to Sentinel */
	atomic_store(&arena->top_handle, sentinel_handle);

	/*
	 * Link slots 1..N sequentially to form a pre-filled stack.
	 * Slot 1 -> Sentinel
	 * Slot 2 -> Slot 1
	 * ...
	 * Slot N -> Slot N-1
	 * Local Top -> Slot N
	 */
	next_in_stack = sentinel_handle;

	for (i = 1; i < SLOTS_PER_ARENA; i++) {
		curr = construct_handle(tid, arena_idx, i);
		slot = resolve_handle(curr);
		
		/* Link current node to the node below it in the stack */
		atomic_store(&slot->next_handle, next_in_stack);
		next_in_stack = curr;
	}

	/* The last processed handle is the new top of the local stack */
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
static uint64_t pop_local(struct thread_context *ctx)
{
	uint64_t handle_tagged;
	struct atomsnap_version *slot;
	atomsnap_handle_t h;

	if (ctx->local_top == HANDLE_NULL) {
		return HANDLE_NULL;
	}

	/* 
	 * Mask out any potential tags from the handle before checking
	 * if it is the Sentinel.
	 */
	handle_tagged = ctx->local_top;
	h.raw = handle_tagged & HANDLE_MASK_40;

	/* Check if the top is the Sentinel (Slot 0) */
	if (h.slot_idx == 0) {
		/* Stack is empty (hit sentinel) */
		ctx->local_top = HANDLE_NULL;
		return HANDLE_NULL;
	}

	slot = resolve_handle(handle_tagged);

	/*
	 * Move top to the next node down the stack.
	 */
	ctx->local_top = atomic_load_explicit(&slot->next_handle, 
		memory_order_relaxed);

	/* Restore self_handle for Allocated state (clean 40-bit) */
	slot->self_handle = h.raw;
	return h.raw;
}

/**
 * @brief   Allocates a slot handle.
 *
 * Strategy:
 * 1. Try Local Stack (pop_local).
 * 2. Try Batch Steal from Arenas (atomic_exchange).
 * 3. Init New Arena.
 *
 * @param   ctx: Thread context.
 *
 * @return  Handle of the allocated slot, or HANDLE_NULL on failure.
 */
static uint64_t alloc_slot(struct thread_context *ctx)
{
	uint64_t handle;
	struct memory_arena *arena;
	uint64_t sentinel_handle, batch_top, curr_top, depth;
	int i, new_idx;

	/* Increment allocation counter for periodic trigger */
	ctx->alloc_count++;

	/*
	 * Periodic Reclamation Check.
	 * Check if the last active arena is fully free.
	 */
	if ((ctx->alloc_count % SLOTS_PER_ARENA) == 0) {
		/* Only reclaim if we have more than 1 arena */
		if (ctx->active_arena_count > 1) {
			arena = ctx->arenas[ctx->active_arena_count - 1];
			
			/* Read top handle to check utilization */
			curr_top = atomic_load(&arena->top_handle);
			depth = (curr_top & STACK_DEPTH_MASK) >> STACK_DEPTH_SHIFT;

			/*
			 * If depth equals (SLOTS - 1), it means all slots (1..N) 
			 * have been returned to the global stack.
			 * Note: Slot 0 is sentinel, so count is SLOTS - 1.
			 */
			if (depth == (SLOTS_PER_ARENA - 1)) {
				/* Reclaim physical memory but keep virtual address/pointer */
				madvise(arena, sizeof(struct memory_arena), MADV_DONTNEED);
				
				/* Shrink the active arena stack */
				ctx->active_arena_count--;
			}
		}
	}

	/* 1. Try Local Free Stack */
	handle = pop_local(ctx);
	if (handle != HANDLE_NULL) {
		return handle;
	}

	/* 2. Try Batch Steal from existing arenas */
	for (i = 0; i < ctx->active_arena_count; i++) {
		arena = ctx->arenas[i];
		sentinel_handle = construct_handle(ctx->thread_id, i, 0);

		/*
		 * Check if there is anything to steal.
		 * If top matches sentinel, the stack is empty (contains only sentinel).
		 */
		curr_top = atomic_load(&arena->top_handle);
		if (curr_top == sentinel_handle) {
			continue;
		}

		/*
		 * Batch Steal: Atomically exchange Top with Sentinel.
		 * This detaches the entire stack and resets the arena to empty state.
		 * Since we are the only consumer for this arena, this is safe.
		 */
		batch_top = atomic_exchange(&arena->top_handle, sentinel_handle);

		/*
		 * If we raced with a free operation, we might get sentinel.
		 */
		if ((batch_top & HANDLE_MASK_40) == sentinel_handle) {
			continue;
		}

		/*
		 * Adopt the batch as the new local stack.
		 * The batch is a linked list ending at Sentinel.
		 */
		ctx->local_top = batch_top;

		/* Retry pop from the newly filled local stack */
		return pop_local(ctx);
	}

	/* 3. Allocate New Arena */
	if (ctx->active_arena_count < MAX_ARENAS_PER_THREAD - 1) {
		new_idx = ctx->active_arena_count;
		if (init_arena(ctx, new_idx) == 0) {
			ctx->active_arena_count++;
			/* Retry pop */
			return pop_local(ctx);
		}
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
	uint64_t my_handle = slot->self_handle;
	atomsnap_handle_t h = { .raw = my_handle };
	struct memory_arena *arena = g_arena_table[h.thread_idx][h.arena_idx];
	uint64_t old_top, new_top, depth;

	old_top = atomic_load(&arena->top_handle);
	do {
		/* 
		 * 1. Extract current stack depth.
		 */
		depth = (old_top & STACK_DEPTH_MASK);

		/* 
		 * 2. Increment depth.
		 */
		depth += STACK_DEPTH_INC;

		/* 
		 * 3. Construct new top handle: [ New Depth | My Handle ] 
		 */
		new_top = depth | my_handle;

		/* Link: Me -> Old Top (Down the stack) */
		atomic_store(&slot->next_handle, old_top);
		
		/* Attempt to make Me the New Top */
	} while (!atomic_compare_exchange_weak(&arena->top_handle,
				&old_top, new_top));
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
		/* Check without locking cache line first */
		if (atomic_load(&g_tid_used[i]) == true) {
			continue;
		}

		expected = false;
		if (atomic_compare_exchange_strong(&g_tid_used[i], &expected, true)) {
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
			/* Rollback ID */
			atomic_store(&g_tid_used[tid], false);
			return -1;
		}
		ctx->thread_id = tid;
		ctx->active_arena_count = 0;
		ctx->alloc_count = 0;
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
	uint64_t handle;
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

	if (version->object && version->gate && version->gate->free_impl) {
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
 * @param   ver: The version pointer.
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
	uint64_t handle;

	/* Increment Reference Count (Upper 24 bits) */
	val = atomic_fetch_add_explicit(cb, REF_COUNT_INC, memory_order_acquire);

	handle = (val & HANDLE_MASK_64);

	return resolve_handle(handle);
}

/**
 * @brief   Release a version previously acquired.
 *
 * Increments inner ref count.
 * If 0 (meaning all readers done and writer detached), frees the version.
 *
 * @param   ver: Version to release.
 */
void atomsnap_release_version(struct atomsnap_version *ver)
{
	int64_t rc;

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
	uint64_t new_handle = new_ver ? new_ver->self_handle : HANDLE_NULL;
	_Atomic uint64_t *cb = get_cb_slot(gate, slot_idx);
	uint64_t old_val, old_handle;
	uint32_t old_refs;
	struct atomsnap_version *old_ver;
	int64_t rc;

	/*
	 * Swap the handle in the control block.
	 * The new value will have 'new_handle' and 'RefCount = 0' (implicitly).
	 */
	old_val = atomic_exchange_explicit(cb, new_handle, memory_order_acq_rel);

	old_handle = (old_val & HANDLE_MASK_64);
	old_refs = (uint32_t)((old_val & REF_COUNT_MASK) >> REF_COUNT_SHIFT);

	old_ver = resolve_handle(old_handle);
	if (old_ver) {
		/* Consider wraparound */
		atomic_fetch_and_explicit(&old_ver->inner_ref_cnt, WRAPAROUND_MASK,
			memory_order_relaxed);

		/* Decrease inner ref counter, we expect the result is minus */
		rc = atomic_fetch_sub_explicit(&old_ver->inner_ref_cnt, old_refs,
			memory_order_release) - old_refs;

		/* The outer counter has been wraparound, adjust inner count */
		if (rc > 0) {
			rc = atomic_fetch_sub_explicit(&old_ver->inner_ref_cnt,
				WRAPAROUND_FACTOR, memory_order_relaxed) - WRAPAROUND_FACTOR;
		}

		assert(rc <= 0);

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
	uint64_t new_handle = new_ver ? new_ver->self_handle : HANDLE_NULL;
	uint64_t exp_handle = expected ? expected->self_handle : HANDLE_NULL;
	_Atomic uint64_t *cb = get_cb_slot(gate, slot_idx);
	uint64_t current_val, cur_handle, next_val;
	uint32_t old_refs;
	struct atomsnap_version *old_ver;
	int64_t rc;

	current_val = atomic_load_explicit(cb, memory_order_acquire);
	cur_handle = (current_val & HANDLE_MASK_64);

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
		if ((current_val & HANDLE_MASK_64) != exp_handle) {
			return false;
		}

		next_val = new_handle;

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
		atomic_fetch_and_explicit(&old_ver->inner_ref_cnt, WRAPAROUND_MASK,
			memory_order_relaxed);

		rc = atomic_fetch_sub_explicit(&old_ver->inner_ref_cnt, old_refs,
			memory_order_release) - old_refs;

		if (rc > 0) {
			rc = atomic_fetch_sub_explicit(&old_ver->inner_ref_cnt,
				WRAPAROUND_FACTOR, memory_order_relaxed) - WRAPAROUND_FACTOR;
		}

		assert(rc <= 0);

		if (rc == 0) {
			if (gate->free_impl) {
				gate->free_impl(old_ver->object, old_ver->free_context);
			}
			free_slot(old_ver);
		}
	}

	return true;
}
