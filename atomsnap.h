#ifndef ATOMSNAP_H
#define ATOMSNAP_H

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

/**
 * @file    atomsnap.h
 * @brief   Public interface for the atomsnap library.
 *
 * Atomsnap provides a wait-free/lock-free mechanism for managing shared
 * objects with multiple versions.
 */

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

/*
 * Opaque types to hide implementation details (handles, arenas, etc.).
 */
typedef struct atomsnap_gate atomsnap_gate;
typedef struct atomsnap_version atomsnap_version;

/**
 * @brief   User-defined callback to free the payload object.
 *
 * This function is invoked when a version is released and its reference
 * count drops to zero. The user is responsible for cleaning up the
 * resources associated with the object.
 *
 * @param   object:        The user object attached to the version.
 * @param   free_context:  The user-defined context provided during creation.
 */
typedef void (*atomsnap_free_func)(void *object, void *free_context);

/**
 * @brief   Initialization context for creating a new gate.
 *
 * @free_impl:        Required callback to free the user object.
 * @num_extra_slots:  Number of extra control block slots.
 *                    Set to 0 for a single slot.
 */
typedef struct atomsnap_init_context {
	atomsnap_free_func free_impl;
	int num_extra_control_blocks;
} atomsnap_init_context;

/**
 * @brief   Create a new atomsnap_gate instance.
 *
 * @param   ctx: Initialization context containing the free callback.
 *
 * @return  Pointer to a new atomsnap_gate on success, NULL on failure.
 */
struct atomsnap_gate *atomsnap_init_gate(struct atomsnap_init_context *ctx);

/**
 * @brief   Destroy the given atomsnap_gate.
 *
 * @param   gate: Gate returned by atomsnap_init_gate().
 */
void atomsnap_destroy_gate(struct atomsnap_gate *gate);

/**
 * @brief   Allocate memory for an atomsnap_version.
 *
 * Uses the internal memory allocator (arena) to get a version slot.
 *
 * @param   gate: Gate to associate with the version.
 *
 * @return  Pointer to the new version, or NULL on failure.
 */
struct atomsnap_version *atomsnap_make_version(struct atomsnap_gate *gate);

/**
 * @brief   Manually free a version that was created but NEVER exchanged.
 *
 * This function is used when a writer creates a version but decides not to
 * publish it (e.g., CAS failure). It invokes the user-defined free callback
 * to clean up the object, and then returns the version slot to the pool.
 *
 * @param   version: The version to free.
 */
void atomsnap_free_version(struct atomsnap_version *version);

/**
 * @brief   Set the user payload object and free context for a version.
 *
 * @param   ver:           The version created by atomsnap_make_version().
 * @param   object:        The actual data pointer to be managed.
 * @param   free_context:  Context passed to the free_impl callback.
 */
void atomsnap_set_object(struct atomsnap_version *ver, void *object,
	void *free_context);

/**
 * @brief   Get the user payload object from a version.
 *
 * @param   ver: The version pointer.
 *
 * @return  Pointer to the user object.
 */
void *atomsnap_get_object(const struct atomsnap_version *ver);

/**
 * @brief   Atomically acquire the current version from a slot.
 *
 * Increments the outer reference count.
 *
 * @param   gate:     Target gate.
 * @param   slot_idx: Control block slot index (0 for default).
 *
 * @return  Pointer to the acquired version.
 */
struct atomsnap_version *atomsnap_acquire_version_slot(
	struct atomsnap_gate *gate, int slot_idx);

/**
 * @brief   Release a version previously acquired.
 *
 * Increments inner ref count. If 0 (meaning all readers done and writer
 * detached), frees the version.
 *
 * @param   ver: Version to release.
 */
void atomsnap_release_version(struct atomsnap_version *ver);

/**
 * @brief   Replace the version in the given slot unconditionally.
 *
 * @param   gate:     Target gate.
 * @param   slot_idx: Control block slot index.
 * @param   new_ver:  New version to register.
 */
void atomsnap_exchange_version_slot(struct atomsnap_gate *gate, int slot_idx,
	struct atomsnap_version *new_ver);

/**
 * @brief   Conditionally replace the version if @expected matches.
 *
 * @param   gate:      Target gate.
 * @param   slot_idx:  Control block slot index.
 * @param   expected:  Expected current version.
 * @param   new_ver:   New version to register.
 *
 * @return  true on successful exchange, false otherwise.
 */
bool atomsnap_compare_exchange_version_slot(struct atomsnap_gate *gate,
	int slot_idx, struct atomsnap_version *expected,
	struct atomsnap_version *new_ver);

/*
 * Convenience wrappers for slot 0 (backward compatibility).
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
