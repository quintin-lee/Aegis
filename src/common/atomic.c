#define _POSIX_C_SOURCE 200809L
#include "aegis/common/atomic.h"
#include <stdlib.h>

struct aegis_atomic_int {
    int _value;
};

int aegis_atomic_int_create(aegis_atomic_int_t** out, int initial)
{
    if (!out)
        return -1;
    aegis_atomic_int_t* a = calloc(1, sizeof(*a));
    if (!a)
        return -1;
    __atomic_store_n(&a->_value, initial, __ATOMIC_SEQ_CST);
    *out = a;
    return 0;
}

void aegis_atomic_int_destroy(aegis_atomic_int_t* a)
{
    if (!a)
        return;
    free(a);
}

int aegis_atomic_int_load(const aegis_atomic_int_t* a, int* out)
{
    if (!a)
        return -1;
    if (out)
        *out = __atomic_load_n(&a->_value, __ATOMIC_SEQ_CST);
    return 0;
}

void aegis_atomic_int_store(aegis_atomic_int_t* a, int val)
{
    if (!a)
        return;
    __atomic_store_n(&a->_value, val, __ATOMIC_SEQ_CST);
}

int aegis_atomic_int_fetch_add(aegis_atomic_int_t* a, int add, int* old_out)
{
    if (!a)
        return -1;
    int old = __atomic_fetch_add(&a->_value, add, __ATOMIC_SEQ_CST);
    if (old_out)
        *old_out = old;
    return 0;
}

bool aegis_atomic_int_compare_exchange(aegis_atomic_int_t* a, int expected, int desired)
{
    if (!a)
        return false;
    return __atomic_compare_exchange_n(&a->_value, &expected, desired, false, __ATOMIC_SEQ_CST,
                                       __ATOMIC_SEQ_CST);
}
int aegis_atomic_int_inc(aegis_atomic_int_t* a)
{
    if (!a)
        return 0;
    return __atomic_fetch_add(&a->_value, 1, __ATOMIC_SEQ_CST);
}
