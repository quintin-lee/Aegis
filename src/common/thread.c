/**
 * @file thread.c
 * @brief POSIX thread wrapper with explicit lifecycle.
 *
 * Threads are created via aegis_thread_create and must be joined
 * before destruction. aegis_thread_join marks the thread as joined
 * and prevents double-join. aegis_thread_destroy must only be called
 * after join (or after the thread has exited independently).
 */
#define _POSIX_C_SOURCE 200809L
#include "aegis/common/thread.h"
#include <pthread.h>
#include <stdlib.h>

/** Opaque thread: POSIX handle plus join-once bookkeeping. */
struct aegis_thread {
    pthread_t handle; /**< Underlying POSIX thread. */
    int       joined; /**< Non-zero once aegis_thread_join has run. */
};

/**
 * @brief Default worker used when the caller passes a NULL entry point.
 *
 * Exits immediately; keeps thread creation total (no special-casing at
 * the pthread_create call site).
 *
 * @param arg Unused.
 * @return Always NULL.
 */
static void* thread_noop(void* arg)
{
    (void)arg;
    return NULL;
}

/**
 * @brief Create and start a thread running @p fn.
 *
 * A NULL @p fn runs an immediate no-op worker. A positive @p stack_size
 * overrides the default stack; 0 keeps the system default.
 *
 * @param[out] out Receives the handle (ownership: transferred).
 * @param fn   Entry point (borrowed; may be NULL for a no-op thread).
 * @param arg  Argument passed to @p fn (borrowed).
 * @param stack_size Custom stack size in bytes (0 = default).
 * @return 0 on success, -1 on NULL out / allocation failure, or negated errno from pthread_create.
 */
int aegis_thread_create(aegis_thread_t** out, aegis_thread_fn fn, void* arg, size_t stack_size)
{
    if (!out) {
        return -1;
    }
    aegis_thread_t* t = calloc(1, sizeof(*t));
    if (!t) {
        return -1;
    }
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    if (stack_size > 0) {
        pthread_attr_setstacksize(&attr, stack_size);
    }
    aegis_thread_fn real_fn = fn ? fn : thread_noop;
    int             rc      = pthread_create(&t->handle, &attr, (void* (*)(void*))real_fn, arg);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        free(t);
        return -rc;
    }
    *out = t;
    return 0;
}

/**
 * @brief Join a thread, exactly once.
 *
 * No-op for NULL handles and already-joined threads (join-once guard).
 *
 * @param t Handle to join (borrowed).
 */
void aegis_thread_join(aegis_thread_t* t)
{
    if (!t || t->joined) {
        return;
    }
    pthread_join(t->handle, NULL);
    t->joined = 1;
}

/**
 * @brief Destroy a thread handle. Safe to call with NULL (no-op).
 *
 * Must only be called after the thread was joined or exited; destroying
 * a still-running thread leaks its resources.
 *
 * @param t Handle to destroy (ownership: consumed).
 */
void aegis_thread_destroy(aegis_thread_t* t)
{
    if (!t) {
        return;
    }
    free(t);
}

/**
 * @brief Yield the calling thread's remaining time slice.
 */
void aegis_thread_yield(void)
{
    sched_yield();
}

/**
 * @brief Return an opaque ID for the calling thread.
 *
 * @return pthread_self cast to uint64_t (for logging/affinity only).
 */
uint64_t aegis_thread_id(void)
{
    return (uint64_t)pthread_self();
}
