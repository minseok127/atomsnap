# ATOMSNAP

This library is designed to atomically manage multiple versions of an object in a multi-threaded environment. It ensures wait-free access to versions and guarantees their safe memory release.

Multiple readers obtain a pointer instantly without failure. Multiple writers can decide whether to update the pointer using TAS without failure, or to use CAS with a retry mechanism, depending on the requirements of the application.

Acquiring and releasing a version should always be done as a pair. Avoid acquiring repeatedly without releasing. If the gap between acquisitions and releases for the same version exceeds the range of uint16_t (0xffff), the behavior becomes unpredictable. However, as long as this gap does not widen, there are no restrictions on accessing the same version. 

Note that this library is implemented under the assumption that user virtual memory address is limited to 48 bits. Using virtual memory beyond this range requires additional implementation.

# Build
```
$ git clone https://github.com/minseok127/atomsnap.git
$ cd atomsnap
$ make
=> libatomsnap.a, libatomsnap.so, atomsnap.h
```

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

## Implementation with atomsnap

When implementing with atomsnap, just like creating a global_ptr using std::shared_ptr, an atomsnap_gate must be created. This data structure is allocated using the atomsnap_init_gate initialization function and deallocated using the atomsnap_destroy_gate function. To call the initialization function, an atomsnap_init_context data structure is required.

The user must provide two function pointers in the context. 

```
/* atomsnap.h */

typedef struct atomsnap_version {
	void *object;
	void *free_context;
	struct atomsnap_gate *gate;
	void *opaque;
} atomsnap_version;

typedef struct atomsnap_init_context {
	struct atomsnap_version *(*atomsnap_alloc_impl)(void* alloc_arg);
	void (*atomsnap_free_impl)(struct atomsnap_version *version);
} atomsnap_init_context;
```

The first function is responsible for allocating an atomsnap_version structure. This function is later called inside atomsnap_make_version, which is used by writers to allocate an atomsnap_version structure. The allocation function receives its argument from the parameters passed to atomsnap_make_version. The second function pointer is for deallocating an atomsnap_version. This function is automatically called when all threads referencing the atomsnap_version have disappeared. The user can specify arguments for the free function by setting them in the free_context field of atomsnap_version.

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

Once the preparation steps are complete, the atomsnap_init_context can be passed as an argument to atomsnap_init_gate, which will return a pointer to an atomsnap_gate.

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

Just like obtaining global_ptr using atomic_load in std::shared_ptr, atomsnap allows acquiring the current version using atomsnap_acquire_version. This function must be used in pairs with atomsnap_release_version. The lifetime of the acquired version is guaranteed until atomsnap_release_version is called. Accessing the version after calling release is not safe and is not guaranteed to work correctly.

Writers call the atomsnap_make_version function to allocate a new version. In this process, the allocation function pointer set during initialization is invoked, and the second argument of atomsnap_make_version is passed as an argument to the allocation function.

## Two options for the writer

If the writer creates a new version regardless of the previous state, it can replace the version using the atomsnap_exchange_version function, as shown in the pseudocode above. On the other hand, if the writer is sensitive to the previous version's state, it may need to use atomsnap_compare_exchange_version to replace the version, as shown in the following pseudocode.

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

This function replaces the gate’s version with new_version only if old_version is the latest version of the gate. It returns true if the replacement succeeds and false otherwise. Note that calling atomsnap_release_version on old_version before atomsnap_compare_exchange_version can lead to an ABA problem.

For example, suppose Thread A obtains old_version and creates new_version based on it. Before calling atomsnap_compare_exchange_version, it calls atomsnap_release_version, freeing old_version. Meanwhile, Thread B creates a new_version, its memory address matches that of the now-freed old_version. If Thread B successfully replaces the version in gate, and Thread A then calls atomsnap_compare_exchange_version, Thread A would replace the version based on an invalid version.

To prevent this, the order of atomsnap_compare_exchange_version and atomsnap_release_version must be carefully managed.
