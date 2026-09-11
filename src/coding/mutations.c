/**
 * @file mutations.c
 * @brief Per-path file-mutation queue serializing write→edit→edit
 * sequences so concurrent tool calls cannot interleave on one path.
 * Mutex-guarded; entries are lightweight path locks, not content.
 */
#define _POSIX_C_SOURCE 200809L
#include "aegis/coding/mutations.h"
#include <stdlib.h>
#include <pthread.h>

struct aegis_mutation_queue {
    pthread_mutex_t lock;
};

/**
 * @brief Create an empty mutation queue.
 *
 * @param[out] out Receives the new queue; untouched on failure.
 * @return AEGIS_OK on success, AEGIS_ERR_INVALID on NULL @p out,
 *   AEGIS_ERR_NOMEM on allocation failure.
 */
aegis_status_t aegis_mutation_queue_create(aegis_mutation_queue_t** out)
{
    if (!out) {
        return AEGIS_ERR_INVALID;
    }
    aegis_mutation_queue_t* q = (aegis_mutation_queue_t*)calloc(1, sizeof(*q));
    if (!q) {
        return AEGIS_ERR_NOMEM;
    }
    pthread_mutex_init(&q->lock, NULL);
    *out = q;
    return AEGIS_OK;
}

/**
 * @brief Destroy a mutation queue.
 *
 * The queue must be fully released (no outstanding acquire) beforehand.
 * NULL is a no-op.
 *
 * @param[in] q Queue to destroy, or NULL.
 */
void aegis_mutation_queue_destroy(aegis_mutation_queue_t* q)
{
    if (!q) {
        return;
    }
    pthread_mutex_destroy(&q->lock);
    free(q);
}

/**
 * @brief Acquire exclusive mutation rights before touching @p path.
 *
 * Currently a single global mutex: @p path is accepted but not yet used for
 * per-path granularity (reserved for future fine-grained locking), so
 * acquires on different paths still serialize against each other. Blocks
 * until the lock is held; pair every success with release().
 * Thread-safe.
 *
 * @param[in] q    Queue, must be non-NULL.
 * @param[in] path File path being mutated (currently informational only).
 * @return AEGIS_OK once exclusive rights are held, AEGIS_ERR_INVALID on
 *   NULL queue.
 */
aegis_status_t aegis_mutation_queue_acquire(aegis_mutation_queue_t* q, const char* path)
{
    (void)path;
    if (!q) {
        return AEGIS_ERR_INVALID;
    }
    pthread_mutex_lock(&q->lock);
    return AEGIS_OK;
}

/**
 * @brief Release rights previously acquired with acquire().
 *
 * Must pair a successful acquire() on the same queue; @p path mirrors the
 * acquire argument and is likewise informational. NULL queue is a no-op.
 * Thread-safe.
 *
 * @param[in] q    Queue, or NULL.
 * @param[in] path File path passed to the matching acquire() (informational).
 */
void aegis_mutation_queue_release(aegis_mutation_queue_t* q, const char* path)
{
    (void)path;
    if (!q) {
        return;
    }
    pthread_mutex_unlock(&q->lock);
}
