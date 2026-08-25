#ifndef AEGIS_VECTOR_H
#define AEGIS_VECTOR_H

#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file vector.h
 * @brief Generic dynamic array with opaque handle.
 *
 * The vector stores raw bytes of a fixed element size. All operations
 * copy elements by value (memcpy), so the caller retains ownership
 * of the underlying data and is responsible for its lifetime.
 *
 * Thread safety: NOT thread-safe.
 */

/** Opaque vector handle. */
typedef struct aegis_vector aegis_vector_t;

/**
 * @brief Create a vector that stores elements of the given size.
 *
 * @param[out] out       Receives the vector handle. Ownership: transferred.
 * @param[in]  elem_size  Size of each element in bytes (must be > 0).
 * @return 0 on success, -1 on allocation failure or invalid @p elem_size.
 */
int aegis_vector_create(aegis_vector_t** out, size_t elem_size);

/**
 * @brief Destroy a vector and free all backing storage.
 *
 * Does NOT call destructors on elements — the caller is responsible
 * for cleaning up any resources held by the elements.
 *
 * Safe to call with NULL (no-op).
 *
 * @param v Handle to destroy (ownership: consumed).
 */
void aegis_vector_destroy(aegis_vector_t* v);

/**
 * @brief Append an element to the end of the vector.
 *
 * The element bytes are copied from @p item into the vector's
 * internal storage. @p item must remain valid for the duration
 * of the call.
 *
 * @param v    Vector handle (borrowed).
 * @param item Pointer to the element to copy (borrowed; must not be NULL).
 * @return 0 on success, -1 on allocation failure or invalid arguments.
 */
int aegis_vector_push(aegis_vector_t* v, const void* item);

/**
 * @brief Remove the last element and copy it to @p out.
 *
 * @param v    Vector handle (borrowed).
 * @param[out] out  Destination buffer (must be at least elem_size bytes).
 * @return 0 on success, -1 if the vector is empty or invalid.
 */
int aegis_vector_pop(aegis_vector_t* v, void* out);

/**
 * @brief Copy the element at index @p idx to @p out.
 *
 * @param v    Vector handle (borrowed).
 * @param idx  Element index (0-based).
 * @param[out] out  Destination buffer (must be at least elem_size bytes).
 * @return 0 on success, -1 if @p idx is out of bounds or invalid args.
 */
int aegis_vector_get(const aegis_vector_t* v, size_t idx, void* out);

/**
 * @brief Replace the element at index @p idx with bytes from @p item.
 *
 * @param v    Vector handle (borrowed).
 * @param idx  Element index (0-based).
 * @param item New element bytes (borrowed; must not be NULL).
 * @return 0 on success, -1 if @p idx is out of bounds or invalid args.
 */
int aegis_vector_set(aegis_vector_t* v, size_t idx, const void* item);

/**
 * @brief Return the number of elements currently in the vector.
 *
 * @param v Vector handle (borrowed; may be NULL → returns 0).
 * @return Element count.
 */
size_t aegis_vector_len(const aegis_vector_t* v);

/**
 * @brief Return true if the vector contains no elements.
 *
 * @param v Vector handle (borrowed; may be NULL → returns true).
 * @return true if empty.
 */
bool aegis_vector_is_empty(const aegis_vector_t* v);

/**
 * @brief Reset length to 0, keeping capacity intact.
 *
 * Does NOT call destructors on elements.
 *
 * @param v Vector handle (borrowed; may be NULL — no-op).
 */
void aegis_vector_clear(aegis_vector_t* v);

/**
 * @brief Ensure the vector can hold at least @p min_cap elements.
 *
 * @param v         Vector handle (borrowed).
 * @param min_cap   Minimum capacity in elements.
 * @return 0 on success, -1 on allocation failure or invalid @p v.
 */
int aegis_vector_reserve(aegis_vector_t* v, size_t min_cap);

#ifdef __cplusplus
}
#endif

#endif /* AEGIS_VECTOR_H */
