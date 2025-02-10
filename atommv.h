#ifndef ATOMMV_H
#define ATOMMV_H

#include <stddef.h>
#include <stdbool.h>

typedef enum {
	ATOMMV_SAFE_FREE,
	ATOMMV_UNSAFE_FREE,
} ATOMMV_STATUS;

typedef struct atommv_gate atommv_gate;
typedef struct atommv_version atommv_version;

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

#endif /* ATOMMV_H */
