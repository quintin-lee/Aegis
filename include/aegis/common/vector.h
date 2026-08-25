#ifndef AEGIS_VECTOR_H
#define AEGIS_VECTOR_H

#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file vector.h
 * @brief Generic dynamic array.
 *
 * Opaque handle; all failures return -1.
 * Not thread-safe. Caller manages item data (vector copies bytes only).
 */

typedef struct aegis_vector aegis_vector_t;

/**
 * @brief Create a vector with the given element size.
 *
 * @param[out] out        Receives the vector handle.
 * @param[in]  elem_size  Size of each element in bytes (> 0).
 * @return 0 on success, -1 on allocation failure or invalid argument.
 */
int aegis_vector_create(aegis_vector_t** out, size_t elem_size);

/**
 * @brief Destroy a vector and free all memory.
 */
void aegis_vector_destroy(aegis_vector_t* v);

/**
 * @brief Append an item to the end of the vector.
 *
 * @param[in] v    Vector handle.
 * @param[in] item Pointer to the item to copy (caller retains ownership).
 * @return 0 on success, -1 on allocation failure or invalid argument.
 */
int aegis_vector_push(aegis_vector_t* v, const void* item);

/**
 * @brief Remove the last item and copy it to out.
 *
 * @param[in]  v    Vector handle.
 * @param[out] out  Destination buffer (must be at least elem_size bytes).
 * @return 0 on success, -1 if the vector is empty or invalid.
 */
int aegis_vector_pop(aegis_vector_t* v, void* out);

/**
 * @brief Copy the element at idx to out.
 *
 * @param[in]  v    Vector handle.
 * @param[in]  idx  Element index.
 * @param[out] out  Destination buffer.
 * @return 0 on success, -1 if idx is out of bounds or invalid args.
 */
int aegis_vector_get(const aegis_vector_t* v, size_t idx, void* out);

/**
 * @brief Replace the element at idx with item.
 *
 * @param[in] v    Vector handle.
 * @param[in] idx  Element index.
 * @param[in] item New item data (caller retains ownership).
 * @return 0 on success, -1 if idx is out of bounds or invalid args.
 */
int aegis_vector_set(aegis_vector_t* v, size_t idx, const void* item);

/**
 * @brief Return the number of elements.
 */
size_t aegis_vector_len(const aegis_vector_t* v);

/**
 * @brief Return true if the vector has no elements.
 */
bool aegis_vector_is_empty(const aegis_vector_t* v);

/**
 * @brief Reset length to 0, keeping capacity intact.
 */
void aegis_vector_clear(aegis_vector_t* v);

/**
 * @brief Ensure capacity is at least min_cap elements.
 *
 * @return 0 on success, -1 on allocation failure or invalid argument.
 */
int aegis_vector_reserve(aegis_vector_t* v, size_t min_cap);

#ifdef __cplusplus
}
#endif

#endif /* AEGIS_VECTOR_H */
