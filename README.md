# ATOMSNAP

### Purpose:

- Atomically manages multiple versions of an object in a multi-threaded environment.
- Ensures wait-free access and safe memory release.

### Reader & Writer Behavior:

- Readers: 
	- Instantly obtain a pointer without failure
- Writers:
	- Use TAS (Test-And-Set) for guaranteed updates.
	- Use CAS (Compare-And-Swap) with a retry mechanism if needed.

### Version Management Rules:

- Acquiring and releasing a version must always be paired.
- Avoid repeated acquisition without release.
- If the acquisition-release gap exceeds 0xFFFF (uint16_t), behavior is unpredictable.

### Memory Assumption:

- Assumes user virtual memory is limited to 48 bits.
- Using memory beyond this range requires additional implementation.

# Build
```
$ git clone https://github.com/minseok127/atomsnap.git
$ cd atomsnap
$ make
=> libatomsnap.a, libatomsnap.so, atomsnap.h
```

# Data Structure

- struct atomsnap_gate
	- Synchronization point that manages versioned object in a multi-threaded environment.
	- Readers and writers accessing the same gate handle different versions of the same object.

- struct atomsnap_init_context
	- Passed as an argument to atomsnap_init_gate().
	- Contains user-defined function pointers for managing memory allocation and deallocation of atomsnap_version objects.
	- Fields:
		- atomsnap_alloc_impl → Custom memory allocation function pointer.
		- atomsnap_free_impl → Custom memory deallocation function pointer.

- struct atomsnap_version (Versioned Object)
	- Represents a specific version of an object.
	- Fields:
		- object → Pointer to the actual data for this version.
		- free_context → User-defined context for the free function.
		- gate → Identifies the atomsnap_gate this version belongs to.
		- opaque → Internal version management data, not user-modifiable.
	- Usage:
		- Writers create a version using atomsnap_make_version().
		- After creation, writers assign their object and set free_context for cleanup.
		- gate and opaque are initialized internally and should not be modified.

# API

- Gate Management
	- atomsnap_init_gate(ctx)
		- Initializes and returns a pointer to an atomsnap_gate, or NULL on failure.
	- atomsnap_destroy_gate(gate)
		- Destroys an atomsnap_gate.

- Version Management
	- atomsnap_acquire_version(gate)
		- Atomically acquires the current version.
		- Ensures the version is not deallocated until released.
	- atomsnap_release_version(version)
		- Pairs with atomsnap_acquire_version()
		- Releases a version and invoking the user-defined free function when no threads reference it.

- Writer Operations
	- atomsnap_make_version(gate, alloc_arg)
		- Allocates memory for an atomsnap_version. 
		- Calls a user-defined allocation function with alloc_arg.
	- atomsnap_exchange_version(gate, version)
		- Unconditionally replaces the gate’s version with the given version.
	- atomsnap_compare_exchange_version(gate, old_version, new_version) 
		- Replaces the gate’s version only if the latest version matches old_version.
		- Returns true on success.

# Example

The target situation involves multiple writers attempting to modify a logically identical object. The object is too large to be modified with a single atomic instruction, so writers copy it into their own memory, make all necessary modifications, and then attempt to replace the object with the updated version. Readers, on the other hand, should only see objects that are either entirely unmodified or fully updated, ensuring atomic visibility without partial modifications.

## Implementation with std::shared_ptr
```
struct Data {
	int64_t value1;
	int64_t value2;
};

std::shared_ptr<Data> global_ptr = std::make_shared<Data>();
```
This situation can be implemented using C++'s std::shared_ptr. For example, consider a Data structure that contains two 8-byte integers. Writers need to update both value1 and value2, while readers must always see a fully updated or completely unchanged version of Data. To achieve this, we can declare a global std::shared_ptr<Data> and use it to manage versions atomically.

```
void writer(std::barrier<> &sync) {
	while (true) {
		Data *old_data = std::atomic_load(&global_ptr);
		Data *new_data = std::make_shared<Data>(*old_data);
		new_data->value1 = old_data->value1 + 1;
		new_data->value2 = old_data->value2 + 1;
		std::atomic_store(&global_ptr, new_data);
	}
}

void reader(std::barrier<> &sync) {
	while (true) {
		Data *current_data = std::atomic_load(&global_ptr);
		if (current_data->value1 != current_data->value2) {
			fprintf(stderr, "Invalid data, value1: %ld, value2: %ld\n",
				current_data->value1, current_data->value2);
			exit(1);
		}
	}
}

```
Writers create a new Data instance, modify all necessary fields, and then replace the old version with the new one in an atomic operation. Readers always access a fully consistent snapshot of Data, ensuring they never observe partially modified values. Since both readers and writers use std::shared_ptr, safe memory deallocation of Data is guaranteed.

## Implementation with atomsnap (instead of std::shared_ptr)

When implementing with atomsnap, just like creating a global_ptr using std::shared_ptr, an atomsnap_gate must be created. This data structure is allocated using the atomsnap_init_gate() initialization function and deallocated using the atomsnap_destroy_gate() function. To call the initialization function, an atomsnap_init_context data structure is required.

The user must provide two function pointers in the context. 

```
/* atomsnap.h */
typedef struct atomsnap_init_context {
	struct atomsnap_version *(*atomsnap_alloc_impl)(void* alloc_arg);
	void (*atomsnap_free_impl)(struct atomsnap_version *version);
} atomsnap_init_context;
```
```
/* atomsnap.h */
typedef struct atomsnap_version {
	void *object;
	void *free_context;
	struct atomsnap_gate *gate;
	void *opaque;
} atomsnap_version;
```
```
struct atomsnap_version *atomsnap_make_version(struct atomsnap_gate *gate,
	void *alloc_arg);
```

The first function is responsible for allocating an atomsnap_version structure. This function is later called inside atomsnap_make_version(), which is used by writers to allocate an atomsnap_version structure. The allocation function receives its argument from the parameters passed to atomsnap_make_version(). 

The second function pointer is for deallocating an atomsnap_version. This function is automatically called when all threads referencing the atomsnap_version have disappeared. The user can specify arguments for the free function by setting them in the free_context field of atomsnap_version.

Here is an example:
```
struct atomsnap_version *atomsnap_alloc_impl(void *arg) {
	struct atomsnap_version *version = new atomsnap_version;
	Data *data = new Data;
	int *values = (int *)arg;

	data->value1 = values[0];
	data->value2 = values[1];

	version->object = data;
	version->free_context = NULL;

	return version;
}

void atomsnap_free_impl(struct atomsnap_version *version) {
	delete (Data *)version->object;
	delete version;
}

struct atomsnap_init_context atomsnap_gate_ctx = {
	.atomsnap_alloc_impl = atomsnap_alloc_impl,
	.atomsnap_free_impl = atomsnap_free_impl
};

atomsnap_gate *gate = atomsnap_init_gate(&atomsnap_gate_ctx);
```

Once the preparation steps are complete, the atomsnap_init_context can be passed as an argument to atomsnap_init_gate(), which will return a pointer to an atomsnap_gate.

```
void writer(std::barrier<> &sync) {
	struct atomsnap_version *new_version;
	int values[2];

	while (true) {
		struct atomsnap_version *old_version = atomsnap_acquire_version(gate);
		Data *old_data = static_cast<Data*>(old_version->object);
		values[0] = old_data->value1 + 1;
		values[1] = old_data->value2 + 1;
		new_version = atomsnap_make_version(gate, (void*)values);
		atomsnap_exchange_version(gate, new_version);
		atomsnap_release_version(old_version);
	}
}

void reader(std::barrier<> &sync) {
	struct atomsnap_version *current_version;

	while (true) {
		current_version = atomsnap_acquire_version(gate);
		Data *d = static_cast<Data*>(current_version->object);
		if (d->value1 != d->value2) {
			fprintf(stderr, "Invalid data, value1: %ld, value2: %ld\n",
					d->value1, d->value2);
			exit(1);
		}
		atomsnap_release_version(current_version);
	}
}
```

Just like obtaining global_ptr using atomic_load in std::shared_ptr, atomsnap allows acquiring the current version using atomsnap_acquire_version(). This function must be used in pairs with atomsnap_release_version(). The lifetime of the acquired version is guaranteed until atomsnap_release_version() is called. Accessing the version after calling release is not safe and is not guaranteed to work correctly.

Writers call the atomsnap_make_version() function to allocate a new version. In this process, the allocation function pointer set during initialization is invoked, and the second argument of atomsnap_make_version is passed as an argument to the allocation function.

## Caution for the writer when using CAS

If the writer creates a new version regardless of the previous state, it can replace the version using the atomsnap_exchange_version() function, as shown in the pseudocode above. On the other hand, if the writer is sensitive to the previous version's state, it may need to use atomsnap_compare_exchange_version() to replace the version, as shown in the following pseudocode.

```
void writer(std::barrier<> &sync) {
	struct atomsnap_version *new_version;
	int values[2];

	while (true) {
		struct atomsnap_version *old_version = atomsnap_acquire_version(gate);
		auto old_data = static_cast<Data*>(old_version->object);
		values[0] = old_data->value1 + 1;
		values[1] = old_data->value2 + 1;
		new_version = atomsnap_make_version(gate, (void*)values);
		if (atomsnap_compare_exchange_version(gate,
				old_version, new_version)) {
			// do something
		}
		atomsnap_release_version(old_version); /* !!! Call this function after atomsnap_compare_exchange_version !!! */
	}
}
```

This function replaces the gate’s version with new_version only if old_version is the latest version of the gate. It returns true if the replacement succeeds and false otherwise. Note that calling atomsnap_release_version() on old_version before atomsnap_compare_exchange_version() can lead to an ABA problem.

For example, suppose Thread A obtains old_version and creates new_version based on it. Before calling atomsnap_compare_exchange_version(), it calls atomsnap_release_version(), freeing old_version. Meanwhile, Thread B creates a new_version, its memory address accidentally matches that of the now-freed old_version. If Thread B successfully replaces the version in the gate, and Thread A then calls atomsnap_compare_exchange_version(), Thread A would replace the version based on an invalid version (what we expect is for atomsnap_compare_exchange_version() to fail since the replacement should be based on Thread B’s version).

To prevent this, atomsnap_release_version() for the old_version used in atomsnap_compare_exchange_version() must be called only after atomsnap_compare_exchange_version() has completed.

# Evaluation (std::shared_ptr vs atomsnap)
