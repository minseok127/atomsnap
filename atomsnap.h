#ifndef ATOMIC_SNAPSHOT_H
#define ATOMIC_SNAPSHOT_H

#include <stddef.h>
#include <stdbool.h>

#define ATOMIC_SNAPSHOT_FREE_SAFE	(0)
#define ATOMIC_SNAPSHOT_FREE_UNSAFE	(1)

typedef struct atomsnap_gate atomsnap_gate;
typedef struct atomsnap atomsnap;

struct atomsnap_gate *atomsnap_init_gate(void);
void atomsnap_destroy_gate(struct atomsnap_gate *g);

void *atomsnap_get_object(struct atomsnap *s);
void atomsnap_set_object(struct atomsnap *s, void *object);

struct atomsnap *atomsnap_acquire(struct atomsnap_gate *g);
int atomsnap_release(struct atomsnap *s);

struct atomsnap *atomsnap_test_and_set(
	struct atomsnap_gate *g, struct atomsnap *s,
	int *old_snapshot_status);
bool atomsnap_compare_and_exchange(struct atomsnap_gate *g,
	struct atomsnap *olds, struct atomsnap *news,
	int *old_snapshot_status);

#endif /* ATOMIC_SHAPSHOT_H */
