#ifndef ATOMIC_SNAPSHOT_H
#define ATOMIC_SNAPSHOT_H

#include <stddef.h>
#include <stdbool.h>

typedef enum {
	ATOMSNAP_SAFE_FREE,
	ATOMSNAP_UNSAFE_FREE,
} ATOMSNAP_STATUS

typedef struct atomsnap_gate atomsnap_gate;
typedef struct atomsnap atomsnap;

struct atomsnap_gate *atomsnap_init_gate(void);
void atomsnap_destroy_gate(struct atomsnap_gate *g);

void *atomsnap_get_object(struct atomsnap *s);
void atomsnap_set_object(struct atomsnap *s, void *object);

struct atomsnap *atomsnap_acquire(struct atomsnap_gate *g);
ATOMSNAP_STATUS atomsnap_release(struct atomsnap *s);

struct atomsnap *atomsnap_test_and_set(
	struct atomsnap_gate *g, struct atomsnap *s,
	ATOMSNAP_STATUS *old_snapshot_status);
bool atomsnap_compare_and_exchange(struct atomsnap_gate *g,
	struct atomsnap *olds, struct atomsnap *news,
	ATOMSNAP_STATUS *old_snapshot_status);

#endif /* ATOMIC_SHAPSHOT_H */
