# ATOMSNAP

This library is a lock-free concurrency primitive for managing shared objects with multiple versions. It allows multiple readers and writers to access a shared pointer simultaneously without blocking, ensuring system-wide progress and low latency.

### Use Cases

- Objects are too large for single atomic instructions (>8 bytes).
- Readers access immutable snapshots while writers create new versions.

## Critical Usage Rules

**[Planned]: Features or constraints targeted for optimization or removal in future releases.**

- **MAX_THREADS**: This library supports a global maximum of 1,048,576 threads.

**[Fixed]: Permanent architectural constraints or safety policies that will remain unchanged.**

- **Acquire-Release Pairing**: Every `atomsnap_acquire_version()` must have a matching `atomsnap_release_version()`.
- **No Nested Acquires**: Do not acquire multiple versions without releasing previous ones.
- **CAS Ordering**: When using `atomsnap_compare_exchange_version()`, always call `atomsnap_release_version()` AFTER the CAS operation to prevent ABA problems.
- **Failed CAS Cleanup**: When CAS fails, manually free the unused version with `atomsnap_free_version()` or retry CAS with that version to prevent memory leaks.

---

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

---

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

---

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

| Readers/Writers | std::shared_ptr | atomsnap   | urcu (memb)  |
|:---------------:|:---------------:|:----------:|:------------:|
| 1/1             | 4,118,869       | 8,867,808  | 22,938,519   |
| 2/2             | 3,919,675       | 10,974,547 | 75,880,492   |
| 4/4             | 3,157,681       | 14,354,099 | 153,899,850  |
| 8/8             | 2,421,110       | 16,246,219 | 265,617,635  |

### Writer Throughput (ops/sec)

| Readers/Writers | std::shared_ptr | atomsnap  | urcu (memb) |
|:---------------:|:---------------:|:---------:|:-----------:|
| 1/1             | 2,290,178       | 4,206,934 | 327,205     |
| 2/2             | 1,874,746       | 5,623,703 | 130,919     |
| 4/4             | 1,419,709       | 6,646,504 | 98,408      |
| 8/8             | 1,122,508       | 7,416,587 | 58,712      |

## Benchmark 2: Stateful CAS (16 bytes)

Writers use conditional exchange with retry.

### Reader Throughput (ops/sec)

| Readers/Writers | shared_mutex | shared_ptr | spinlock   | atomsnap   | urcu (memb) |
|:---------------:|:------------:|:----------:|:----------:|:----------:|:-----------:|
| 1/1             | 483,073      | 4,168,734  | 13,861,844 | 8,701,174  | 22,389,676  |
| 2/2             | 10,806,981   | 3,790,341  | 7,474,572  | 10,893,707 | 75,936,437  |
| 4/4             | 13,945,730   | 3,082,837  | 4,546,094  | 15,158,793 | 158,888,978 |
| 8/8             | 17,149,214   | 2,567,489  | 4,055,162  | 18,533,507 | 276,082,043 |

### Writer Throughput (ops/sec)

| Readers/Writers | shared_mutex | shared_ptr | spinlock   | atomsnap  | urcu (memb) |
|:---------------:|:------------:|:----------:|:----------:|:---------:|:-----------:|
| 1/1             | 395,415      | 2,152,445  | 13,621,219 | 4,352,176 | 317,286     |
| 2/2             | 150,856      | 1,444,993  | 5,589,843  | 3,761,673 | 131,496     |
| 4/4             | 27,958       | 1,103,961  | 3,962,019  | 2,894,568 | 95,164      |
| 8/8             | 8,882        | 528,600    | 2,639,648  | 2,091,873 | 61,349      |

## Benchmark 3: Unbalanced Workloads (CAS, 16 bytes)

Tests extreme reader/writer ratios.

### Configuration: 1 Reader, 16 Writers

| Metric          | shared_mutex | shared_ptr | spinlock | atomsnap  | urcu (memb) |
|:----------------|:------------:|:----------:|:--------:|:---------:|:-----------:|
| Reader ops/sec  | 8,620,295    | 234,983    | 347,163  | 2,374,780 | 40,273,630  |
| Writer ops/sec  | 630,098      | 1,618,858  | 6,251,655| 2,957,111 | 108,184     |

### Configuration: 16 Readers, 1 Writer

| Metric          | shared_mutex | shared_ptr | spinlock  | atomsnap   | urcu (memb) |
|:----------------|:------------:|:----------:|:---------:|:----------:|:-----------:|
| Reader ops/sec  | 18,221,686   | 5,208,506  | 5,079,128 | 36,763,930 | 354,783,087 |
| Writer ops/sec  | 1            | 129,827    | 328,145   | 517,201    | 49,727      |

---
