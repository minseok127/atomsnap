# ATOMSNAP

This library is a wait-free, and lock-free concurrency primitive for managing shared object with multiple versions. It allows multiple readers and writers to access a shared pointer simultaneously without blocking, ensuring system-wide progress and low latency.

### Key Features

- **Wait-Free Reads**: Readers acquire object versions without blocking or spinning.
- **Lock-Free Writes**: Writers update objects using atomic operations (TAS/CAS).
- **Memory Safety**: Automatic garbage collection through reference counting.
- **Version Consistency**: Readers never observe partially updated objects.

### Use Cases

- Objects are too large for single atomic instructions (>8 bytes).
- Readers require consistent snapshots without tearing.

## Critical Usage Rules

1. **Acquire-Release Pairing**: Every `atomsnap_acquire_version()` must have a matching `atomsnap_release_version()`.
2. **No Nested Acquires**: Do not acquire multiple versions without releasing previous ones.
3. **CAS Ordering**: When using `atomsnap_compare_exchange_version()`, always call `atomsnap_release_version()` AFTER the CAS operation to prevent ABA problems.
4. **Failed CAS Cleanup**: When CAS fails, manually free the unused version with `atomsnap_free_version()` or retry CAS with that version to prevent memory leaks.
5. **MAX_THREADS**: This library supports a global maximum of 1,048,575 threads.

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
This ensures that reading the pointer and incrementing the reference count happens in a single atomic instruction.

```
+----------------+---------------------------------------+
| RefCount (24b) |             Handle (40b)              |
+----------------+---------------------------------------+
|    Acquires    |       Thread ID | Arena ID | Slot ID  |
+----------------+---------------------------------------+
```

- Reference Count (24-bit): Tracks the number of Acquires (active readers) for the current version.
- Handle (40-bit): A unique identifier for the `atomsnap_version`.

### 2. Handle Structure (40-bit)

The 40-bit handle uniquely identifies a version slot within the global memory space.

- Thread ID (20-bit): Identifies the owner thread (0 ~ 1,048,574).
- Arena ID (6-bit): Identifies the specific arena within the thread's context (0 ~ 63).
- Slot ID (14-bit): Identifies the slot index within the arena (0 ~ 16,382).

Note: MAX_THREADS is limited to $1,048,575$ to prevent the handle from conflicting with the HANDLE_NULL value (all bits set to 1).

### 3. Memory Arena & Page Alignment

Memory is managed in Arenas, which are contiguous blocks of pre-allocated slots.

- Page Alignment: Each arena is designed to fit perfectly within 160 memory pages (4KB pages).
- `SLOTS_PER_ARENA` is set to 16,383.
    - Formula: $8\text{B (Header)} + (16,383 \times 40\text{B}) = 655,328\text{ Bytes}$
    - Capacity: $160 \times 4096\text{B} = 655,360\text{ Bytes}$
    - Wasted Space: Only 32 Bytes per arena (0.004% fragmentation).

**Free List Design**:
- Thread-local batch for fast allocation (wait-free)
- Arena-level shared list for cross-thread recycling
- Sentinel-based wait-free push operation

**Lifecycle**:
1. Writer allocates version from thread-local arena
2. Writer sets object and exchanges version atomically
3. Readers increment outer reference count on acquire (control block)
4. Writer decrements inner reference count on exchange (by outer count value)
5. Readers increment inner reference count on release
6. Version freed when inner reference count reaches zero

## Reference Counting Algorithm

Atomsnap uses a dual-counter approach to manage object lifecycles safely without locks.

1. **Outer Reference Count (24-bit)**: Located in the Control Block. Counts the number of times a version has been Acquired.
2. **Inner Reference Count (32-bit)**: Located in the Version object. Counts the number of times a version has been Released.

### The Mismatch Problem & Solution

Since the Outer counter (24-bit) and Inner counter (32-bit) have different bit-widths, simply subtracting them would lead to errors. Atomsnap solves this with a robust normalization logic:

- **Masking**: The Inner counter is masked to the 24-bit domain.
- **Subtraction**: Active Readers = (Inner Releases - Outer Acquires).
- **Wraparound Correction**: If the outer counter wraps around relative to the inner counter, the algorithm detects the anomaly (positive result) and applies a WRAPAROUND_FACTOR ($2^{24}$) correction.

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
| 1/1             | 4,118,869       | 15,364,317 | 22,938,519   |
| 2/2             | 3,919,675       | 12,911,508 | 75,880,492   |
| 4/4             | 3,157,681       | 17,237,426 | 153,899,850  |
| 8/8             | 2,421,110       | 18,977,734 | 265,617,635  |
| 16/16           | 2,914,017       | 19,803,148 | 382,315,056  |

### Writer Throughput (ops/sec)

| Readers/Writers | std::shared_ptr | atomsnap  | urcu (memb) |
|:---------------:|:---------------:|:---------:|:-----------:|
| 1/1             | 2,290,178       | 6,536,867 | 327,205     |
| 2/2             | 1,874,746       | 5,934,981 | 130,919     |
| 4/4             | 1,419,709       | 7,147,858 | 98,408      |
| 8/8             | 1,122,508       | 7,605,056 | 58,712      |
| 16/16           | 1,356,290       | 7,342,350 | 3,763       |

## Benchmark 2: Stateful CAS (16 bytes)

Writers use conditional exchange with retry.

### Reader Throughput (ops/sec)

| Readers/Writers | shared_mutex | shared_ptr | spinlock   | atomsnap   | urcu (memb) |
|:---------------:|:------------:|:----------:|:----------:|:----------:|:-----------:|
| 1/1             | 483,073      | 4,168,734  | 13,861,844 | 13,345,141 | 22,389,676  |
| 2/2             | 10,806,981   | 3,790,341  | 7,474,572  | 14,037,157 | 75,936,437  |
| 4/4             | 13,945,730   | 3,082,837  | 4,546,094  | 17,340,585 | 158,888,978 |
| 8/8             | 17,149,214   | 2,567,489  | 4,055,162  | 19,344,667 | 276,082,043 |
| 16/16           | 17,000,505   | 2,372,081  | 1,608,919  | 21,623,425 | 377,963,931 |

### Writer Throughput (ops/sec)

| Readers/Writers | shared_mutex | shared_ptr | spinlock   | atomsnap  | urcu (memb) |
|:---------------:|:------------:|:----------:|:----------:|:---------:|:-----------:|
| 1/1             | 395,415      | 2,152,445  | 13,621,219 | 6,104,996 | 317,286     |
| 2/2             | 150,856      | 1,444,993  | 5,589,843  | 3,325,338 | 131,496     |
| 4/4             | 27,958       | 1,103,961  | 3,962,019  | 2,238,743 | 95,164      |
| 8/8             | 8,882        | 528,600    | 2,639,648  | 1,344,667 | 61,349      |
| 16/16           | 11           | 693,230    | 1,639,855  | 1,324,588 | 3,811       |

## Benchmark 3: Unbalanced Workloads (CAS, 16 bytes)

Tests extreme reader/writer ratios.

### Configuration: 1 Reader, 16 Writers

| Metric          | shared_mutex | shared_ptr | spinlock | atomsnap  | urcu (memb) |
|:----------------|:------------:|:----------:|:--------:|:---------:|:-----------:|
| Reader ops/sec  | 8,620,295    | 234,983    | 347,163  | 2,796,784 | 40,273,630  |
| Writer ops/sec  | 630,098      | 1,618,858  | 6,251,655| 1,929,570 | 108,184     |

### Configuration: 16 Readers, 1 Writer

| Metric          | shared_mutex | shared_ptr | spinlock  | atomsnap   | urcu (memb) |
|:----------------|:------------:|:----------:|:---------:|:----------:|:-----------:|
| Reader ops/sec  | 18,221,686   | 5,208,506  | 5,079,128 | 43,766,774 | 354,783,087 |
| Writer ops/sec  | 1            | 129,827    | 328,145   | 549,203    | 49,727      |

---
