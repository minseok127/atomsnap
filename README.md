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

# Usage

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

When implementing with atomsnap, just like creating a global_ptr using std::shared_ptr, an atomsnap_gate must be created. This data structure is allocated using the atomsnap_init_gate initialization function and deallocated using the atomsnap_destroy_gate function. To call this initialization function, an atomsnap_init_context data structure is required. The following is an example:

```
struct atomsnap_version *atomsnap_alloc_impl(void *arg) {
	auto version = new atomsnap_version;
	auto data = new Data;
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
