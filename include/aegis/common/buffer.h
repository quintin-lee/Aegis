#ifndef AEGIS_BUFFER_H
#define AEGIS_BUFFER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file buffer.h
 * @brief Growable byte buffer with amortised O(1) append.
 *
 * Backed by a dynamic array; capacity doubles on overflow.
 * Not thread-safe.
 */

/** Opaque buffer handle. */
typedef struct aegis_buffer aegis_buffer_t;

/**
 * @brief Create an empty buffer with optional initial capacity.
 *
 * @param[out] out       Receives the buffer handle. Ownership: transferred.
 * @param[in]  init_cap  Initial reserved capacity in bytes (0 uses a small default).
 * @return 0 on success, -1 on allocation failure.
 */
int aegis_buffer_create(aegis_buffer_t** out, size_t init_cap);

/**
 * @brief Destroy a buffer and free all backing storage.
 *
 * Safe to call with NULL (no-op).
 *
 * @param buf Handle to destroy (ownership: consumed).
 */
void aegis_buffer_destroy(aegis_buffer_t* buf);

/**
 * @brief Append a byte sequence to the buffer.
 *
 * @param buf  Buffer to grow (borrowed).
 * @param data Source bytes to append (borrowed; must remain valid during the call).
 * @param len  Number of bytes to append.
 * @return 0 on success, -1 on allocation failure.
 */
int aegis_buffer_append(aegis_buffer_t* buf, const uint8_t* data, size_t len);

/**
 * @brief Append a single byte.
 *
 * @param buf  Buffer to grow (borrowed).
 * @param byte Byte value to append.
 * @return 0 on success, -1 on allocation failure.
 */
int aegis_buffer_append_byte(aegis_buffer_t* buf, uint8_t byte);

/**
 * @brief Append a null-terminated C string.
 *
 * @param buf  Buffer to grow (borrowed).
 * @param str  String to append (borrowed; must remain valid during the call).
 * @return 0 on success, -1 on allocation failure.
 */
int aegis_buffer_append_str(aegis_buffer_t* buf, const char* str);

/**
 * @brief Read bytes from the buffer at @p offset without consuming them.
 *
 * @param buf   Buffer to read from (borrowed).
 * @param out   Destination buffer (must hold at least @p len bytes).
 * @param offset Byte offset within the buffer.
 * @param len    Number of bytes to read.
 * @return 0 on success, -1 if offset+len exceeds buffer length.
 */
int aegis_buffer_read(const aegis_buffer_t* buf, uint8_t* out, size_t offset, size_t len);

/**
 * @brief Return a pointer to the underlying bytes.
 *
 * The pointer is valid until the next mutation (append / clear / reserve).
 * Do NOT free this pointer.
 *
 * @param buf Buffer handle (borrowed).
 * @return Pointer to internal storage, or NULL if @p buf is NULL.
 */
const uint8_t* aegis_buffer_data(const aegis_buffer_t* buf);

/**
 * @brief Return the number of valid bytes currently in the buffer.
 *
 * @param buf Buffer handle (borrowed; may be NULL → returns 0).
 * @return Current byte count.
 */
size_t aegis_buffer_len(const aegis_buffer_t* buf);

/**
 * @brief Return the current backing-storage capacity in bytes.
 *
 * @param buf Buffer handle (borrowed; may be NULL → returns 0).
 * @return Allocated capacity.
 */
size_t aegis_buffer_capacity(const aegis_buffer_t* buf);

/**
 * @brief Reset the length to 0 while retaining capacity.
 *
 * Does NOT free backing storage.
 *
 * @param buf Buffer to clear (borrowed; may be NULL — no-op).
 */
void aegis_buffer_clear(aegis_buffer_t* buf);

/**
 * @brief Ensure the buffer can hold at least @p min_cap bytes without reallocating.
 *
 * @param buf      Buffer to grow (borrowed).
 * @param min_cap  Minimum capacity in bytes.
 * @return 0 on success, -1 on allocation failure.
 */
int aegis_buffer_reserve(aegis_buffer_t* buf, size_t min_cap);

#ifdef __cplusplus
}
#endif

#endif /* AEGIS_BUFFER_H */
