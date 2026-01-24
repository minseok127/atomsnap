# ATOMSNAP

This library is a lock-free concurrency primitive for managing shared objects with multiple versions. It allows multiple readers and writers to access a shared pointer simultaneously without blocking, ensuring system-wide progress and low latency.

# Use Cases

- Objects are too large for single atomic instructions (>8 bytes).
- Readers access immutable snapshots while writers create new versions.

# Critical Usage Rules

**[Planned]: Features or constraints targeted for optimization or removal in future releases.**

- **MAX_THREADS**: This library supports a global maximum of 1,048,576 threads.

**[Fixed]: Permanent architectural constraints or safety policies that will remain unchanged.**

- **Acquire-Release Pairing**: Every `atomsnap_acquire_version()` must have a matching `atomsnap_release_version()`.
- **No Nested Acquires**: Do not acquire multiple versions without releasing previous ones.
- **CAS Ordering**: When using `atomsnap_compare_exchange_version()`, always call `atomsnap_release_version()` AFTER the CAS operation to prevent ABA problems.
- **Failed CAS Cleanup**: When CAS fails, manually free the unused version with `atomsnap_free_version()` or retry CAS with that version to prevent memory leaks.

# Build
```bash
$ git clone https://github.com/minseok127/atomsnap.git
$ cd atomsnap
$ make
```

This produces:
- `libatomsnap.a` - Static library
- `libatomsnap.so` - Shared library
- `atomsnap.h` - Public header file

### Build Options
```bash
# Release build (default, -O2)
$ make BUILD_MODE=release

# Debug build (-O0 -g -pg)
$ make BUILD_MODE=debug
```

---

# Architecture

## Core Concepts

### 1. Control Block Layout (64-bit)

The core atomic variable is a 64-bit control block that packs both the reference count and the version handle.
This ensures that reading the pointer and incrementing the reference count happen in a single atomic instruction.

```
+--------------------+----------------------+
|   RefCount (32b)   |     Handle (32b)     |
+--------------------+----------------------+
|      Acquires      |  Arena ID | Slot ID  |
+--------------------+----------------------+
```

- **Reference Count (32-bit)**: Tracks the number of Acquires for the current version.
- **Handle (32-bit)**: A unique identifier for the `atomsnap_version`.

### 2. Handle Structure (32-bit)

The 32-bit handle uniquely identifies a version slot within the global memory space.

- **Arena Index (20-bit)**: Identifies the arena in the global table (0 ~ 1,048,575).
- **Slot Index (12-bit)**: Identifies the slot index within the arena (0 ~ 4,095).

### 3. Memory Arena & Page Alignment

Memory is managed in Arenas, which are contiguous blocks of pre-allocated
version slots.

- **Page Alignment**: Each arena is designed to fit within **32 memory pages**
  (4KB pages).
- `SLOTS_PER_ARENA` is chosen based on `sizeof(atomsnap_version)`.

Example (current layout):
- `sizeof(atomsnap_version) = 40 bytes`
- `memory_arena` header: `8 bytes` (top_handle)

Formula:
- `8B + (3,276 * 40B) = 131,048B`
- `32 * 4096B = 131,072B`
- Wasted: `24B` per arena

> Note: `SLOTS_PER_ARENA` may change if the internal version layout changes,
> while maintaining the 32-page arena goal.

**Free List Design**:
- MPSC lock-free stack operation
    - Thread-local batch for fast allocation (pop)
    - Arena-level shared list for cross-thread recycling (push)
    - Global arena table with dynamic thread-local caching

### 4. Reference Counting Logic

Atomsnap uses a dual-layer accounting scheme:

- **Outer RefCount (32-bit)** lives in the Gate's Control Block
  (upper 32 bits of a 64-bit atomic). Each acquire increments this value.

- **Inner State (64-bit)** lives in each Version:
  `[32-bit Counter | 32-bit Flags]`.

Readers increment **only the counter** (upper 32 bits) on release, so the
lower 32-bit flags are never affected by carry/overflow.

When a writer publishes a new version (exchange / CAS success), it **detaches**
the old version and **atomically adjusts** the inner counter by subtracting the
accumulated outer refcount. A version is eligible for reclamation only when:

- `DETACHED` flag is set, and
- inner counter becomes `0`.

Reclamation is claimed via a `FINALIZED` flag to guarantee the free callback
runs exactly once even under contention.

> Note: The 32-bit counters can wrap around by definition, but wrap-around
> cannot cause premature reclamation because reclamation is gated by `DETACHED`
> and finalized via `FINALIZED`.

### 5. Reclamation Algorithm (Outer RefCount + Inner State)

**Lifecycle**:

1. Writer allocates a version from the arena.
2. Writer sets the payload pointer and publishes it via `exchange` / `CAS`.
3. Readers acquire the current version by incrementing the outer refcount in a
   single atomic instruction.
4. After publishing, the writer detaches the old version and atomically adjusts
   its inner counter by subtracting the accumulated outer refcount.
5. Readers release by incrementing the inner counter (upper 32 bits only).
6. When `DETACHED && counter == 0`, a thread claims `FINALIZED` and runs the
   free callback exactly once, then returns the slot to the arena.

---

# API Reference

## Data Structures

### atomsnap_gate
Synchronization point managing versioned objects. Supports multiple independent control block slots for managing separate version chains within a single gate.

### atomsnap_version
Represents a specific version of an object.

**Fields**:
- `object` - User payload pointer
- `free_context` - User-defined cleanup context
- `gate` - Associated gate (read-only)
- `opaque` - Internal management data (do not modify)

### atomsnap_init_context
Configuration for gate initialization.

**Fields**:
- `free_impl` - Cleanup function: `void (*)(void *object, void *free_context)`
- `num_extra_control_blocks` - Number of additional slots (0 for single slot)

## Functions

### Gate Management

**`atomsnap_gate *atomsnap_init_gate(atomsnap_init_context *ctx)`**
- Creates and initializes a gate
- Returns: Gate pointer, or NULL on failure

**`void atomsnap_destroy_gate(atomsnap_gate *gate)`**
- Destroys a gate
- Note: Undefined behavior if versions are still in use

### Version Allocation

**`atomsnap_version *atomsnap_make_version(atomsnap_gate *gate)`**
- Allocates a version from the internal memory pool
- Returns: Version pointer, or NULL if arena exhausted

**`void atomsnap_set_object(atomsnap_version *ver, void *object, void *free_context)`**
- Sets user object and cleanup context
- Must be called before exchanging the version

**`void atomsnap_free_version(atomsnap_version *version)`**
- Manually frees an unused version
- Use when CAS fails or version creation is aborted
- Calls user's free_impl callback

**`void *atomsnap_get_object(const atomsnap_version *ver)`**
- Retrieves user object from version
- Returns: Object pointer, or NULL if version is NULL

### Reader Operations

**`atomsnap_version *atomsnap_acquire_version(atomsnap_gate *gate)`**
- Atomically acquires current version from slot 0
- Wait-free operation
- Must be paired with `atomsnap_release_version()`

**`atomsnap_version *atomsnap_acquire_version_slot(atomsnap_gate *gate, int slot_idx)`**
- Acquires version from specified slot
- slot_idx: 0 to num_extra_control_blocks

**`void atomsnap_release_version(atomsnap_version *ver)`**
- Releases a previously acquired version
- May trigger version deallocation if reference count reaches zero

### Writer Operations

**`void atomsnap_exchange_version(atomsnap_gate *gate, atomsnap_version *version)`**
- Unconditionally replaces version in slot 0
- Previous version released when all readers finish

**`void atomsnap_exchange_version_slot(atomsnap_gate *gate, int slot_idx, atomsnap_version *version)`**
- Replaces version in specified slot

**`bool atomsnap_compare_exchange_version(atomsnap_gate *gate, atomsnap_version *expected, atomsnap_version *new_ver)`**
- Conditionally replaces version in slot 0
- Succeeds only if current version matches expected
- Returns: true on success, false on failure
- **Critical**: Call `atomsnap_release_version(expected)` AFTER this operation

**`bool atomsnap_compare_exchange_version_slot(atomsnap_gate *gate, int slot_idx, atomsnap_version *expected, atomsnap_version *new_ver)`**
- CAS operation on specified slot

# Usage Guide

## Basic Example

### Problem Statement

Multiple writers need to update a shared object that requires multiple fields to be modified atomically. The object is too large for a single atomic instruction. Readers must always see a fully consistent state—never partially updated values.
```cpp
struct Data {
    int64_t value1;
    int64_t value2;
};
```

### Solution with ATOMSNAP

#### 1. Initialize the Gate
```cpp
void cleanup_data(void *object, void *context) {
    delete static_cast<Data*>(object);
}

atomsnap_init_context ctx = {
    .free_impl = cleanup_data,
    .num_extra_control_blocks = 0  // Single slot
};

atomsnap_gate *gate = atomsnap_init_gate(&ctx);

// Create initial version
atomsnap_version *initial = atomsnap_make_version(gate);
atomsnap_set_object(initial, new Data{0, 0}, nullptr);
atomsnap_exchange_version(gate, initial);
```

#### 2. Reader Implementation
```cpp
void reader_thread() {
    while (running) {
        // Acquire current version (wait-free)
        atomsnap_version *ver = atomsnap_acquire_version(gate);
        Data *data = static_cast<Data*>(atomsnap_get_object(ver));
        
        // Access data safely - always consistent
        assert(data->value1 == data->value2);
        
        // Release version
        atomsnap_release_version(ver);
    }
}
```

#### 3. Writer Implementation (Unconditional Update)
```cpp
void writer_thread_tas() {
    while (running) {
        // Read current version
        atomsnap_version *old_ver = atomsnap_acquire_version(gate);
        Data *old_data = static_cast<Data*>(atomsnap_get_object(old_ver));
        
        // Create new version with updated data
        atomsnap_version *new_ver = atomsnap_make_version(gate);
        Data *new_data = new Data{
            old_data->value1 + 1,
            old_data->value2 + 1
        };
        atomsnap_set_object(new_ver, new_data, nullptr);
        
        // Replace version atomically
        atomsnap_exchange_version(gate, new_ver);
        
        // Release old version
        atomsnap_release_version(old_ver);
    }
}
```

#### 4. Writer Implementation (Conditional Update)
```cpp
void writer_thread_cas() {
    while (running) {
        // Read current version
        atomsnap_version *old_ver = atomsnap_acquire_version(gate);
        Data *old_data = static_cast<Data*>(atomsnap_get_object(old_ver));
        
        // Create new version
        atomsnap_version *new_ver = atomsnap_make_version(gate);
        Data *new_data = new Data{
            old_data->value1 + 1,
            old_data->value2 + 1
        };
        atomsnap_set_object(new_ver, new_data, nullptr);
        
        // Attempt conditional replacement
        if (!atomsnap_compare_exchange_version(gate, old_ver, new_ver)) {
            // CAS failed - free unused version
            atomsnap_free_version(new_ver);
        }
        
        // CRITICAL: Release AFTER CAS to prevent ABA
        atomsnap_release_version(old_ver);
    }
}
```

## Advanced: Multi-Slot Gates

For managing multiple independent version chains:
```cpp
atomsnap_init_context ctx = {
    .free_impl = cleanup_data,
    .num_extra_control_blocks = 3  // 4 total slots (0-3)
};

atomsnap_gate *gate = atomsnap_init_gate(&ctx);

// Use different slots for different purposes
atomsnap_version *ver_slot0 = atomsnap_acquire_version_slot(gate, 0);
atomsnap_version *ver_slot1 = atomsnap_acquire_version_slot(gate, 1);

// Independent updates
atomsnap_exchange_version_slot(gate, 0, new_version0);
atomsnap_exchange_version_slot(gate, 1, new_version1);
```

# Common Pitfalls

## ABA Problem with CAS

**Wrong**:
```cpp
atomsnap_version *old_ver = atomsnap_acquire_version(gate);
atomsnap_version *new_ver = atomsnap_make_version(gate);
// ... prepare new_ver ...

atomsnap_release_version(old_ver);  // ❌ Released too early!
if (!atomsnap_compare_exchange_version(gate, old_ver, new_ver)) {
    atomsnap_free_version(new_ver);
}
```

**Correct**:
```cpp
atomsnap_version *old_ver = atomsnap_acquire_version(gate);
atomsnap_version *new_ver = atomsnap_make_version(gate);
// ... prepare new_ver ...

if (!atomsnap_compare_exchange_version(gate, old_ver, new_ver)) {
    atomsnap_free_version(new_ver);
}
atomsnap_release_version(old_ver);  // ✓ Released after CAS
```

**Why?** Releasing old_ver before CAS allows its handle to be recycled. If another thread creates a new version with the same handle, CAS may incorrectly succeed.

## Memory Leak on CAS Failure

**Wrong**:
```cpp
atomsnap_version *new_ver = atomsnap_make_version(gate);
// ... prepare new_ver ...

if (!atomsnap_compare_exchange_version(gate, old_ver, new_ver)) {
    // ❌ new_ver is leaked!
}
```

**Correct**:
```cpp
atomsnap_version *new_ver = atomsnap_make_version(gate);
// ... prepare new_ver ...

if (!atomsnap_compare_exchange_version(gate, old_ver, new_ver)) {
    atomsnap_free_version(new_ver);  // ✓ Explicitly freed
}
```

## Unbalanced Acquire/Release

**Wrong**:
```cpp
atomsnap_version *ver1 = atomsnap_acquire_version(gate);
atomsnap_version *ver2 = atomsnap_acquire_version(gate);  // ❌ Nested acquire
// ... use ver1 and ver2 ...
atomsnap_release_version(ver1);
atomsnap_release_version(ver2);
```

**Correct**:
```cpp
atomsnap_version *ver1 = atomsnap_acquire_version(gate);
// ... use ver1 ...
atomsnap_release_version(ver1);

atomsnap_version *ver2 = atomsnap_acquire_version(gate);  // ✓ After release
// ... use ver2 ...
atomsnap_release_version(ver2);
```

---

# Performance Comparison

## Environment

- **CPU**: Intel Core i5-13400F (16 cores)
- **RAM**: 16GB DDR5 5600MHz
- **OS**: Ubuntu 24.04.1 LTS
- **Compiler**: GCC 13.3.0

## Benchmark 1: atomsnap vs std::shared_ptr / mutex / spinlock

**Config**
- Duration: 100 seconds per test
- Payload: `struct Data { int64_t value1; int64_t value2; }` (16 bytes)

**Invariant**
- Readers verify `value1 == value2` on every read.
- In CAS tests, readers also verify monotonicity (value never decreases).

**Measurement**
- All threads start together via a barrier.
- Reader op: acquire → read/verify → release.
- Writer op (TAS): one `exchange` operation.
- Writer op (CAS): one successful `compare_exchange`; failed attempts are not counted.
- Throughput = total ops / duration.

**Baselines**

| Baseline | Update Pattern | Experiments |
|----------|----------------|-------------|
| `atomsnap` | copy-on-write (allocate new version, publish atomically) | TAS, CAS |
| `std::shared_ptr` | copy-on-write (allocate new object, atomic store/CAS) | TAS, CAS |
| `shared_mutex` | in-place update (exclusive lock for write, shared lock for read) | CAS only |
| `spinlock` | in-place update (exclusive lock for both read and write) | CAS only |

Note: `shared_mutex` and `spinlock` modify the existing object directly under lock, while `atomsnap` and `shared_ptr` allocate a new object for each update. This difference affects allocation overhead and cache behavior.

### Experiment A: TAS (unconditional exchange)

Reader Throughput (ops/sec)

| Readers/Writers | std::shared_ptr | atomsnap   |
|:---------------:|:---------------:|:----------:|
| 1/1             | 4,636,111       | 8,867,808  |
| 2/2             | 4,645,677       | 10,974,547 |
| 4/4             | 2,995,396       | 14,354,099 |
| 8/8             | 2,787,014       | 16,246,219 |

Writer Throughput (ops/sec)

| Readers/Writers | std::shared_ptr | atomsnap  |
|:---------------:|:---------------:|:---------:|
| 1/1             | 2,158,231       | 4,206,934 |
| 2/2             | 2,098,231       | 5,623,703 |
| 4/4             | 1,273,549       | 6,646,504 |
| 8/8             | 1,193,263       | 7,416,587 |

**Observations (TAS)**

- `atomsnap` reader throughput increases with thread count (8.9M → 16.2M). `shared_ptr` reader throughput decreases (4.6M → 2.8M).
- `atomsnap` writer throughput increases with thread count (4.2M → 7.4M). `shared_ptr` writer throughput decreases (2.2M → 1.2M).
- At 8/8 configuration, `atomsnap` achieves 5.8x reader throughput and 6.2x writer throughput compared to `shared_ptr`.

### Experiment B: CAS (conditional compare_exchange with retry)

Reader Throughput (ops/sec)

| Readers/Writers | shared_mutex | shared_ptr | spinlock   | atomsnap   |
|:---------------:|:------------:|:----------:|:----------:|:----------:|
| 1/1             | 483,073      | 4,311,094  | 13,861,844 | 8,701,174  |
| 2/2             | 10,806,981   | 4,550,829  | 7,474,572  | 10,893,707 |
| 4/4             | 13,945,730   | 3,029,803  | 4,546,094  | 15,158,793 |
| 8/8             | 17,149,214   | 2,303,844  | 4,055,162  | 18,533,507 |

Writer Throughput (ops/sec)

| Readers/Writers | shared_mutex | shared_ptr | spinlock   | atomsnap  |
|:---------------:|:------------:|:----------:|:----------:|:---------:|
| 1/1             | 395,415      | 2,300,239  | 13,621,219 | 4,352,176 |
| 2/2             | 150,856      | 1,759,203  | 5,589,843  | 3,761,673 |
| 4/4             | 27,958       | 972,971    | 3,962,019  | 2,894,568 |
| 8/8             | 8,882        | 1,054,694  | 2,639,648  | 2,091,873 |

**Observations (CAS)**

- Reader throughput at 1/1: `spinlock` (13.9M) > `atomsnap` (8.7M) > `shared_ptr` (4.3M) > `shared_mutex` (0.5M).
- Reader throughput at 8/8: `atomsnap` (18.5M) > `shared_mutex` (17.1M) > `spinlock` (4.1M) > `shared_ptr` (2.3M).
- Writer throughput: `spinlock` is highest across all configurations. At 8/8: `spinlock` (2.6M) > `atomsnap` (2.1M) > `shared_ptr` (1.1M) > `shared_mutex` (0.009M).
- `spinlock` reader throughput decreases from 13.9M to 4.1M as thread count increases. `atomsnap` and `shared_mutex` reader throughput increase.
- `shared_mutex` writer throughput drops to near-zero (8,882 ops/sec) at 8/8 due to writer starvation under read-heavy contention.

### Experiment C: Unbalanced workloads (CAS)

Configuration: 1 Reader, 16 Writers

| Metric          | shared_mutex | shared_ptr | spinlock | atomsnap  |
|:----------------|:------------:|:----------:|:--------:|:---------:|
| Reader ops/sec  | 8,620,295    | 301,761    | 347,163  | 2,374,780 |
| Writer ops/sec  | 630,098      | 1,751,873  | 6,251,655| 2,957,111 |

Configuration: 16 Readers, 1 Writer

| Metric          | shared_mutex | shared_ptr | spinlock  | atomsnap   |
|:----------------|:------------:|:----------:|:---------:|:----------:|
| Reader ops/sec  | 18,221,686   | 5,168,143  | 5,079,128 | 36,763,930 |
| Writer ops/sec  | 1            | 134,753    | 328,145   | 517,201    |

**Observations (Unbalanced)**

- 1R/16W: `shared_mutex` provides highest reader throughput (8.6M) due to reader priority in `rwlock` semantics. `spinlock` provides highest writer throughput (6.3M).
- 16R/1W: `atomsnap` provides highest reader throughput (36.8M), 2x higher than the next (`shared_mutex` 18.2M). `atomsnap` also provides highest writer throughput (517K).
- `shared_mutex` writer throughput drops to 1 op/sec at 16R/1W, indicating complete writer starvation.
- `spinlock` and `shared_ptr` show similar reader throughput (~5M) at 16R/1W.

### Benchmark 1 Summary

| Scenario | Highest Reader Throughput | Highest Writer Throughput |
|----------|---------------------------|---------------------------|
| TAS (all configs) | atomsnap | atomsnap |
| CAS 1/1 | spinlock | spinlock |
| CAS 8/8 | atomsnap | spinlock |
| 1R/16W | shared_mutex | spinlock |
| 16R/1W | atomsnap | atomsnap |

Trade-offs:
- **spinlock**: Highest writer throughput in CAS scenarios. Reader throughput degrades under high contention. Not suitable for read-heavy workloads with many threads.
- **shared_mutex**: Good reader scaling under contention. Severe writer starvation in read-heavy scenarios (writer throughput approaches zero).
- **shared_ptr**: Moderate throughput. Does not scale well with thread count. Throughput decreases as contention increases.
- **atomsnap**: Consistent scaling for both readers and writers. Highest reader throughput in read-heavy scenarios. Writer throughput is lower than spinlock but does not starve.

## Benchmark 2: atomsnap vs liburcu (RCU, urcu-memb)

This benchmark compares `atomsnap` against `liburcu` (`urcu-memb` flavor).

**Comparison dimensions**
- Read-side latency (per-op overhead)
- Writer throughput under reader stalls
- Memory usage (RSS) under varying conditions

**Terminology**

| Term | Description |
|------|-------------|
| `payload` | Size (bytes) of user data allocated per update. |
| `shards` | Number of independent version chains. Reduces contention on a single control block. atomsnap uses multi-slot gates; urcu uses an array of pointers. |
| `CS` | Critical section delay. Simulates work done while holding a version. |

**Output columns**
- `r_ops_s`: reader throughput (ops/sec)
- `w_ops_s`: writer throughput (ops/sec)
- `lat_avg_ns`: average per-op latency in nanoseconds (excluding CS delay)
- `peak_rss_kb`: peak resident set size (KB)

### Experiment A: Reader critical-section delay sweep

Config:
- readers=16, writers=1, duration=15s
- payload=64 bytes
- updates/s = unlimited
- critical-section(cs) delay: 0us, 10us, 100us
- `urcu` reclamation: async (`call_rcu`)

#### A-1) shards=1

| CS(us) | urcu r(M/s) | urcu w(M/s) | urcu RSS(MB) | urcu lat(us) | atomsnap r(M/s) | atomsnap w(M/s) | atomsnap RSS(MB) | atomsnap lat(us) |
|---|---|---|---|---|---|---|---|---|
| 0 | 28.68 | 0.545 | 12.9 | 0.085 | 27.37 | 0.428 | 20.6 | 0.475 |
| 10 | 1.69 | 2.832 | 94.4 | 9.666 | 1.77 | 3.879 | 20.6 | 9.361 |
| 100 | 0.18 | 3.063 | 94.0 | 86.819 | 0.18 | 5.763 | 20.8 | 88.186 |

#### A-2) shards=8

| CS(us) | urcu r(M/s) | urcu w(M/s) | urcu RSS(MB) | urcu lat(us) | atomsnap r(M/s) | atomsnap w(M/s) | atomsnap RSS(MB) | atomsnap lat(us) |
|---|---|---|---|---|---|---|---|---|
| 0 | 48.29 | 0.751 | 18.5 | 0.099 | 35.42 | 0.632 | 20.8 | 0.237 |
| 10 | 1.73 | 2.929 | 78.0 | 9.310 | 1.74 | 3.806 | 20.6 | 9.303 |
| 100 | 0.18 | 3.145 | 129.9 | 87.458 | 0.18 | 5.833 | 20.8 | 90.497 |

**Observations (Experiment A)**

Read-side overhead (CS=0us):
- `urcu` read latency: 0.085us (shards=1), 0.099us (shards=8).
- `atomsnap` read latency: 0.475us (shards=1), 0.237us (shards=8).
- `urcu` read-side overhead is 5.6x lower than `atomsnap` at shards=1.
- `urcu` reader throughput: 28.7M (shards=1) → 48.3M (shards=8), a 1.7x increase.
- `atomsnap` reader throughput: 27.4M (shards=1) → 35.4M (shards=8), a 1.3x increase.

Memory usage under reader stalls:
- `urcu` RSS at CS=0us: 12.9MB (shards=1), 18.5MB (shards=8).
- `urcu` RSS at CS=100us: 94.0MB (shards=1), 129.9MB (shards=8).
- `atomsnap` RSS: 20.6–20.8MB across all CS values and shard configurations.
- `urcu` RSS increases 7.3x (shards=1) and 7.0x (shards=8) as CS increases from 0us to 100us.
- `atomsnap` RSS remains constant regardless of CS delay.

Writer throughput under reader stalls:
- At CS=100us (shards=1): `atomsnap` 5.76M/s, `urcu` 3.06M/s. `atomsnap` is 1.9x higher.
- At CS=100us (shards=8): `atomsnap` 5.83M/s, `urcu` 3.15M/s. `atomsnap` is 1.9x higher.

### Experiment B: Writer rate limiting sweep

Config:
- readers=16, writers=1, duration=15s
- critical-section(cs) delay = 200ns
- payload=64 bytes
- updates/s throttled: 100k, 500k, 1M, 2M, and unlimited
- `urcu` reclamation: async (`call_rcu`)

#### B-1) shards=1

| updates/s | urcu r(M/s) | urcu w(M/s) | urcu lat(us) | urcu RSS(MB) | atomsnap r(M/s) | atomsnap w(M/s) | atomsnap lat(us) | atomsnap RSS(MB) |
|---|---|---|---|---|---|---|---|---|
| unlimited | 31.22 | 1.339 | 0.438 | 34.5 | 20.54 | 0.711 | 0.772 | 20.6 |
| 100k | 34.43 | 0.100 | 0.415 | 5.0 | 16.45 | 0.100 | 0.873 | 20.5 |
| 500k | 32.82 | 0.500 | 0.415 | 18.9 | 20.89 | 0.500 | 0.675 | 20.6 |
| 1M | 21.58 | 0.868 | 0.588 | 17.0 | 20.29 | 0.680 | 0.746 | 20.6 |
| 2M | 20.98 | 0.903 | 0.585 | 26.0 | 20.78 | 0.646 | 0.699 | 20.8 |

#### B-2) shards=8

| updates/s | urcu r(M/s) | urcu w(M/s) | urcu lat(us) | urcu RSS(MB) | atomsnap r(M/s) | atomsnap w(M/s) | atomsnap lat(us) | atomsnap RSS(MB) |
|---|---|---|---|---|---|---|---|---|
| unlimited | 23.66 | 1.237 | 0.561 | 31.8 | 17.85 | 0.832 | 0.675 | 20.8 |
| 100k | 22.57 | 0.100 | 0.571 | 5.4 | 29.09 | 0.100 | 0.482 | 20.6 |
| 500k | 24.34 | 0.500 | 0.605 | 17.0 | 28.31 | 0.500 | 0.527 | 20.6 |
| 1M | 30.91 | 0.994 | 0.473 | 28.1 | 20.38 | 0.843 | 0.631 | 20.6 |
| 2M | 21.65 | 0.847 | 0.571 | 26.5 | 20.69 | 0.903 | 0.622 | 20.6 |

**Observations (Experiment B)**

Memory usage vs update rate:
- `urcu` RSS at 100k updates/s: 5.0MB (shards=1), 5.4MB (shards=8).
- `urcu` RSS at unlimited: 34.5MB (shards=1), 31.8MB (shards=8).
- `atomsnap` RSS: 20.5–20.8MB across all update rates.
- `urcu` RSS varies by 6.9x between 100k and unlimited (shards=1). `atomsnap` RSS is constant.

Reader throughput vs update rate (shards=8):
- `urcu` at 100k: 22.6M/s. `urcu` at unlimited: 23.7M/s.
- `atomsnap` at 100k: 29.1M/s. `atomsnap` at unlimited: 17.9M/s.
- `atomsnap` reader throughput is 1.3x higher than `urcu` at 100k updates/s.
- `urcu` reader throughput is 1.3x higher than `atomsnap` at unlimited updates/s.

### Benchmark 2 Summary

| Metric | urcu | atomsnap |
|--------|------|----------|
| Read-side latency (CS=0us) | 0.085–0.099us | 0.237–0.475us |
| Reader throughput (CS=0us, shards=8) | 48.3M/s | 35.4M/s |
| Writer throughput (CS=100us) | 3.1M/s | 5.8M/s |
| RSS at CS=0us | 12.9–18.5MB | 20.6–20.8MB |
| RSS at CS=100us | 94.0–129.9MB | 20.6–20.8MB |
| RSS variation with CS delay | 7.0–7.3x increase | None |
| RSS variation with update rate | 5.0–34.5MB | 20.5–20.8MB |

Trade-offs:
- **urcu**: Lower read-side latency (5.6x lower than `atomsnap` at shards=1). Higher reader throughput at CS=0us (1.4x higher at shards=8). Memory usage increases with reader critical section length and update rate.
- **atomsnap**: Higher read-side latency due to atomic refcount operations. Constant memory usage regardless of reader critical section length or update rate. Higher writer throughput when readers hold versions for extended periods (1.9x higher at CS=100us).

---
