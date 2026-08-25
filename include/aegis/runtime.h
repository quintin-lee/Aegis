/**
 * @file runtime.h
 * @brief Runtime lifecycle: create, start, stop, destroy.
 *
 * The runtime manages a pool of worker threads and provides the core
 * execution context for an Aegis agent.
 *
 * Lifecycle:
 *   aegis_runtime_create → aegis_runtime_start (idempotent)
 *                       → aegis_runtime_stop  (idempotent)
 *                       → aegis_runtime_destroy
 */
#ifndef AEGIS_RUNTIME_H
#define AEGIS_RUNTIME_H

#include "aegis/types.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Opaque runtime handle. */
typedef struct aegis_runtime aegis_runtime_t;

/**
 * Create a new runtime handle.
 *
 * @param out [out] Pointer to receive the allocated handle.
 * @return AEGIS_OK on success, or a negative error code.
 */
aegis_status_t aegis_runtime_create(aegis_runtime_t** out);

/**
 * Start the runtime's worker threads.
 *
 * Idempotent: calling when already STARTED or STARTING returns AEGIS_OK.
 *
 * @param rt Runtime handle.
 * @return AEGIS_OK on success, or a negative error code.
 */
aegis_status_t aegis_runtime_start(aegis_runtime_t* rt);

/**
 * Stop the runtime and join all worker threads.
 *
 * Idempotent: calling when already STOPPED or STOPPING returns AEGIS_OK.
 *
 * @param rt Runtime handle.
 * @return AEGIS_OK on success, or a negative error code.
 */
aegis_status_t aegis_runtime_stop(aegis_runtime_t* rt);

/**
 * Destroy a runtime handle. Stops the runtime first if necessary.
 *
 * Safe to call with NULL.
 *
 * @param rt Runtime handle (may be NULL).
 */
void aegis_runtime_destroy(aegis_runtime_t* rt);

#ifdef __cplusplus
}
#endif

#endif /* AEGIS_RUNTIME_H */
