/**
 * @file string.c
 * @brief Immutable string backed by a growable buffer.
 *
 * All mutating operations (append, substring) create new string handles
 * and leave the original untouched. The underlying buffer is shared
 * via the aegis_buffer_t handle stored inside the string struct.
 */
#define _POSIX_C_SOURCE 200809L
#include "aegis/common/string.h"
#include "buffer_internal.h"
#include <stdlib.h>
#include <string.h>

/** Owned string: thin wrapper around a growable byte buffer. */
struct aegis_string {
    aegis_buffer_t* buf; /**< Owned backing storage. */
};

/**
 * @brief Create an empty string (32-byte initial buffer).
 *
 * @param[out] out Receives the handle (ownership: transferred).
 * @return 0 on success, -1 on NULL out or allocation failure.
 */
int aegis_string_create(aegis_string_t** out)
{
    if (!out) {
        return -1;
    }
    aegis_string_t* s = calloc(1, sizeof(*s));
    if (!s) {
        return -1;
    }
    if (aegis_buffer_create(&s->buf, 32) != 0) {
        free(s);
        return -1;
    }
    *out = s;
    return 0;
}

/**
 * @brief Create a string copying a NUL-terminated C string.
 *
 * @param[out] out Receives the handle (ownership: transferred).
 * @param cstr Source text (borrowed; must be non-NULL).
 * @return 0 on success, -1 on NULL args or allocation failure.
 */
int aegis_string_from_cstr(aegis_string_t** out, const char* cstr)
{
    if (!out || !cstr) {
        return -1;
    }
    aegis_string_t* s;
    int             rc = aegis_string_create(&s);
    if (rc != 0) {
        return rc;
    }
    if (aegis_buffer_append_str(s->buf, cstr) != 0) {
        aegis_string_destroy(s);
        return -1;
    }
    *out = s;
    return 0;
}

/**
 * @brief Create a string copying @p len raw bytes (may contain NULs).
 *
 * @param[out] out Receives the handle (ownership: transferred).
 * @param data Bytes to copy (borrowed; must be non-NULL).
 * @param len  Number of bytes to copy.
 * @return 0 on success, -1 on NULL args or allocation failure.
 */
int aegis_string_from_range(aegis_string_t** out, const uint8_t* data, size_t len)
{
    if (!out || !data) {
        return -1;
    }
    aegis_string_t* s;
    int             rc = aegis_string_create(&s);
    if (rc != 0) {
        return rc;
    }
    if (aegis_buffer_append(s->buf, data, len) != 0) {
        aegis_string_destroy(s);
        return -1;
    }
    *out = s;
    return 0;
}

/**
 * @brief Destroy a string and its buffer. Safe to call with NULL (no-op).
 *
 * @param s Handle to destroy (ownership: consumed).
 */
void aegis_string_destroy(aegis_string_t* s)
{
    if (!s) {
        return;
    }
    aegis_buffer_destroy(s->buf);
    free(s);
}

/**
 * @brief Borrow the content as a NUL-terminated C string.
 *
 * Appends a hidden terminator on first call (mutates the buffer but not
 * the logical length), so the pointer stays valid until the next mutation.
 * Never returns NULL ("" for NULL/empty input).
 *
 * @param s Handle (borrowed).
 * @return Borrowed NUL-terminated text.
 */
const char* aegis_string_cstr(const aegis_string_t* s)
{
    if (!s || !s->buf) {
        return "";
    }
    if (s->buf->data[s->buf->len] != '\0') {
        aegis_buffer_append_byte(s->buf, '\0');
    }
    return (const char*)s->buf->data;
}

/**
 * @brief Return the content length in bytes (0 for NULL input).
 *
 * @param s Handle (borrowed).
 * @return Content length.
 */
size_t aegis_string_len(const aegis_string_t* s)
{
    return s ? aegis_buffer_len(s->buf) : 0;
}

/**
 * @brief Test for empty content (NULL counts as empty).
 *
 * @param s Handle (borrowed).
 * @return true when empty or NULL, false otherwise.
 */
bool aegis_string_is_empty(const aegis_string_t* s)
{
    return s ? aegis_buffer_len(s->buf) == 0 : true;
}

/**
 * @brief Byte-wise equality (two NULLs compare equal).
 *
 * Compares lengths first to avoid touching buffers on trivial mismatch.
 *
 * @param a First string (borrowed; may be NULL).
 * @param b Second string (borrowed; may be NULL).
 * @return true when contents are identical.
 */
bool aegis_string_eq(const aegis_string_t* a, const aegis_string_t* b)
{
    if (!a && !b) {
        return true;
    }
    if (!a || !b) {
        return false;
    }
    size_t la = aegis_buffer_len(a->buf);
    size_t lb = aegis_buffer_len(b->buf);
    if (la != lb) {
        return false;
    }
    return memcmp(aegis_buffer_data(a->buf), aegis_buffer_data(b->buf), la) == 0;
}

/**
 * @brief Append a copy of @p other's content to @p s.
 *
 * @param s     Destination (borrowed).
 * @param other Source (borrowed).
 * @return 0 on success, -1 on NULL args or allocation failure.
 */
int aegis_string_append(aegis_string_t* s, const aegis_string_t* other)
{
    if (!s || !other) {
        return -1;
    }
    return aegis_buffer_append(s->buf, aegis_buffer_data(other->buf), aegis_buffer_len(other->buf));
}

/**
 * @brief Copy the [offset, offset+len) slice into a new string.
 *
 * Overlong @p len is clamped to the available tail; offsets past the end
 * are rejected.
 *
 * @param s      Source (borrowed).
 * @param offset Start byte offset.
 * @param len    Bytes to copy (clamped to the tail).
 * @param[out] out Receives the new handle (ownership: transferred).
 * @return 0 on success, -1 on NULL args, bad offset, or allocation failure.
 */
int aegis_string_substring(const aegis_string_t* s, size_t offset, size_t len, aegis_string_t** out)
{
    if (!s || !out) {
        return -1;
    }
    if (offset > aegis_buffer_len(s->buf)) {
        return -1;
    }
    if (offset + len > aegis_buffer_len(s->buf)) {
        len = aegis_buffer_len(s->buf) - offset;
    }
    const uint8_t* src = aegis_buffer_data(s->buf) + offset;
    return aegis_string_from_range(out, src, len);
}
