#ifndef AEGIS_ATOMIC_H
#define AEGIS_ATOMIC_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file atomic.h
 * @brief Lock-free atomic integer operations via __atomic builtins.
 *
 * Thread-safe: all operations use __ATOMIC_SEQ_CST ordering unless
 * otherwise noted. No mutex is required for concurrent access.
 */

/** Opaque atomic integer handle. */
typedef struct aegis_atomic_int aegis_atomic_int_t;

/**
 * @brief Create an atomic int initialised to @p initial.
 *
 * @param[out] out     Receives the handle. Ownership: transferred.
 * @param[in]  initial Initial value.
 * @return 0 on success, negative on allocation failure.
 */
int aegis_atomic_int_create(aegis_atomic_int_t** out, int initial);

/**
 * @brief Destroy an atomic int and free its storage.
 *
 * Safe to call with NULL (no-op).
 *
 * @param a Handle to destroy (ownership: consumed).
 */
void aegis_atomic_int_destroy(aegis_atomic_int_t* a);

/**
 * @brief Atomically load the current value.
 *
 * @param[in]  a    The atomic int (borrowed; must not be destroyed concurrently).
 * @param[out] out  Receives the loaded value (may be NULL — call is still valid).
 * @return 0 on success, negative if @p a is NULL.
 */
int aegis_atomic_int_load(const aegis_atomic_int_t* a, int* out);

/**
 * @brief Atomically store a new value.
 *
 * @param a    The atomic int (borrowed).
 * @param val  New value to store.
 */
void aegis_atomic_int_store(aegis_atomic_int_t* a, int val);

/**
 * @brief Atomically add @p add to the current value; return the old value.
 *
 * @param[in]  a        The atomic int (borrowed).
 * @param[in]  add      Value to add.
 * @param[out] old_out  Receives the value before addition (may be NULL).
 * @return 0 on success, negative if @p a is NULL.
 */
int aegis_atomic_int_fetch_add(aegis_atomic_int_t* a, int add, int* old_out);

/**
 * @brief Compare-and-swap: store @p desired iff current value equals @p expected.
 *
 * @param a         The atomic int (borrowed).
 * @param expected  Value expected to be present.
 * @param desired   Value to store if the comparison succeeds.
 * @return true if the swap occurred, false otherwise.
 */
bool aegis_atomic_int_compare_exchange(aegis_atomic_int_t* a, int expected, int desired);

/**
 * @brief Lock-free increment; returns the old value.
 *
 * Equivalent to fetch_add(a, 1, &old) but inlined for hot paths.
 *
 * @param a  The atomic int (borrowed; must be non-NULL).
 * @return The value before increment.
 */
int aegis_atomic_int_inc(aegis_atomic_int_t* a);

#ifdef __cplusplus
}
#endif

#endif /* AEGIS_ATOMIC_H */
