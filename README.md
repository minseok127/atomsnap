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
> cannot cause premature reclamation because reclamation is gated by DETACHED
> and finalized via FINALIZED.

### 5. Reclamation Algorithm (Outer RefCount + Inner State)

**Lifecycle**:

1. Writer allocates a version from the arena.
2. Writer sets the payload pointer and publishes it via exchange / CAS.
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
- **Duration**: 100 seconds per test

## Benchmark 1: Stateless TAS (16 bytes)

Writers use unconditional exchange.

### Reader Throughput (ops/sec)

| Readers/Writers | std::shared_ptr | atomsnap   |
|:---------------:|:---------------:|:----------:|
| 1/1             | 4,118,869       | 8,867,808  |
| 2/2             | 3,919,675       | 10,974,547 |
| 4/4             | 3,157,681       | 14,354,099 |
| 8/8             | 2,421,110       | 16,246,219 |

### Writer Throughput (ops/sec)

| Readers/Writers | std::shared_ptr | atomsnap  |
|:---------------:|:---------------:|:---------:|
| 1/1             | 2,290,178       | 4,206,934 |
| 2/2             | 1,874,746       | 5,623,703 |
| 4/4             | 1,419,709       | 6,646,504 |
| 8/8             | 1,122,508       | 7,416,587 |

## Benchmark 2: Stateful CAS (16 bytes)

Writers use conditional exchange with retry.

### Reader Throughput (ops/sec)

| Readers/Writers | shared_mutex | shared_ptr | spinlock   | atomsnap   |
|:---------------:|:------------:|:----------:|:----------:|:----------:|
| 1/1             | 483,073      | 4,168,734  | 13,861,844 | 8,701,174  |
| 2/2             | 10,806,981   | 3,790,341  | 7,474,572  | 10,893,707 |
| 4/4             | 13,945,730   | 3,082,837  | 4,546,094  | 15,158,793 |
| 8/8             | 17,149,214   | 2,567,489  | 4,055,162  | 18,533,507 |

### Writer Throughput (ops/sec)

| Readers/Writers | shared_mutex | shared_ptr | spinlock   | atomsnap  |
|:---------------:|:------------:|:----------:|:----------:|:---------:|
| 1/1             | 395,415      | 2,152,445  | 13,621,219 | 4,352,176 |
| 2/2             | 150,856      | 1,444,993  | 5,589,843  | 3,761,673 |
| 4/4             | 27,958       | 1,103,961  | 3,962,019  | 2,894,568 |
| 8/8             | 8,882        | 528,600    | 2,639,648  | 2,091,873 |

## Benchmark 3: Unbalanced Workloads (CAS, 16 bytes)

Tests extreme reader/writer ratios.

### Configuration: 1 Reader, 16 Writers

| Metric          | shared_mutex | shared_ptr | spinlock | atomsnap  |
|:----------------|:------------:|:----------:|:--------:|:---------:|
| Reader ops/sec  | 8,620,295    | 234,983    | 347,163  | 2,374,780 |
| Writer ops/sec  | 630,098      | 1,618,858  | 6,251,655| 2,957,111 |

### Configuration: 16 Readers, 1 Writer

| Metric          | shared_mutex | shared_ptr | spinlock  | atomsnap   |
|:----------------|:------------:|:----------:|:---------:|:----------:|
| Reader ops/sec  | 18,221,686   | 5,208,506  | 5,079,128 | 36,763,930 |
| Writer ops/sec  | 1            | 129,827    | 328,145   | 517,201    |

## Benchmark 4: atomsnap vs liburcu (RCU, urcu-memb)

This benchmark compares atomsnap (refcount + bounded arena reuse) against
Userspace RCU (liburcu, memb flavor) under a single-writer / many-readers
workload. The goal is to quantify the trade-offs between:

- **Read-side cost** (fast path overhead)
- **Write-side cost** (publish + reclamation pressure)
- **Memory behavior** (peak RSS under reader stalls / deferred frees)

### What "payload" means

`payload` is the size (bytes) of the user data copied/initialized per update.
It models the cost of producing a new immutable snapshot. Larger payload means
more per-update work and more memory traffic.

### What "shards" means

`shards` is the number of independent version chains. Readers/writers map to a
shard (e.g., by thread id / hash) to reduce cache-line contention on a single
shared pointer / control block.

- atomsnap: uses multi-slot gates (multiple control blocks).
- urcu: uses an array of `Data*` pointers, one per shard.

### Output columns (CSV)

- `r_ops_s`: reader ops/sec (measured actual throughput)
- `w_ops_s`: writer ops/sec (measured actual throughput)
- `lat_avg_ns`: average per-op overhead in ns (excluding the simulated CS delay)
- `peak_rss_kb`: process peak RSS (KB)

### Experiment A: Reader critical-section delay sweep (payload=64)

Config:
- readers=16, writers=1, duration=15s
- updates/s = unlimited (writer runs as fast as it can)
- payload=64 bytes
- async reclamation for urcu (call_rcu), normal atomsnap reclamation
- cs delay: 0us, 10us, 100us
- reported as Mops/s and MB

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

**How to read Experiment A**

- **CS=0us (pure overhead)**:
  - urcu has a very cheap read-side fast path and scales strongly with sharding.
  - atomsnap is also fast, but has a higher per-op overhead due to refcounting.

- **CS grows (10us/100us)**:
  - urcu peak RSS rises significantly: long reader CS delays the grace period,
    so callbacks accumulate and memory pressure grows.
  - atomsnap peak RSS stays almost flat (~20–21MB in these runs), because freed
    slots are recycled through arenas and reclamation is tied to refcount drain.

- **Writer throughput under stalls**:
  - with long CS, atomsnap shows higher w_ops_s than urcu in these runs.
    Intuition: urcu’s async callback path is fast, but prolonged grace periods
    create backlog and memory traffic; atomsnap pays per-op atomic refcount cost
    but stays stable in memory.

### Experiment B: Writer rate limiting sweep (payload=64, cs=0)

Config:
- readers=16, writers=1, duration=15s
- cs_ns = 0 (no simulated CS work)
- payload=64 bytes
- updates/s throttled: 100k, 500k, 1M, 2M (and unlimited)
- goal: isolate memory effects when the writer is intentionally paced

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

**How to read Experiment B**

- Rate limiting largely removes the “writer outruns reclamation” effect for urcu:
  at low update rates (100k/s), urcu RSS drops sharply.
- atomsnap RSS is relatively flat across rates in these runs, reflecting the
  arena reuse behavior.
- Throughput can shift with sharding: with `shards=8` and throttled writer,
  atomsnap readers become very strong at 100k–500k updates/s, consistent with
  reduced contention on a single hot control block.

### Summary: Practical trade-offs

- If your priority is **maximum read throughput** and you can tolerate memory
  growth under long reader critical sections, **urcu** is hard to beat.
- If you care about **predictable memory** (stable RSS) and **bounded behavior**
  under reader stalls, **atomsnap** is attractive, at the cost of higher
  read-side overhead (refcounting) and slightly higher baseline latency.
- `shards` is worth enabling on both: it usually reduces contention and can
  materially improve throughput, but it also changes cache locality and may
  shift the optimal point depending on update rate and CPU topology.

---
