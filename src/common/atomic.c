/**
 * @file atomic.c
 * @brief Lock-free atomic integer operations via GCC __atomic builtins.
 *
 * All operations use SEQ_CST memory ordering for simplicity.
 * Thread-safe: concurrent access from multiple threads is safe
 * without external synchronisation.
 */
#define _POSIX_C_SOURCE 200809L
#include "aegis/common/atomic.h"
#include <stdlib.h>

/** Opaque atomic integer: single SEQ_CST-guarded value. */
struct aegis_atomic_int {
    int _value; /**< Guarded value; access only via __atomic builtins. */
};

/**
 * @brief Create an atomic integer with an initial value.
 *
 * Thread-safe: the value is published with SEQ_CST ordering.
 *
 * @param[out] out Receives the handle (ownership: transferred).
 * @param initial Initial value.
 * @return 0 on success, -1 on NULL out or allocation failure.
 */
int aegis_atomic_int_create(aegis_atomic_int_t** out, int initial)
{
    if (!out) {
        return -1;
    }
    aegis_atomic_int_t* a = calloc(1, sizeof(*a));
    if (!a) {
        return -1;
    }
    __atomic_store_n(&a->_value, initial, __ATOMIC_SEQ_CST);
    *out = a;
    return 0;
}

/**
 * @brief Destroy an atomic integer. Safe to call with NULL (no-op).
 *
 * @param a Handle to destroy (ownership: consumed).
 */
void aegis_atomic_int_destroy(aegis_atomic_int_t* a)
{
    if (!a) {
        return;
    }
    free(a);
}

/**
 * @brief Atomically load the current value (SEQ_CST).
 *
 * @param a   Handle (borrowed).
 * @param[out] out Receives the value (may be NULL to skip).
 * @return 0 on success, -1 when @p a is NULL.
 */
int aegis_atomic_int_load(const aegis_atomic_int_t* a, int* out)
{
    if (!a) {
        return -1;
    }
    if (out) {
        *out = __atomic_load_n(&a->_value, __ATOMIC_SEQ_CST);
    }
    return 0;
}

/**
 * @brief Atomically store a value (SEQ_CST). No-op when @p a is NULL.
 *
 * @param a   Handle (borrowed).
 * @param val Value to store.
 */
void aegis_atomic_int_store(aegis_atomic_int_t* a, int val)
{
    if (!a) {
        return;
    }
    __atomic_store_n(&a->_value, val, __ATOMIC_SEQ_CST);
}

/**
 * @brief Atomically add @p add and optionally report the previous value.
 *
 * @param a   Handle (borrowed).
 * @param add Delta to add (may be negative).
 * @param[out] old_out Receives the pre-add value (may be NULL).
 * @return 0 on success, -1 when @p a is NULL.
 */
int aegis_atomic_int_fetch_add(aegis_atomic_int_t* a, int add, int* old_out)
{
    if (!a) {
        return -1;
    }
    int old = __atomic_fetch_add(&a->_value, add, __ATOMIC_SEQ_CST);
    if (old_out) {
        *old_out = old;
    }
    return 0;
}

/**
 * @brief Strong compare-and-exchange: store @p desired iff current == @p expected.
 *
 * @param a        Handle (borrowed).
 * @param expected Value the current content is compared against.
 * @param desired  Value to store on match.
 * @return true if the exchange happened, false otherwise (or @p a is NULL).
 */
bool aegis_atomic_int_compare_exchange(aegis_atomic_int_t* a, int expected, int desired)
{
    if (!a) {
        return false;
    }
    return __atomic_compare_exchange_n(&a->_value, &expected, desired, false, __ATOMIC_SEQ_CST,
                                       __ATOMIC_SEQ_CST);
}
/**
 * @brief Atomically increment by one and return the previous value.
 *
 * @param a Handle (borrowed).
 * @return Pre-increment value, or 0 when @p a is NULL.
 */
int aegis_atomic_int_inc(aegis_atomic_int_t* a)
{
    if (!a) {
        return 0;
    }
    return __atomic_fetch_add(&a->_value, 1, __ATOMIC_SEQ_CST);
}
