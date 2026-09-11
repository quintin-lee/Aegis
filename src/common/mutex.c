/**
 * @file mutex.c
 * @brief POSIX mutex wrapper with RAII guard support.
 *
 * Wraps pthread_mutex_t; supports both plain and recursive kinds.
 * The guard type provides C++-style RAII locking via aegis_mutex_guard_lock()
 * and aegis_mutex_guard_release().
 */
#define _POSIX_C_SOURCE 200809L
#include "aegis/common/mutex.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

/** Opaque mutex: pthread primitive plus its configured kind. */
struct aegis_mutex {
    pthread_mutex_t inner;     /**< Underlying POSIX mutex. */
    int             recursive; /**< Non-zero when created recursive. */
};

/**
 * @brief Create a mutex of the requested kind.
 *
 * @param[out] out Receives the handle (ownership: transferred).
 * @param kind AEGIS_MUTEX_RECURSIVE for re-entrant locking, plain otherwise.
 * @return 0 on success, -1 on NULL out / allocation failure, or negated errno from pthread_mutex_init.
 */
int aegis_mutex_create(aegis_mutex_t** out, aegis_mutex_kind_t kind)
{
    if (!out) {
        return -1;
    }
    aegis_mutex_t* m = calloc(1, sizeof(*m));
    if (!m) {
        return -1;
    }
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    if (kind == AEGIS_MUTEX_RECURSIVE) {
        pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    }
    int rc = pthread_mutex_init(&m->inner, &attr);
    pthread_mutexattr_destroy(&attr);
    if (rc != 0) {
        free(m);
        return -rc;
    }
    m->recursive = (kind == AEGIS_MUTEX_RECURSIVE) ? 1 : 0;
    *out         = m;
    return 0;
}

/**
 * @brief Destroy a mutex. Safe to call with NULL (no-op).
 *
 * The mutex must be unlocked and no thread may be waiting on it.
 *
 * @param m Handle to destroy (ownership: consumed).
 */
void aegis_mutex_destroy(aegis_mutex_t* m)
{
    if (!m) {
        return;
    }
    pthread_mutex_destroy(&m->inner);
    free(m);
}

/**
 * @brief Lock the mutex, blocking until acquired.
 *
 * @param m Handle (borrowed; must be non-NULL).
 */
void aegis_mutex_lock(aegis_mutex_t* m)
{
    pthread_mutex_lock(&m->inner);
}

/**
 * @brief Attempt to lock without blocking.
 *
 * @param m Handle (borrowed).
 * @return true if the lock was acquired, false if already held.
 */
bool aegis_mutex_trylock(aegis_mutex_t* m)
{
    int rc = pthread_mutex_trylock(&m->inner);
    return rc == 0;
}

/**
 * @brief Unlock a mutex held by the calling thread.
 *
 * @param m Handle (borrowed; must be locked by the caller).
 */
void aegis_mutex_unlock(aegis_mutex_t* m)
{
    pthread_mutex_unlock(&m->inner);
}

/**
 * @brief Lock the mutex and return an RAII guard holding it.
 *
 * A NULL @p m yields an empty guard. The caller must eventually hand
 * the guard to the matching unlock path (guard release semantics live
 * with the guard type, not here).
 *
 * @param m Handle to hold (borrowed; may be NULL).
 * @return Guard owning the lock (or an empty guard for NULL input).
 */
aegis_mutex_guard_t aegis_mutex_guard_lock(aegis_mutex_t* m)
{
    aegis_mutex_guard_t g = {m};
    if (m) {
        pthread_mutex_lock(&m->inner);
    }
    return g;
}

/**
 * @brief Disarm a guard without unlocking and return the raw mutex.
 *
 * Clears the guard so its later destruction will not touch the mutex.
 *
 * @param g Guard to disarm (borrowed; may be NULL).
 * @return The held mutex (borrowed), or NULL when @p g is NULL.
 */
aegis_mutex_t* aegis_mutex_guard_release(aegis_mutex_guard_t* g)
{
    if (!g) {
        return NULL;
    }
    aegis_mutex_t* m = g->mutex;
    g->mutex         = NULL;
    return m;
}
