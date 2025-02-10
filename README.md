## ATOMMV

This library is designed to atomically manage multiple versions of a specific object in a multi-threaded environment. It provides wait-free access to atomic versions and ensures the safe freeing of versions. Readers obtain a pointer immediately without failure. Writers can decide whether to update the pointer instantly without failure using TAS or to use CAS with a retry mechanism, depending on the requirements of the application.

Acquiring and releasing a version should always be done as a pair. Avoid acquiring repeatedly without releasing. If the gap between acquisitions and releases for the same version exceeds the range of uint16_t (0xffff), the behavior becomes unpredictable.

## Build
```
$ git clone https://github.com/minseok127/atommv.git
$ cd atommv
$ make
=> libatommv.a, libatommv.so, atommv.h
```

## API
```
typedef enum {
	ATOMMV_SAFE_FREE,
	ATOMMV_UNSAFE_FREE,
} ATOMMV_STATUS;

struct atommv_gate *atommv_init_gate(void);

void atommv_destroy_gate(struct atommv_gate *g);

void *atommv_get_object(struct atommv_version *v);

void atommv_set_object(struct atommv_version *v, void *object);

struct atommv_version *atommv_acquire(struct atommv_gate *g);

ATOMMV_STATUS atommv_release(struct atommv_version *v);

struct atommv_version *atommv_test_and_set(
	struct atommv_gate *g, struct atommv_version *v,
	ATOMMV_STATUS *old_version_status);

bool atommv_compare_and_exchange(struct atommv_gate *g,
	struct atommv_version *oldv, struct atommv_version *newv,
	ATOMMV_STATUS *old_version_status);
```

## Usage

### Common (reader, writer)
```
{
  /* Open the gate for the object to be managed with multiple versions */
  struct atommv_gate *gate = atommv_init_gate();

  ...

  /* Done */
  atommv_destroy_gate(gate);
}
```

### Reader
```
{
  atommv_version *current_version = atommv_acquire(gate);
  void *object = atommv_get_object(current_version);

  ...

  ATOMMV_STATUS s = atommv_release(current_version):
  if (s == ATOMMV_SAFE_FREE) {
    free(current_version);
  }
}
```

### Writer (TAS)
```
{
  atommv_version *old_version, *new_version;
  ATOMMV_STATUS s;

  new_version = (struct atommv_version *)malloc(sizeof(atommv_version));
  atommv_set_object(new_version, new_object);
  
  /*
   * If unconditional version replacement is allowed
   */
  old_version = atommv_test_and_set(gate, new_version, &s);
  if (s == ATOMMV_SAFE_FREE) {
    free(old_version);
  }
}
```

### Writer (CAS)
```
{
  atommv_version *latest_version, *new_version;
  ATOMMV_STATUS s;

  /* 
   * If the new version must be created exactly from the latest version
   */
  for (;;) {
    latest_version = atommv_acquire(gate);
    new_version = make_new_version(latest_version);
    s = atommv_release(latest_version);

    /*
     * If latest_version can be freed, it means that another new version has been 
     * registered. This implies that our new_version cannot be registered because
     * our new_version would no longer be created from the current latest version
     */
    if (s == ATOMMV_SAFE_FREE) {
        free(latest_version);
        continue;
    }

    if (atommv_compare_and_exchange(gate, latest_version, new_version, &s)) {
      if (s == ATOMMV_SAFE_FREE) {
        free(latest_version);
      }
      break;
    }

    /* Another version has been registered, try again */
  }
}
```
