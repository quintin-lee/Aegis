/**
 * @file buffer.c
 * @brief Growable byte buffer backed by a dynamic array.
 *
 * Capacity doubles on overflow (amortised O(1) append).
 * Not thread-safe. Caller owns the buffer handle and is responsible
 * for calling aegis_buffer_destroy() when done.
 */
#include "aegis/common/buffer.h"
#include <stdlib.h>
#include <string.h>

/** Opaque growable byte buffer: heap data plus length/capacity bookkeeping. */
struct aegis_buffer {
    uint8_t* data; /**< Heap storage (never NULL after create). */
    size_t   len;  /**< Bytes currently used. */
    size_t   cap;  /**< Bytes allocated. */
};

/** Default initial capacity used when the caller passes 0. */
#define BUFFER_INIT_CAP 64

/**
 * @brief Create a buffer with at least @p init_cap bytes of storage.
 *
 * A zero @p init_cap selects BUFFER_INIT_CAP. Storage is zero-filled.
 *
 * @param[out] out Receives the handle (ownership: transferred).
 * @param init_cap Initial capacity in bytes (0 = default).
 * @return 0 on success, -1 on NULL out or allocation failure.
 */
int aegis_buffer_create(aegis_buffer_t** out, size_t init_cap)
{
    if (!out) {
        return -1;
    }
    aegis_buffer_t* buf = calloc(1, sizeof(*buf));
    if (!buf) {
        return -1;
    }
    size_t cap = init_cap ? init_cap : BUFFER_INIT_CAP;
    buf->data  = (uint8_t*)calloc(cap, 1);
    if (!buf->data) {
        free(buf);
        return -1;
    }
    buf->cap = cap;
    *out     = buf;
    return 0;
}

/**
 * @brief Destroy a buffer and its storage. Safe to call with NULL (no-op).
 *
 * @param buf Handle to destroy (ownership: consumed).
 */
void aegis_buffer_destroy(aegis_buffer_t* buf)
{
    if (!buf) {
        return;
    }
    free(buf->data);
    free(buf);
}

/**
 * @brief Grow capacity by doubling until @p need more bytes fit.
 *
 * Existing content is preserved via realloc; on failure the buffer is
 * left unchanged.
 *
 * @param buf  Buffer to grow (borrowed).
 * @param need Additional bytes that must fit.
 * @return 0 on success, -1 on allocation failure.
 */
static int buffer_grow(aegis_buffer_t* buf, size_t need)
{
    size_t new_cap = buf->cap;
    while (new_cap < buf->len + need) {
        new_cap *= 2;
    }
    uint8_t* next = (uint8_t*)realloc(buf->data, new_cap);
    if (!next) {
        return -1;
    }
    buf->data = next;
    buf->cap  = new_cap;
    return 0;
}

/**
 * @brief Append @p len bytes to the buffer (grows as needed).
 *
 * Zero-length appends succeed without touching storage.
 *
 * @param buf  Buffer (borrowed).
 * @param data Bytes to copy (borrowed; must be non-NULL).
 * @param len  Number of bytes to copy.
 * @return 0 on success, -1 on NULL args or allocation failure.
 */
int aegis_buffer_append(aegis_buffer_t* buf, const uint8_t* data, size_t len)
{
    if (!buf || !data) {
        return -1;
    }
    if (len == 0) {
        return 0;
    }
    if (buffer_grow(buf, len) != 0) {
        return -1;
    }
    memcpy(buf->data + buf->len, data, len);
    buf->len += len;
    return 0;
}

/**
 * @brief Append a single byte.
 *
 * @param buf  Buffer (borrowed).
 * @param byte Byte to append.
 * @return 0 on success, -1 on NULL buffer or allocation failure.
 */
int aegis_buffer_append_byte(aegis_buffer_t* buf, uint8_t byte)
{
    return aegis_buffer_append(buf, &byte, 1);
}

/**
 * @brief Append a NUL-terminated string without its terminator.
 *
 * @param buf Buffer (borrowed).
 * @param str String to copy (borrowed; must be non-NULL).
 * @return 0 on success, -1 on NULL args or allocation failure.
 */
int aegis_buffer_append_str(aegis_buffer_t* buf, const char* str)
{
    if (!buf || !str) {
        return -1;
    }
    size_t len = strlen(str);
    return aegis_buffer_append(buf, (const uint8_t*)str, len);
}

/**
 * @brief Copy @p len bytes starting at @p offset into @p out.
 *
 * Bounds-checked: reads past the used length are rejected.
 *
 * @param buf    Buffer (borrowed).
 * @param[out] out Destination (must hold @p len bytes).
 * @param offset Start offset within used bytes.
 * @param len    Bytes to copy.
 * @return 0 on success, -1 on NULL args or out-of-range read.
 */
int aegis_buffer_read(const aegis_buffer_t* buf, uint8_t* out, size_t offset, size_t len)
{
    if (!buf || !out || offset + len > buf->len) {
        return -1;
    }
    memcpy(out, buf->data + offset, len);
    return 0;
}

/**
 * @brief Direct read access to the buffer storage.
 *
 * @param buf Buffer (borrowed).
 * @return Storage pointer (borrowed; valid until next mutation), or NULL for NULL input.
 */
const uint8_t* aegis_buffer_data(const aegis_buffer_t* buf)
{
    return buf ? buf->data : NULL;
}

/**
 * @brief Return the number of used bytes (0 for NULL input).
 *
 * @param buf Buffer (borrowed).
 * @return Used length in bytes.
 */
size_t aegis_buffer_len(const aegis_buffer_t* buf)
{
    return buf ? buf->len : 0;
}

/**
 * @brief Return the allocated capacity in bytes (0 for NULL input).
 *
 * @param buf Buffer (borrowed).
 * @return Allocated capacity in bytes.
 */
size_t aegis_buffer_capacity(const aegis_buffer_t* buf)
{
    return buf ? buf->cap : 0;
}

/**
 * @brief Reset the used length to zero, keeping allocated storage.
 *
 * @param buf Buffer (borrowed; NULL is a no-op).
 */
void aegis_buffer_clear(aegis_buffer_t* buf)
{
    if (buf) {
        buf->len = 0;
    }
}

/**
 * @brief Ensure capacity is at least @p min_cap bytes (exact size, no doubling).
 *
 * Already-sufficient buffers succeed immediately; content is preserved.
 *
 * @param buf     Buffer (borrowed).
 * @param min_cap Required capacity in bytes.
 * @return 0 on success, -1 on NULL buffer or allocation failure.
 */
int aegis_buffer_reserve(aegis_buffer_t* buf, size_t min_cap)
{
    if (!buf) {
        return -1;
    }
    if (buf->cap >= min_cap) {
        return 0;
    }
    uint8_t* next = (uint8_t*)realloc(buf->data, min_cap);
    if (!next) {
        return -1;
    }
    buf->data = next;
    buf->cap  = min_cap;
    return 0;
}
