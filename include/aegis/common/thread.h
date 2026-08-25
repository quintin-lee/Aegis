#ifndef AEGIS_THREAD_H
#define AEGIS_THREAD_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file thread.h
 * @brief Portable thread abstraction over pthreads (POSIX).
 *
 * Thread handles are opaque; lifecycle is explicit:
 * create → join → destroy. Detaching a thread transfers ownership
 * to the runtime and makes join impossible.
 */

/** Opaque thread handle. */
typedef struct aegis_thread aegis_thread_t;

/**
 * @brief Thread entry point function signature.
 *
 * @param arg  Argument passed to the thread at creation.
 * @return Result value passed to aegis_thread_join (ignored by the runtime).
 */
typedef void* (*aegis_thread_fn)(void* arg);

/**
 * @brief Create and start a new thread.
 *
 * @param[out] out        Receives the thread handle. Ownership: transferred.
 * @param[in]  fn         Thread entry function (must not be NULL).
 * @param[in]  arg        Argument passed to @p fn (may be NULL).
 * @param[in]  stack_size Preferred stack size in bytes (0 = system default).
 * @return 0 on success, negative on failure (e.g. resource limit exceeded).
 */
int aegis_thread_create(aegis_thread_t** out, aegis_thread_fn fn, void* arg,
                        size_t stack_size);

/**
 * @brief Wait for a thread to finish and destroy it.
 *
 * Blocks the calling thread until @p t terminates. After this call,
 * @p t is destroyed and must not be used again.
 *
 * Safe to call with NULL (no-op).
 *
 * @param t Thread handle (ownership: consumed).
 */
void aegis_thread_join(aegis_thread_t* t);

/**
 * @brief Destroy a thread handle without waiting.
 *
 * Only safe to call after the thread has already exited (joined)
 * or has been explicitly detached.
 *
 * Safe to call with NULL (no-op).
 *
 * @param t Thread handle (ownership: consumed).
 */
void aegis_thread_destroy(aegis_thread_t* t);

/**
 * @brief Yield the current thread's time slice to the scheduler.
 *
 * Hint to the OS that the caller is willing to give up the CPU.
 */
void aegis_thread_yield(void);

/**
 * @brief Get the ID of the calling thread.
 *
 * @return Platform-native thread ID (uint64_t). Value is only
 *         meaningful for comparison within the same process.
 */
uint64_t aegis_thread_id(void);

#ifdef __cplusplus
}
#endif

#endif /* AEGIS_THREAD_H */
