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
 * @brief UUID v4 (random) generation and parsing.
 *
 * UUID layout: 16 bytes in network byte order.
 * Hex string representation: 36 chars + null (8-4-4-4-12).
 */

typedef struct aegis_uuid {
    uint8_t bytes[16];
} aegis_uuid_t;

/** Null UUID (all zeros). */
aegis_uuid_t aegis_uuid_null(void);

/**
 * @brief Generate a random UUID v4.
 *
 * Thread-safe. Uses /dev/urandom on POSIX, CryptGenRandom on Windows.
 */
aegis_uuid_t aegis_uuid_generate(void);

/**
 * @brief Parse a UUID from a hex string.
 *
 * Accepts formats:
 *   "550e8400-e29b-41d4-a716-446655440000"
 *   "550E8400-E29B-41D4-A716-446655440000"
 *   "550e8400e29b41d4a716446655440000"
 *
 * @param[in]  str  Hex string (null-terminated).
 * @param[out] out  Parsed UUID.
 * @return true on success.
 */
bool aegis_uuid_parse(const char* str, aegis_uuid_t* out);

/**
 * @brief Format a UUID into a hex string.
 *
 * buf must be at least 37 bytes.
 */
void aegis_uuid_format(const aegis_uuid_t* u, char* buf, size_t buf_len);

/**
 * @brief Compare two UUIDs for equality.
 */
bool aegis_uuid_eq(const aegis_uuid_t* a, const aegis_uuid_t* b);

/**
 * @brief Check if a UUID is null (all zeros).
 */
bool aegis_uuid_is_null(const aegis_uuid_t* u);

#ifdef __cplusplus
}
#endif

#endif /* AEGIS_UUID_H */
