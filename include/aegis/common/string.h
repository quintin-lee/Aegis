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
 * @brief Immutable string backed by a growable buffer.
 *
 * String content is null-terminated at all times. The string is
 * immutable after creation — all mutating operations return a new
 * string and leave the original unchanged.
 *
 * Thread safety: NOT thread-safe.
 */

/** Opaque string handle. */
typedef struct aegis_string aegis_string_t;

/**
 * @brief Create an empty string.
 *
 * @param[out] out  Receives the string handle. Ownership: transferred.
 * @return 0 on success, -1 on allocation failure.
 */
int aegis_string_create(aegis_string_t** out);

/**
 * @brief Create a string from a null-terminated C string.
 *
 * @param[out] out   Receives the string handle. Ownership: transferred.
 * @param[in]  cstr  C string to copy (borrowed; must remain valid during the call).
 * @return 0 on success, -1 on allocation failure or invalid arguments.
 */
int aegis_string_from_cstr(aegis_string_t** out, const char* cstr);

/**
 * @brief Create a string from a byte range.
 *
 * @param[out] out   Receives the string handle. Ownership: transferred.
 * @param[in]  data  Byte range to copy (borrowed).
 * @param[in]  len   Number of bytes to copy.
 * @return 0 on success, -1 on allocation failure or invalid arguments.
 */
int aegis_string_from_range(aegis_string_t** out, const uint8_t* data, size_t len);

/**
 * @brief Destroy a string and free all backing storage.
 *
 * Safe to call with NULL (no-op).
 *
 * @param s Handle to destroy (ownership: consumed).
 */
void aegis_string_destroy(aegis_string_t* s);

/**
 * @brief Return a null-terminated C string view.
 *
 * The returned pointer is valid as long as the string is not
 * destroyed. Do NOT free the returned pointer.
 *
 * @param s String handle (borrowed; may be NULL → returns "").
 * @return C string view, or "" if @p s is NULL.
 */
const char* aegis_string_cstr(const aegis_string_t* s);

/**
 * @brief Return the length in bytes (excluding the null terminator).
 *
 * @param s String handle (borrowed; may be NULL → returns 0).
 * @return Byte length.
 */
size_t aegis_string_len(const aegis_string_t* s);

/**
 * @brief Return true if the string is empty (zero bytes).
 *
 * @param s String handle (borrowed; may be NULL → returns true).
 * @return true if empty.
 */
bool aegis_string_is_empty(const aegis_string_t* s);

/**
 * @brief Compare two strings for byte-wise equality.
 *
 * @param a  First string (borrowed; may be NULL → treated as empty).
 * @param b  Second string (borrowed; may be NULL → treated as empty).
 * @return true if both strings are identical (including the case where both are NULL).
 */
bool aegis_string_eq(const aegis_string_t* a, const aegis_string_t* b);

/**
 * @brief Append another string to this one, growing in place.
 *
 * The original string @p s is modified; @p other is untouched.
 *
 * @param s      String to grow (borrowed; must not be NULL).
 * @param other  String to append (borrowed; must not be NULL).
 * @return 0 on success, -1 on allocation failure.
 */
int aegis_string_append(aegis_string_t* s, const aegis_string_t* other);

/**
 * @brief Extract a substring [offset, offset+len) as a new string.
 *
 * @param[in]  s      Source string (borrowed).
 * @param[in]  offset Byte offset of the substring start.
 * @param[in]  len    Number of bytes in the substring.
 * @param[out] out    Receives the new string handle. Ownership: transferred.
 * @return 0 on success, -1 if offset exceeds the string length.
 */
int aegis_string_substring(const aegis_string_t* s, size_t offset, size_t len,
                           aegis_string_t** out);

#ifdef __cplusplus
}
#endif

#endif /* AEGIS_STRING_H */
