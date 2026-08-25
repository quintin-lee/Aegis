#ifndef AEGIS_BUFFER_H
#define AEGIS_BUFFER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file buffer.h
 * @brief Growable byte buffer with amortized O(1) append.
 *
 * Backed by a dynamic array; capacity doubles on overflow.
 * Not thread-safe.
 */

typedef struct aegis_buffer aegis_buffer_t;

/**
 * @brief Create an empty buffer with optional initial capacity.
 *
 * @param[out] out        Receives the buffer handle.
 * @param[in]  init_cap   Initial reserved capacity (0 = small default).
 * @return 0 on success, -1 on allocation failure.
 */
int aegis_buffer_create(aegis_buffer_t** out, size_t init_cap);

/**
 * @brief Destroy a buffer and free all memory.
 */
void aegis_buffer_destroy(aegis_buffer_t* buf);

/**
 * @brief Append bytes to the buffer.
 *
 * @return 0 on success, -1 on allocation failure.
 */
int aegis_buffer_append(aegis_buffer_t* buf, const uint8_t* data, size_t len);

/**
 * @brief Append a single byte.
 */
int aegis_buffer_append_byte(aegis_buffer_t* buf, uint8_t byte);

/**
 * @brief Append a null-terminated string.
 *
 * @return 0 on success, -1 on failure.
 */
int aegis_buffer_append_str(aegis_buffer_t* buf, const char* str);

/**
 * @brief Read bytes from the buffer into out without consuming them.
 *
 * @param[out] out      Destination buffer.
 * @param[in]  offset   Byte offset in the buffer.
 * @param[in]  len      Number of bytes to read.
 * @return 0 on success, -1 if offset+len exceeds buffer length.
 */
int aegis_buffer_read(const aegis_buffer_t* buf, uint8_t* out, size_t offset, size_t len);

/**
 * @brief Consume and return a pointer to the underlying bytes.
 *
 * The returned pointer is valid until the next mutation.
 * Do NOT free this pointer.
 */
const uint8_t* aegis_buffer_data(const aegis_buffer_t* buf);

/**
 * @brief Return the number of valid bytes.
 */
size_t aegis_buffer_len(const aegis_buffer_t* buf);

/**
 * @brief Return the current capacity.
 */
size_t aegis_buffer_capacity(const aegis_buffer_t* buf);

/**
 * @brief Clear the buffer (retain capacity).
 */
void aegis_buffer_clear(aegis_buffer_t* buf);

/**
 * @brief Reserve at least min_cap bytes without changing length.
 *
 * @return 0 on success, -1 on failure.
 */
int aegis_buffer_reserve(aegis_buffer_t* buf, size_t min_cap);

#ifdef __cplusplus
}
#endif

#endif /* AEGIS_BUFFER_H */
