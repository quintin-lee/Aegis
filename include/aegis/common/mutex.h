#ifndef AEGIS_MUTEX_H
#define AEGIS_MUTEX_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file mutex.h
 * @brief Mutex with try-lock, scoped lock, and ownership transfer.
 *
 * Supports both plain mutex and recursive mutex.
 */

typedef enum aegis_mutex_kind {
    AEGIS_MUTEX_PLAIN,
    AEGIS_MUTEX_RECURSIVE,
} aegis_mutex_kind_t;

typedef struct aegis_mutex aegis_mutex_t;

/**
 * @brief Create a mutex.
 *
 * @param[out] out  Receives the mutex handle.
 * @param[in]  kind Mutex kind.
 * @return 0 on success.
 */
int aegis_mutex_create(aegis_mutex_t** out, aegis_mutex_kind_t kind);

/**
 * @brief Destroy a mutex. Must not be held.
 */
void aegis_mutex_destroy(aegis_mutex_t* m);

/**
 * @brief Lock the mutex (blocking).
 */
void aegis_mutex_lock(aegis_mutex_t* m);

/**
 * @brief Try to lock without blocking.
 *
 * @return true if lock acquired, false if already held.
 */
bool aegis_mutex_trylock(aegis_mutex_t* m);

/**
 * @brief Unlock the mutex.
 */
void aegis_mutex_unlock(aegis_mutex_t* m);

/**
 * @brief RAII-style scoped lock guard.
 *
 * Acquires on creation, releases on destruction.
 * Safe across functions via aegis_mutex_guard_transfer().
 */
typedef struct aegis_mutex_guard {
    aegis_mutex_t* mutex;
} aegis_mutex_guard_t;

/**
 * @brief Acquire a lock guard.
 */
aegis_mutex_guard_t aegis_mutex_guard_lock(aegis_mutex_t* m);

/**
 * @brief Release a lock guard without unlocking (transfer ownership).
 *
 * Use only when passing the guard across function boundaries.
 */
aegis_mutex_t* aegis_mutex_guard_release(aegis_mutex_guard_t* g);

#ifdef __cplusplus
}
#endif

#endif /* AEGIS_MUTEX_H */
