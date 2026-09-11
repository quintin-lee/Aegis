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

/** Initial slot count for a fresh vector. */
#define VECTOR_INIT_CAP 4

/** Generic dynamic array: raw storage plus fixed element stride. */
struct aegis_vector {
    void*  data;      /**< Raw element storage (elem_size * cap bytes). */
    size_t elem_size; /**< Fixed stride per element (set at creation). */
    size_t len;       /**< Elements currently stored. */
    size_t cap;       /**< Allocated element slots. */
};

/**
 * @brief Create a vector for fixed-size elements.
 *
 * Element bytes are always copied in; the caller keeps owning the source.
 *
 * @param[out] out Receives the handle (ownership: transferred).
 * @param elem_size Bytes per element (must be non-zero).
 * @return 0 on success, -1 on NULL out, zero stride, or allocation failure.
 */
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

/**
 * @brief Destroy a vector and its storage (elements are plain bytes, not freed).
 *
 * Safe to call with NULL (no-op).
 *
 * @param v Vector to destroy (ownership: consumed).
 */
void aegis_vector_destroy(aegis_vector_t* v)
{
    if (!v) {
        return;
    }
    free(v->data);
    free(v);
}

/**
 * @brief Grow capacity by doubling until @p need more elements fit.
 *
 * Existing elements are preserved via realloc; on failure the vector is
 * left unchanged.
 *
 * @param v    Vector to grow (borrowed).
 * @param need Additional elements that must fit.
 * @return 0 on success, -1 on allocation failure.
 */
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

/**
 * @brief Copy one element's bytes onto the tail (grows as needed).
 *
 * @param v    Vector (borrowed).
 * @param item Source bytes, exactly elem_size long (borrowed; must be non-NULL).
 * @return 0 on success, -1 on NULL args or allocation failure.
 */
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

/**
 * @brief Remove the tail element, copying its bytes into @p out.
 *
 * The slot keeps its stale bytes; only the length shrinks.
 *
 * @param v   Vector (borrowed).
 * @param[out] out Receives elem_size bytes (must be non-NULL).
 * @return 0 on success, -1 on NULL args or empty vector.
 */
int aegis_vector_pop(aegis_vector_t* v, void* out)
{
    if (!v || !out || v->len == 0) {
        return -1;
    }
    v->len--;
    memcpy(out, (uint8_t*)v->data + v->len * v->elem_size, v->elem_size);
    return 0;
}

/**
 * @brief Copy the element at @p idx into @p out (bounds-checked).
 *
 * @param v   Vector (borrowed).
 * @param idx Element index (must be < len).
 * @param[out] out Receives elem_size bytes (must be non-NULL).
 * @return 0 on success, -1 on NULL args or out-of-range index.
 */
int aegis_vector_get(const aegis_vector_t* v, size_t idx, void* out)
{
    if (!v || !out || idx >= v->len) {
        return -1;
    }
    memcpy(out, (uint8_t*)v->data + idx * v->elem_size, v->elem_size);
    return 0;
}

/**
 * @brief Overwrite the element at @p idx with @p item's bytes.
 *
 * @param v    Vector (borrowed).
 * @param idx  Element index (must be < len).
 * @param item Source bytes, exactly elem_size long (borrowed; must be non-NULL).
 * @return 0 on success, -1 on NULL args or out-of-range index.
 */
int aegis_vector_set(aegis_vector_t* v, size_t idx, const void* item)
{
    if (!v || !item || idx >= v->len) {
        return -1;
    }
    memcpy((uint8_t*)v->data + idx * v->elem_size, item, v->elem_size);
    return 0;
}

/**
 * @brief Return the element count (0 for NULL input).
 *
 * @param v Vector (borrowed).
 * @return Element count.
 */
size_t aegis_vector_len(const aegis_vector_t* v)
{
    return v ? v->len : 0;
}

/**
 * @brief Test whether the vector holds no elements (NULL counts as empty).
 *
 * @param v Vector (borrowed).
 * @return true when empty or NULL, false otherwise.
 */
bool aegis_vector_is_empty(const aegis_vector_t* v)
{
    return !v || v->len == 0;
}

/**
 * @brief Reset the element count to zero, keeping allocated storage.
 *
 * @param v Vector (borrowed; NULL is a no-op).
 */
void aegis_vector_clear(aegis_vector_t* v)
{
    if (v) {
        v->len = 0;
    }
}

/**
 * @brief Ensure capacity is at least @p min_cap elements (exact size, no doubling).
 *
 * Already-sufficient vectors succeed immediately; content is preserved.
 *
 * @param v       Vector (borrowed).
 * @param min_cap Required element slots.
 * @return 0 on success, -1 on NULL vector or allocation failure.
 */
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
