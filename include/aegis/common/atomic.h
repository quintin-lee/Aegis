#ifndef AEGIS_ATOMIC_H
#define AEGIS_ATOMIC_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file atomic.h
 * @brief Atomic operations wrapper using __atomic builtins.
 *
 * Thread-safe: all operations are atomic with appropriate memory ordering.
 */

typedef struct aegis_atomic_int aegis_atomic_int_t;

/**
 * @brief Create an atomic int with the given initial value.
 *
 * @param[out] out  Receives the handle. Ownership: transferred.
 * @param[in]  initial  Starting value.
 * @return 0 on success, negative on failure.
 */
int aegis_atomic_int_create(aegis_atomic_int_t** out, int initial);

/**
 * @brief Destroy an atomic int. Safe to call with NULL (no-op).
 */
void aegis_atomic_int_destroy(aegis_atomic_int_t* a);

/**
 * @brief Atomically load the current value.
 *
 * @param[in]  a    The atomic int (borrowed).
 * @param[out] out  Receives the loaded value.
 * @return 0 on success, negative on failure.
 */
int aegis_atomic_int_load(const aegis_atomic_int_t* a, int* out);

/**
 * @brief Atomically store a new value.
 */
void aegis_atomic_int_store(aegis_atomic_int_t* a, int val);

/**
 * @brief Atomically add to the current value and return the old value.
 *
 * @param[in]  a       The atomic int (borrowed).
 * @param[in]  add     Value to add.
 * @param[out] old_out Receives the old value (may be NULL).
 * @return 0 on success, negative on failure.
 */
int aegis_atomic_int_fetch_add(aegis_atomic_int_t* a, int add, int* old_out);

/**
 * @brief Compare-and-swap.
 *
 * Stores desired into *a iff *a equals expected.
 *
 * @return true if the exchange took place.
 */
bool aegis_atomic_int_compare_exchange(aegis_atomic_int_t* a, int expected, int desired);

/**
 * @brief Fast lock-free increment helper for counters.
 *
 * Equivalent to fetch_add(a, 1, NULL) but inlined for hot paths.
 * Caller must hold a valid aegis_atomic_int_t *.
 */
int aegis_atomic_int_inc(aegis_atomic_int_t* a);

#ifdef __cplusplus
}
#endif

#endif /* AEGIS_ATOMIC_H */
