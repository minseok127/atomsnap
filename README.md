# ATOMSNAP

A wait-free versioned object management library for multi-threaded environments.

## Overview

ATOMSNAP enables multiple writers to safely modify shared objects while guaranteeing readers always see consistent snapshots. Unlike traditional locking mechanisms, ATOMSNAP achieves this through version management with wait-free reader access and lock-free writer updates.

### Key Features

- **Wait-Free Reads**: Readers acquire object versions without blocking or spinning
- **Lock-Free Writes**: Writers update objects using atomic operations (TAS/CAS)
- **Memory Safety**: Automatic garbage collection through reference counting
- **Version Consistency**: Readers never observe partially updated objects

### Use Cases

ATOMSNAP is designed for scenarios where:
- Objects are too large for single atomic instructions (>8 bytes)
- Readers require consistent snapshots without tearing

## Critical Usage Rules

1. **Acquire-Release Pairing**: Every `atomsnap_acquire_version()` must have a matching `atomsnap_release_version()`
2. **No Nested Acquires**: Do not acquire multiple versions without releasing previous ones
3. **Reference Count Limit**: If outer reference count exceeds 4,294,967,295 (UINT32_MAX), behavior is undefined
4. **CAS Ordering**: When using `atomsnap_compare_exchange_version()`, always call `atomsnap_release_version()` AFTER the CAS operation to prevent ABA problems
5. **Failed CAS Cleanup**: When CAS fails, manually free the unused version with `atomsnap_free_version()` or reuse the version to prevent memory leaks

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

### Handle-Based Design

ATOMSNAP uses 32-bit handles instead of direct pointers for version management:
```
┌─────────────────────────────────────┐
│  32-bit Handle                      │
├──────────┬──────────┬───────────────┤
│  Thread  │  Arena   │  Slot         │
│  (12bit) │  (6bit)  │  (14bit)      │
└──────────┴──────────┴───────────────┘
```

This design enables:
- Efficient ABA problem prevention
- Compact control block representation (64-bit total)
- Fast handle-to-pointer resolution via table lookup

### Control Block Structure

Each gate maintains a 64-bit atomic control block:
```
┌─────────────────────────────────────┐
│  64-bit Control Block               │
├──────────────────┬──────────────────┤
│  Reference Count │  Version Handle  │
│  (32bit)         │  (32bit)         │
└──────────────────┴──────────────────┘
```

- **Reference Count**: Number of readers currently accessing this version
- **Version Handle**: Identifies the current version

### Memory Management

**Per-Thread Arenas**:
- Each thread owns up to 64 arenas
- Each arena contains 16,384 version slots
- Maximum capacity: 4,096 threads × 64 arenas × 16,384 slots ≈ 4.3B versions

**Free List Design**:
- Thread-local batch for fast allocation (wait-free)
- Arena-level shared list for cross-thread recycling
- Sentinel-based wait-free push operation

**Lifecycle**:
1. Writer allocates version from thread-local arena
2. Writer sets object and exchanges version atomically
3. Readers increment outer reference count on acquire
4. Writer decrements inner reference count on exchange (by outer count value)
5. Readers increment inner reference count on release
6. Version freed when inner reference count reaches zero

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

Writers use unconditional exchange. Object size is 16 bytes.
```bash
$ git clone https://github.com/minseok127/atomsnap.git
$ cd atomsnap && git checkout test && make
$ cd example/exchange && make
$ ./atomsnap_example 8 8 100
```

### Reader Throughput (ops/sec)

| Readers/Writers | std::shared_ptr | atomsnap   | Speedup |
|:---------------:|:---------------:|:----------:|:-------:|
| 1/1             | 4,118,869       | 15,364,317 | 3.73×   |
| 2/2             | 3,919,675       | 12,911,508 | 3.29×   |
| 4/4             | 3,157,681       | 17,237,426 | 5.46×   |
| 8/8             | 2,421,110       | 18,977,734 | 7.84×   |
| 16/16           | 2,914,017       | 19,803,148 | 6.79×   |

### Writer Throughput (ops/sec)

| Readers/Writers | std::shared_ptr | atomsnap  | Speedup |
|:---------------:|:---------------:|:---------:|:-------:|
| 1/1             | 2,290,178       | 6,536,867 | 2.85×   |
| 2/2             | 1,874,746       | 5,934,981 | 3.17×   |
| 4/4             | 1,419,709       | 7,147,858 | 5.03×   |
| 8/8             | 1,122,508       | 7,605,056 | 6.78×   |
| 16/16           | 1,356,290       | 7,342,350 | 5.41×   |

## Benchmark 2: Stateful CAS (16 bytes)

Writers use conditional exchange with retry. Compared against mutex and spinlock.
```bash
$ cd example/cmp_exchange && make
$ ./atomsnap_example 8 8 100
```

### Reader Throughput (ops/sec)

| Readers/Writers | shared_mutex | shared_ptr | spinlock   | atomsnap   |
|:---------------:|:------------:|:----------:|:----------:|:----------:|
| 1/1             | 483,073      | 4,168,734  | 13,861,844 | 13,345,141 |
| 2/2             | 10,806,981   | 3,790,341  | 7,474,572  | 14,037,157 |
| 4/4             | 13,945,730   | 3,082,837  | 4,546,094  | 17,340,585 |
| 8/8             | 17,149,214   | 2,567,489  | 4,055,162  | 19,344,667 |
| 16/16           | 17,000,505   | 2,372,081  | 1,608,919  | 21,623,425 |

### Writer Throughput (ops/sec)

| Readers/Writers | shared_mutex | shared_ptr | spinlock   | atomsnap  |
|:---------------:|:------------:|:----------:|:----------:|:---------:|
| 1/1             | 395,415      | 2,152,445  | 13,621,219 | 6,104,996 |
| 2/2             | 150,856      | 1,444,993  | 5,589,843  | 3,325,338 |
| 4/4             | 27,958       | 1,103,961  | 3,962,019  | 2,238,743 |
| 8/8             | 8,882        | 528,600    | 2,639,648  | 1,344,667 |
| 16/16           | 11           | 693,230    | 1,639,855  | 1,324,588 |

**Key Insight**: shared_mutex writer throughput collapses under contention (11 ops/sec at 16/16). ATOMSNAP maintains 1.3M ops/sec while providing superior reader performance.

## Benchmark 3: Large Object CAS (4 KB)

Object size is 4096 bytes. Tests scalability with larger memory footprint.
```bash
$ cd example/cmp_exchange_large && make
$ ./atomsnap_example 8 8 100
```

### Reader Throughput (ops/sec)

| Readers/Writers | shared_mutex | shared_ptr | spinlock  | atomsnap   |
|:---------------:|:------------:|:----------:|:---------:|:----------:|
| 1/1             | 70,052       | 2,001,176  | 3,805,337 | 1,915,966  |
| 2/2             | 3,629,300    | 2,352,779  | 3,725,530 | 3,647,479  |
| 4/4             | 14,935,709   | 2,803,578  | 3,105,244 | 8,010,336  |
| 8/8             | 12,134,346   | 1,940,209  | 1,960,867 | 13,101,473 |
| 16/16           | 15,975,086   | 1,089,127  | 895,905   | 13,242,798 |

### Writer Throughput (ops/sec)

| Readers/Writers | shared_mutex | shared_ptr | spinlock  | atomsnap  |
|:---------------:|:------------:|:----------:|:---------:|:---------:|
| 1/1             | 83,167       | 1,587,131  | 4,427,044 | 2,104,934 |
| 2/2             | 30,287       | 967,290    | 4,167,675 | 1,771,074 |
| 4/4             | 336          | 617,992    | 3,768,093 | 1,520,589 |
| 8/8             | 2,322        | 416,472    | 2,424,839 | 1,423,559 |
| 16/16           | 5            | 455,773    | 1,489,894 | 1,425,100 |

## Benchmark 4: Unbalanced Workloads (16 bytes)

Tests extreme reader/writer ratios.

### Configuration: 1 Reader, 16 Writers

| Metric          | shared_mutex | shared_ptr | spinlock | atomsnap  |
|:----------------|:------------:|:----------:|:--------:|:---------:|
| Reader ops/sec  | 8,620,295    | 234,983    | 347,163  | 2,796,784 |
| Writer ops/sec  | 630,098      | 1,618,858  | 6,251,655| 1,929,570 |

### Configuration: 16 Readers, 1 Writer

| Metric          | shared_mutex | shared_ptr | spinlock  | atomsnap   |
|:----------------|:------------:|:----------:|:---------:|:----------:|
| Reader ops/sec  | 18,221,686   | 5,208,506  | 5,079,128 | 43,766,774 |
| Writer ops/sec  | 1            | 129,827    | 328,145   | 549,203    |

**Key Insight**: ATOMSNAP excels in read-heavy workloads (16R/1W: 43.7M reader ops/sec, 8.4× faster than shared_mutex).

## Performance Characteristics

1. **Read-Heavy Workloads**: ATOMSNAP provides 2-8× higher reader throughput than std::shared_ptr
2. **Write Contention**: Maintains stable writer performance where shared_mutex degrades to near-zero
3. **Scalability**: Reader throughput increases with thread count (up to 44M ops/sec at 16R/1W)
4. **Large Objects**: Overhead remains reasonable even with 4KB objects
5. **Trade-off**: Writer throughput lower than spinlock under low contention, but avoids spinlock's reader bottleneck

---

# Implementation Details

## Memory Layout

### atomsnap_version (32 bytes)
```
Offset  Field             Size  Description
------  -----             ----  -----------
0       object            8     User payload pointer
8       free_context      8     User cleanup context
16      gate              8     Associated gate pointer
24      inner_ref_cnt     4     Internal reference counter
28      self_handle/next  4     Handle (allocated) or free list next (freed)
```

### Control Block (64-bit atomic)
```
Bits 63-32: Outer Reference Count (incremented by readers)
Bits 31-0:  Version Handle (identifies current version)
```

## Allocation Strategy

1. **Local Free List**: Thread-local batch of freed slots (wait-free pop)
2. **Arena Shared List**: Per-arena free list for cross-thread recycling
3. **New Arena Allocation**: When local and shared lists are exhausted

## Reference Counting Mechanism

ATOMSNAP uses a dual-counter design:

- **Outer Counter**: Tracks active readers (in control block, 32-bit)
- **Inner Counter**: Tracks lifetime (in version, 32-bit signed)

**Lifecycle**:
```
Initial state:        outer=0, inner=0
Reader acquires:      outer=1, inner=0
Writer exchanges:     outer=0, inner=-1  (inner -= outer at exchange)
Reader releases:      outer=0, inner=0   (inner += 1 at release)
→ Version freed when inner == 0
```

**Why it works**: Even if outer wraps around (after 4B operations), the arithmetic still produces the correct delta due to modulo 2³² arithmetic properties. Both counters must have the same bit width for this to work correctly.

## Handle Resolution
```c
// O(1) lookup via global table
arena = g_arena_table[thread_id][arena_id];
version = &arena->slots[slot_id];
```

No pointer chasing, no linked list traversal—direct array indexing.

## Thread Context Adoption

When a thread exits:
1. Thread ID is marked as available (via TLS destructor)
2. Context and arenas remain allocated
3. Next thread reuses the same ID and adopts existing arenas
4. **Benefit**: Avoids arena deallocation and reallocation overhead

This design assumes thread churn is relatively rare compared to thread lifespan.

---

# Limitations

1. **Thread Limit**: Maximum 4,096 threads (compile-time constant `MAX_THREADS`)
2. **Arena Limit**: 64 arenas per thread, 16,384 slots per arena
3. **No Dynamic Cleanup**: Arenas are never freed until process exit (memory grows monotonically)
4. **No Cross-Process Support**: Designed for multi-threaded, single-process use only

---
