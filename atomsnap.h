#ifndef ATOMSNAP_H
#define ATOMSNAP_H

#include <stddef.h>
#include <stdbool.h>

typedef enum {
	ATOMSNAP_SAFE_FREE,
	ATOMSNAP_UNSAFE_FREE,
} ATOMSNAP_STATUS;

typedef struct atomsnap_gate atomsnap_gate;
typedef struct atomsnap_version atomsnap_version;

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

#endif /* ATOMSNAP_H */
