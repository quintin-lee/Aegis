#ifndef AEGIS_MUTEX_H
#define AEGIS_MUTEX_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file mutex.h
 * @brief Mutex with try-lock, scoped guard, and ownership transfer.
 *
 * Supports both plain (non-recursive) and recursive mutexes.
 * Not thread-safe across different mutex instances — each mutex
 * protects its own critical section independently.
 */

/**
 * @brief Mutex kinds.
 *
 * @var AEGIS_MUTEX_PLAIN
 *   Non-recursive: locking an already-held mutex by the same thread
 *   results in undefined behaviour (typically deadlock).
 * @var AEGIS_MUTEX_RECURSIVE
 *   Recursive: the same thread may lock the mutex multiple times;
 *   an equal number of unlock calls is required before the lock is
 *   fully released.
 */
typedef enum aegis_mutex_kind {
    AEGIS_MUTEX_PLAIN,
    AEGIS_MUTEX_RECURSIVE,
} aegis_mutex_kind_t;

/** Opaque mutex handle. */
typedef struct aegis_mutex aegis_mutex_t;

/**
 * @brief Create a mutex of the specified kind.
 *
 * @param[out] out  Receives the mutex handle. Ownership: transferred.
 * @param[in]  kind Mutex kind (plain or recursive).
 * @return 0 on success, negative on allocation failure.
 */
int aegis_mutex_create(aegis_mutex_t** out, aegis_mutex_kind_t kind);

/**
 * @brief Destroy a mutex and release underlying resources.
 *
 * MUST NOT be called while the mutex is held by any thread.
 *
 * Safe to call with NULL (no-op).
 *
 * @param m Handle to destroy (ownership: consumed).
 */
void aegis_mutex_destroy(aegis_mutex_t* m);

/**
 * @brief Lock the mutex (blocking until acquired).
 *
 * @param m Mutex handle (borrowed; must not be NULL).
 */
void aegis_mutex_lock(aegis_mutex_t* m);

/**
 * @brief Try to lock the mutex without blocking.
 *
 * @param m Mutex handle (borrowed; must not be NULL).
 * @return true if the lock was acquired, false if it was already held.
 */
bool aegis_mutex_trylock(aegis_mutex_t* m);

/**
 * @brief Unlock the mutex.
 *
 * The mutex must be currently held by the calling thread.
 *
 * @param m Mutex handle (borrowed; must not be NULL).
 */
void aegis_mutex_unlock(aegis_mutex_t* m);

/**
 * @brief RAII-style scoped lock guard.
 *
 * Acquires the mutex on creation; releases it when destroyed
 * (or when released via aegis_mutex_guard_release()).
 *
 * Use aegis_mutex_guard_release() only when passing the guard
 * across function boundaries where the callee takes ownership
 * of the lock obligation.
 */
typedef struct aegis_mutex_guard {
    aegis_mutex_t* mutex; /**< Held mutex (NULL after release). */
} aegis_mutex_guard_t;

/**
 * @brief Acquire a lock guard by locking @p m.
 *
 * @param m Mutex to lock (borrowed; must not be NULL).
 * @return Guard holding the lock.
 */
aegis_mutex_guard_t aegis_mutex_guard_lock(aegis_mutex_t* m);

/**
 * @brief Release the mutex from the guard without unlocking.
 *
 * Use when transferring lock obligation across function boundaries.
 * After this call, the guard's mutex field is set to NULL.
 *
 * @param g Pointer to the guard (modified in place).
 * @return The mutex that was held (caller becomes responsible for unlocking).
 */
aegis_mutex_t* aegis_mutex_guard_release(aegis_mutex_guard_t* g);

#ifdef __cplusplus
}
#endif

#endif /* AEGIS_MUTEX_H */
