/**
 * @file vector.c
 * @brief Generic dynamic array with amortised O(1) append.
 *
 * Element size is fixed at creation and stored alongside the handle.
 * All mutations copy element bytes via memcpy; the caller retains
 * ownership of the source data. Capacity doubles on overflow starting
 * from VECTOR_INIT_CAP (4).
 */
#define _POSIX_C_SOURCE 200809L
#include "aegis/common/vector.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define VECTOR_INIT_CAP 4

struct aegis_vector {
    void*  data;
    size_t elem_size;
    size_t len;
    size_t cap;
};

int aegis_vector_create(aegis_vector_t** out, size_t elem_size)
{
    if (!out || elem_size == 0) {
        return -1;
    }
    aegis_vector_t* v = calloc(1, sizeof(*v));
    if (!v) {
        return -1;
    }
    v->elem_size = elem_size;
    v->cap       = VECTOR_INIT_CAP;
    v->data      = calloc(v->cap, elem_size);
    if (!v->data) {
        free(v);
        return -1;
    }
    *out = v;
    return 0;
}

void aegis_vector_destroy(aegis_vector_t* v)
{
    if (!v) {
        return;
    }
    free(v->data);
    free(v);
}

static int vector_grow(aegis_vector_t* v, size_t need)
{
    size_t new_cap = v->cap;
    while (new_cap < v->len + need) {
        new_cap *= 2;
    }
    void* next = realloc(v->data, new_cap * v->elem_size);
    if (!next) {
        return -1;
    }
    v->data = next;
    v->cap  = new_cap;
    return 0;
}

int aegis_vector_push(aegis_vector_t* v, const void* item)
{
    if (!v || !item) {
        return -1;
    }
    if (vector_grow(v, 1) != 0) {
        return -1;
    }
    memcpy((uint8_t*)v->data + v->len * v->elem_size, item, v->elem_size);
    v->len++;
    return 0;
}

int aegis_vector_pop(aegis_vector_t* v, void* out)
{
    if (!v || !out || v->len == 0) {
        return -1;
    }
    v->len--;
    memcpy(out, (uint8_t*)v->data + v->len * v->elem_size, v->elem_size);
    return 0;
}

int aegis_vector_get(const aegis_vector_t* v, size_t idx, void* out)
{
    if (!v || !out || idx >= v->len) {
        return -1;
    }
    memcpy(out, (uint8_t*)v->data + idx * v->elem_size, v->elem_size);
    return 0;
}

int aegis_vector_set(aegis_vector_t* v, size_t idx, const void* item)
{
    if (!v || !item || idx >= v->len) {
        return -1;
    }
    memcpy((uint8_t*)v->data + idx * v->elem_size, item, v->elem_size);
    return 0;
}

size_t aegis_vector_len(const aegis_vector_t* v)
{
    return v ? v->len : 0;
}

bool aegis_vector_is_empty(const aegis_vector_t* v)
{
    return !v || v->len == 0;
}

void aegis_vector_clear(aegis_vector_t* v)
{
    if (v) {
        v->len = 0;
    }
}

int aegis_vector_reserve(aegis_vector_t* v, size_t min_cap)
{
    if (!v || v->elem_size == 0) {
        return -1;
    }
    if (v->cap >= min_cap) {
        return 0;
    }
    void* next = realloc(v->data, min_cap * v->elem_size);
    if (!next) {
        return -1;
    }
    v->data = next;
    v->cap  = min_cap;
    return 0;
}
