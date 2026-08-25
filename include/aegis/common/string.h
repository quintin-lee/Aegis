#ifndef AEGIS_STRING_H
#define AEGIS_STRING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "aegis/common/buffer.h"
#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file string.h
 * @brief Immutable string with copy-on-write sharing hint.
 *
 * Backed by a buffer for storage. String is always null-terminated.
 * Not thread-safe.
 */

typedef struct aegis_string aegis_string_t;

/**
 * @brief Create an empty string.
 */
int aegis_string_create(aegis_string_t** out);

/**
 * @brief Create a string from a null-terminated C string.
 */
int aegis_string_from_cstr(aegis_string_t** out, const char* cstr);

/**
 * @brief Create a string from a byte range.
 */
int aegis_string_from_range(aegis_string_t** out, const uint8_t* data, size_t len);

/**
 * @brief Destroy a string.
 */
void aegis_string_destroy(aegis_string_t* s);

/**
 * @brief Get the underlying C string (null-terminated).
 */
const char* aegis_string_cstr(const aegis_string_t* s);

/**
 * @brief Get the length in bytes (excluding null terminator).
 */
size_t aegis_string_len(const aegis_string_t* s);

/**
 * @brief Check if string is empty.
 */
bool aegis_string_is_empty(const aegis_string_t* s);

/**
 * @brief Compare two strings for equality.
 *
 * @return true if equal.
 */
bool aegis_string_eq(const aegis_string_t* a, const aegis_string_t* b);

/**
 * @brief Append another string to this one (in-place grow).
 *
 * @return 0 on success, -1 on allocation failure.
 */
int aegis_string_append(aegis_string_t* s, const aegis_string_t* other);

/**
 * @brief Substring [offset, offset+len).
 *
 * @param[out] out    Receives new string. Caller owns it.
 * @return 0 on success, -1 on failure.
 */
int aegis_string_substring(const aegis_string_t* s, size_t offset, size_t len,
                           aegis_string_t** out);

#ifdef __cplusplus
}
#endif

#endif /* AEGIS_STRING_H */
