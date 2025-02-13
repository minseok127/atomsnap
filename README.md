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

