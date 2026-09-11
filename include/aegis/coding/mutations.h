#ifndef AEGIS_CODING_MUTATIONS_H
#define AEGIS_CODING_MUTATIONS_H

#include "aegis/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file mutations.h
 * @brief Per-path file mutation serialization.
 */

typedef struct aegis_mutation_queue aegis_mutation_queue_t;

/**
 * @brief Allocate an empty per-path mutation queue backed by a single
 *        global mutex (per-path granularity is reserved for later).
 *        Caller owns the queue and must call destroy.
 */
aegis_status_t aegis_mutation_queue_create(aegis_mutation_queue_t** out);
/**
 * @brief Destroy a mutation queue. NULL is a no-op. The queue must be
 *        fully released (no outstanding acquire) beforehand.
 */
void           aegis_mutation_queue_destroy(aegis_mutation_queue_t* q);

/**
 * @brief Acquire exclusive mutation rights before touching @p path.
 *        Currently a single global mutex; blocks until the lock is held.
 *        Thread-safe. Pair every success with release().
 *
 * @param[in] q    Queue (must be non-NULL).
 * @param[in] path File path being mutated (currently informational only).
 * @return AEGIS_OK once exclusive rights are held, AEGIS_ERR_INVALID on
 *         NULL queue.
 */
aegis_status_t aegis_mutation_queue_acquire(aegis_mutation_queue_t* q, const char* path);
/**
 * @brief Release rights previously acquired with acquire(). Must pair a
 *        successful acquire() on the same queue; @p path mirrors the
 *        acquire argument and is informational. NULL queue is a no-op.
 *
 * @param[in] q    Queue, or NULL.
 * @param[in] path File path passed to the matching acquire() (informational).
 */
void           aegis_mutation_queue_release(aegis_mutation_queue_t* q, const char* path);

#ifdef __cplusplus
}
#endif

#endif /* AEGIS_CODING_MUTATIONS_H */
