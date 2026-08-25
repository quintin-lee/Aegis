#ifndef AEGIS_HASHMAP_H
#define AEGIS_HASHMAP_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file hashmap.h
 * @brief Open-addressing hash map with linear probing.
 *
 * Keys and values are stored as raw pointers — the caller manages
 * their lifetimes entirely. The map does NOT copy keys or values.
 *
 * Thread safety: NOT thread-safe. Use an external mutex for
 * concurrent access.
 *
 * Capacity must be a power of two for efficient index hashing
 * (bitmask instead of modulo).
 */

/** Opaque hash map handle. */
typedef struct aegis_hashmap aegis_hashmap_t;

/**
 * @brief Hash function signature.
 *
 * @param key   Key bytes (borrowed).
 * @param len   Length of @p key in bytes.
 * @param seed  Initial hash seed.
 * @return Hash value.
 */
typedef uint64_t (*aegis_hash_fn)(const void* key, size_t len, uint64_t seed);

/**
 * @brief Equality function signature.
 *
 * @param a   First key bytes (borrowed).
 * @param b   Second key bytes (borrowed).
 * @param len Length of each key in bytes.
 * @return true if the two key ranges are byte-equal.
 */
typedef bool (*aegis_eq_fn)(const void* a, const void* b, size_t len);

/**
 * @brief Default FNV-1a hash function.
 *
 * Fast, general-purpose hash suitable for most key types.
 *
 * @param data Key bytes.
 * @param len  Length of @p data.
 * @param seed Initial seed (usually 0).
 * @return Hash value.
 */
uint64_t aegis_hash_fnv1a(const void* data, size_t len, uint64_t seed);

/**
 * @brief Default byte-wise equality comparator.
 *
 * Equivalent to memcmp.
 *
 * @param a   First range.
 * @param b   Second range.
 * @param len Length of each range.
 * @return true if ranges are identical.
 */
bool aegis_eq_bytes(const void* a, const void* b, size_t len);

/**
 * @brief Create a hash map with the given bucket count.
 *
 * @param[out] out       Receives the map handle. Ownership: transferred.
 * @param[in]  capacity  Initial bucket count (power of two recommended).
 * @param[in]  hash      Hash function (required; must not be NULL).
 * @param[in]  eq        Equality function (required; must not be NULL).
 * @param[in]  hash_seed Initial hash seed.
 * @return 0 on success, -1 on allocation failure or invalid arguments.
 */
int aegis_hashmap_create(aegis_hashmap_t** out, size_t capacity,
                         aegis_hash_fn hash, aegis_eq_fn eq,
                         uint64_t hash_seed);

/**
 * @brief Destroy the hash map and free all internal storage.
 *
 * Does NOT free keys or values — the caller is responsible for
 * cleaning up any objects that were stored in the map.
 *
 * Safe to call with NULL (no-op).
 *
 * @param map Handle to destroy (ownership: consumed).
 */
void aegis_hashmap_destroy(aegis_hashmap_t* map);

/**
 * @brief Insert or update a key-value pair.
 *
 * If the key already exists, the old value pointer is overwritten
 * (the old value is NOT freed — the caller must free it explicitly
 * if no longer needed).
 *
 * @param map    Map handle (borrowed).
 * @param key    Key bytes (borrowed; must remain valid for the map's lifetime).
 * @param key_len Length of @p key in bytes.
 * @param value  Value pointer to associate with @p key.
 * @return 0 on success, -1 on allocation failure or invalid arguments.
 */
int aegis_hashmap_insert(aegis_hashmap_t* map, const void* key, size_t key_len,
                         void* value);

/**
 * @brief Look up a value by key.
 *
 * @param map      Map handle (borrowed).
 * @param key      Key bytes to search for (borrowed).
 * @param key_len  Length of @p key in bytes.
 * @param[out] out_value  Receives the value pointer if found (may be NULL).
 * @return true if the key was found, false otherwise.
 */
bool aegis_hashmap_get(const aegis_hashmap_t* map, const void* key, size_t key_len,
                       void** out_value);

/**
 * @brief Remove a key and its associated value from the map.
 *
 * Does NOT free the key or value pointers.
 *
 * @param map      Map handle (borrowed).
 * @param key      Key bytes to remove (borrowed).
 * @param key_len  Length of @p key in bytes.
 * @return true if the key was found and removed, false otherwise.
 */
bool aegis_hashmap_remove(aegis_hashmap_t* map, const void* key, size_t key_len);

/**
 * @brief Return the number of entries currently in the map.
 *
 * @param map Map handle (borrowed; may be NULL → returns 0).
 * @return Entry count.
 */
size_t aegis_hashmap_len(const aegis_hashmap_t* map);

/**
 * @brief Return true if the map contains no entries.
 *
 * @param map Map handle (borrowed; may be NULL → returns true).
 * @return true if empty.
 */
bool aegis_hashmap_is_empty(const aegis_hashmap_t* map);

/**
 * @brief Remove all entries without freeing keys or values.
 *
 * @param map Map handle (borrowed; may be NULL — no-op).
 */
void aegis_hashmap_clear(aegis_hashmap_t* map);

#ifdef __cplusplus
}
#endif

#endif /* AEGIS_HASHMAP_H */
