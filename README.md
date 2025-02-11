# ATOMSNAP

This library is designed to atomically manage multiple versions of an object in a multi-threaded environment. It ensures wait-free access to versions and guarantees their safe memory release.

Multiple readers obtain a pointer instantly without failure. Multiple writers can decide whether to update the pointer instantly without failure using TAS or to use CAS with a retry mechanism, depending on the requirements of the application.

Acquiring and releasing a version should always be done as a pair. Avoid acquiring repeatedly without releasing. If the gap between acquisitions and releases for the same version exceeds the range of uint16_t (0xffff), the behavior becomes unpredictable. However, as long as this gap does not widen, there are no restrictions on accessing the same version. 

Note that this library is implemented under the assumption that user virtual memory address is limited to 48 bits. Using virtual memory beyond this range requires additional implementation.

# Build
```
$ git clone https://github.com/minseok127/atomsnap.git
$ cd atomsnap
$ make
=> libatomsnap.a, libatomsnap.so, atomsnap.h
```

# API
```
typedef enum {
	ATOMSNAP_SAFE_FREE,
	ATOMSNAP_UNSAFE_FREE,
} ATOMSNAP_STATUS;

struct atomsnap_gate *atomsnap_init_gate(void);

void atomsnap_destroy_gate(struct atomsnap_gate *g);

void *atomsnap_get_object(struct atomsnap_version *v);

void atomsnap_set_object(struct atomsnap_version *v, void *object);

struct atomsnap_version *atomsnap_acquire_version(struct atomsnap_gate *g);

ATOMSNAP_STATUS atomsnap_release_version(struct atomsnap_version *v);

struct atomsnap_version *atomsnap_test_and_set(
	struct atomsnap_gate *g, struct atomsnap_version *v,
	ATOMSNAP_STATUS *old_version_status);

bool atomsnap_compare_and_exchange(struct atomsnap_gate *g,
	struct atomsnap_version *oldv, struct atomsnap_version *newv,
	ATOMSNAP_STATUS *old_version_status);
```

# Usage

### Common
```
{
  /* Open the gate for the object to be managed with multiple versions */
  struct atomsnap_gate *gate = atomsnap_init_gate();

  /*
   * If readers and writers operate on the same gate,
   * they will use multiple versions of the same object.
   */

  /* Done */
  atomsnap_destroy_gate(gate);
}
```

### Reader
```
{
  atomsnap_version *current_version = atomsnap_acquire_version(gate);
  void *object = atomsnap_get_object(current_version);

  ...

  ATOMSNAP_STATUS s = atomsnap_release_version(current_version):
  if (s == ATOMSNAP_SAFE_FREE) {
    free(current_version);
  }
}
```

### Writer (TAS)
```
{
  atomsnap_version *old_version, *new_version;
  ATOMSNAP_STATUS s;

  new_version = (struct atomsnap_version *)malloc(sizeof(atomsnap_version));
  atomsnap_set_object(new_version, new_object);
  
  /*
   * If unconditional version replacement is allowed
   */
  old_version = atomsnap_test_and_set(gate, new_version, &s);
  if (s == ATOMSNAP_SAFE_FREE) {
    free(old_version);
  }
}
```

### Writer (CAS)
```
{
  atomsnap_version *latest_version, *new_version;
  ATOMSNAP_STATUS s;

  /* 
   * If the new version must be created exactly from the latest version
   */
  for (;;) {
    latest_version = atomsnap_acquire_version(gate);
    new_version = make_new_version(latest_version); /* user function */
    s = atomsnap_release_version(latest_version);

    /*
     * If latest_version can be freed, it means that another new version has been 
     * registered. So our new_version cannot be registered because it is no longer
     * based on the lasted version (another thread's new version)
     */
    if (s == ATOMSNAP_SAFE_FREE) {
        free(latest_version);
        continue;
    }

    if (atomsnap_compare_and_exchange(gate, latest_version, new_version, &s)) {
      if (s == ATOMSNAP_SAFE_FREE) {
        free(latest_version);
      }
      break;
    }

    /* Another version has been registered, try again */
  }
}
```

# Test

# Evaluation
