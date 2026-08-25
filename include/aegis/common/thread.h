#ifndef AEGIS_THREAD_H
#define AEGIS_THREAD_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file thread.h
 * @brief Portable thread abstraction over pthreads / Windows threads.
 *
 * Thread handles are opaque; lifecycle is explicit.
 */

typedef struct aegis_thread aegis_thread_t;

/** Thread entry point signature. */
typedef void* (*aegis_thread_fn)(void* arg);

/**
 * @brief Create and start a thread.
 *
 * @param[out] out       Receives the thread handle. Ownership: transferred.
 * @param[in]  fn        Thread entry function.
 * @param[in]  arg       Argument passed to fn.
 * @param[in]  stack_size Preferred stack size in bytes (0 = system default).
 * @return 0 on success, negative on failure.
 */
int aegis_thread_create(aegis_thread_t** out, aegis_thread_fn fn, void* arg, size_t stack_size);

/**
 * @brief Wait for a thread to finish and destroy it.
 *
 * Safe to call with NULL (no-op).
 */
void aegis_thread_join(aegis_thread_t* t);

/**
 * @brief Destroy a thread without waiting.
 *
 * Only safe after the thread has exited or been detached.
 */
void aegis_thread_destroy(aegis_thread_t* t);

/**
 * @brief Yield the current thread's time slice.
 */
void aegis_thread_yield(void);

/**
 * @brief Get the current thread ID (platform-native).
 */
uint64_t aegis_thread_id(void);

#ifdef __cplusplus
}
#endif

#endif /* AEGIS_THREAD_H */
