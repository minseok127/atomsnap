#ifndef ATOMSNAP_H
#define ATOMSNAP_H
#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

/*
 * A data structure inside atomsnap.c for managing versions of a specific
 * object. Readers and writers accessing the same gate handle multiple versions
 * of the same object.
 */
typedef struct atomsnap_gate atomsnap_gate;

/*
 * atomsnap_version - target object's version
 * @object: pointer to the actual data of this version
 * @free_context: context to be used inside the user-defined free function
 * @gate: which object this version belongs to
 * @opaque: data structure inside atomsnap.c used for version management
 *
 * Writers obtain a pointer to atomsnap_version through atomsnap_make_version().
 * Then the writers insert their own object into the version and set the
 * @free_context to be used in the user-defined version free fucntion.
 *
 * Initialization of gate and opaque is performed inside
 * atomsnap_make_version(), so users dont's need to modify them explicitly.
 */
typedef struct atomsnap_version {
	void *object;
	void *free_context;
	struct atomsnap_gate *gate;
	void *opaque;
} atomsnap_version;

/*
 * atomsnap_init_context - atomsnap_init_gate's argument
 * @atomsnap_alloc_impl: user-defined memory allocation function
 * @atomsnap_free_impl: user-defined memory free function
 * @num_extra_control_blocks: Number of extra control blocks
 *
 * To manage a specific object with multiple versions, an atomsnap_gate must be
 * created using atomsnap_init_gate().
 *
 * For this, the user must define a custom memory allocation function and a
 * custom memory deallocation function. These functions should be set in the
 * functino pointer of atmosnap_init_context and then passed to
 * atomsnap_init_gate().
 *
 * By default, a gate holds a single control block. If num_extra_control_blocks
 * is 0, the gate operates with just that one control block. Otherwise, multiple
 * control blocks are created, each referred to as a slot. Slot index 0
 * represents the default control block, while indexes starting from 1
 * correspond to the slots in the extra control block array. For gates using
 * multiple slots, functions labeled with the slot keyword should be used.
 *
 * Note that the user-defined atomsnap_alloc_impl() and atomsnap_free_impl() are
 * not limited to just allocating and freeing atomsnap_version. They can also
 * manage other elements as needed by the user. But the gate and opaque fields
 * of atomsnap_version must not be modified by users.
 */
typedef struct atomsnap_init_context {
	struct atomsnap_version *(*atomsnap_alloc_impl)(void* alloc_arg);
	void (*atomsnap_free_impl)(struct atomsnap_version *version);
	int num_extra_control_blocks;
} atomsnap_init_context;

/*
 * Returns pointer to an atomsnap_gate, or NULL on failure.
 */
struct atomsnap_gate *atomsnap_init_gate(struct atomsnap_init_context *ctx);

/*
 * Destory the atomsnap_gate.
 */
void atomsnap_destroy_gate(struct atomsnap_gate *gate);

/*
 * Allocate memroy for an atomsnap_version. This function internally calls
 * the user-defined version allocation function with @alloc_arg as an argument.
 *
 * Note that the version's gate and opaque are initialized, but object and
 * free_context are not explicitly initialized within this function, as they may
 * have been set by the user-defined function.
 */
struct atomsnap_version *atomsnap_make_version(struct atomsnap_gate *gate,
	void *alloc_arg);

/*
 * Atomically acquire the current version. The returned version is guaranteed
 * not to be deallocated until atomsnap_release_version() is called.
 */
struct atomsnap_version *atomsnap_acquire_version_slot(
	struct atomsnap_gate *gate, int slot_idx);

/*
 * This function pairs with atomsanp_acquire_version(). It must be called when
 * the usage of a version is complete. Within this version, if it is ensured
 * that no threads are referencing the given version, the user-defined version
 * free function is invoked.
 *
 * Note that the deallocation of memory for both the version and its object
 * pointer is entirely handled by the user-defined function.
 */
void atomsnap_release_version(struct atomsnap_version *version);

/*
 * Writer's function. This function replaces the version of the gate into the
 * given version, unconditionally.
 */
void atomsnap_exchange_version_slot(struct atomsnap_gate *gate, int slot_idx,
	struct atomsnap_version *new_ver);

/*
 * Writer's function. This function replaces the version of the gate into the
 * given version, only when the latest version is the given old_version.
 */
bool atomsnap_compare_exchange_version_slot(struct atomsnap_gate *gate,
	int slot_idx, struct atomsnap_version *old_ver,
	struct atomsnap_version *new_ver);

/*
 * Convenience wrappers that preserve perfect backward compatibility:
 * they operate on slot 0, i.e., the original single control block.
 */
#define atomsnap_acquire_version(g) \
	atomsnap_acquire_version_slot((g), 0)

#define atomsnap_exchange_version(g, v) \
	atomsnap_exchange_version_slot((g), 0, (v))

#define atomsnap_compare_exchange_version(g, o, n) \
	atomsnap_compare_exchange_version_slot((g), 0, (o), (n))

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* ATOMSNAP_H */
