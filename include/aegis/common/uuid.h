#ifndef AEGIS_UUID_H
#define AEGIS_UUID_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file uuid.h
 * @brief UUID v4 (random) generation, parsing, and formatting.
 *
 * UUID layout: 16 bytes stored in network (big-endian) byte order.
 * Hex string representation follows the canonical 8-4-4-4-12 format
 * with hyphens (36 characters + null terminator = 37 bytes).
 *
 * Thread safety: aegis_uuid_generate() is thread-safe (uses /dev/urandom).
 */

/**
 * @brief UUID type — 16 bytes in network byte order.
 *
 * @field bytes  Raw 16-byte payload. Access directly for byte-level
 *               manipulation; use aegis_uuid_format / aegis_uuid_parse
 *               for human-readable representations.
 */
typedef struct aegis_uuid {
    uint8_t bytes[16];
} aegis_uuid_t;

/**
 * @brief Generate a null UUID (all bytes zero).
 *
 * @return Null UUID value.
 */
aegis_uuid_t aegis_uuid_null(void);

/**
 * @brief Generate a random UUID v4.
 *
 * Uses /dev/urandom on POSIX systems. Falls back to a pseudo-random
 * generator based on wall-clock time if /dev/urandom is unavailable
 * (not cryptographically secure in that case).
 *
 * Thread-safe.
 *
 * @return Random UUID v4.
 */
aegis_uuid_t aegis_uuid_generate(void);

/**
 * @brief Parse a UUID from a hex string.
 *
 * Accepts three input formats:
 *   - Hyphenated:  "550e8400-e29b-41d4-a716-446655440000"  (36 chars)
 *   - Uppercase:   "550E8400-E29B-41D4-A716-446655440000"  (36 chars)
 *   - Raw hex:     "550e8400e29b41d4a716446655440000"      (32 chars)
 *
 * @param[in]  str  Hex string (null-terminated; must not be NULL).
 * @param[out] out  Parsed UUID (must not be NULL).
 * @return true if parsing succeeded, false otherwise.
 */
bool aegis_uuid_parse(const char* str, aegis_uuid_t* out);

/**
 * @brief Format a UUID into a hyphenated hex string.
 *
 * Writes at most @p buf_len bytes including the null terminator.
 * If @p buf_len < 37, the output is truncated (no null terminator
 * is guaranteed in that case).
 *
 * @param u        UUID to format (borrowed; may be NULL — no-op).
 * @param buf      Output buffer (must hold at least 37 bytes).
 * @param buf_len  Size of @p buf in bytes.
 */
void aegis_uuid_format(const aegis_uuid_t* u, char* buf, size_t buf_len);

/**
 * @brief Compare two UUIDs for byte-wise equality.
 *
 * @param a  First UUID (borrowed; may be NULL → treated as null UUID).
 * @param b  Second UUID (borrowed; may be NULL → treated as null UUID).
 * @return true if both UUIDs are identical.
 */
bool aegis_uuid_eq(const aegis_uuid_t* a, const aegis_uuid_t* b);

/**
 * @brief Check if a UUID is null (all bytes zero).
 *
 * @param u  UUID to check (borrowed; may be NULL → returns true).
 * @return true if the UUID is the null UUID.
 */
bool aegis_uuid_is_null(const aegis_uuid_t* u);

#ifdef __cplusplus
}
#endif

#endif /* AEGIS_UUID_H */
