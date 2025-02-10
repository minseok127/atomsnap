#ifndef ATOMIC_SNAPSHOT_H
#define ATOMIC_SNAPSHOT_H

#include <stddef.h>
#include <stdbool.h>

#define ATOMIC_SNAPSHOT_FREE_SAFE	(0)
#define ATOMIC_SNAPSHOT_FREE_UNSAFE	(1)

typedef struct atomic_snapshot_gate atomic_snapshot_gate;
typedef struct atomic_snapshot atomic_snapshot;

struct atomic_snapshot_gate *atomic_snapshot_init_gate(void);
void atomic_snapshot_destroy_gate(struct atomic_snapshot_gate *g);

void *atomic_snapshot_get_object(struct atomic_snapshot *s);
void atomic_snapshot_set_object(struct atomic_snapshot *s, void *object);

struct atomic_snapshot *atomic_snapshot_acquire(struct atomic_snapshot_gate *g);
int atomic_snapshot_release(struct atomic_snapshot *s);

struct atomic_snapshot *atomic_snapshot_test_and_set(
	struct atomic_snapshot_gate *g, struct atomic_snapshot *s,
	int *old_snapshot_status);
bool atomic_snapshot_compare_and_exchange(struct atmoic_snapshot_gate *g,
	struct atomic_snapshot *olds, struct atomic_snapshot *news,
	int *old_snapshot_status);

#endif /* ATOMIC_SHAPSHOT_H */
