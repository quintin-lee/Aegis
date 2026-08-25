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
 * Keys and values are stored as void* — the user must manage their lifetimes.
 * Hash and equality are provided by the caller.
 * Not thread-safe; external synchronization required.
 */

/** Opaque hash map handle. */
typedef struct aegis_hashmap aegis_hashmap_t;

/** Hash function signature. */
typedef uint64_t (*aegis_hash_fn)(const void* key, size_t key_len, uint64_t seed);

/** Equality function signature. Returns true if keys are equal. */
typedef bool (*aegis_eq_fn)(const void* a, const void* b, size_t len);

/**
 * @brief Default FNV-1a hash.
 */
uint64_t aegis_hash_fnv1a(const void* data, size_t len, uint64_t seed);

/**
 * @brief Default byte-wise equality.
 */
bool aegis_eq_bytes(const void* a, const void* b, size_t len);

/**
 * @brief Create a hash map with the given bucket count (power of two recommended).
 *
 * @param[out] out         Receives the map handle.
 * @param[in]  capacity    Initial bucket count.
 * @param[in]  hash        Hash function (required).
 * @param[in]  eq          Equality function (required).
 * @param[in]  hash_seed   Initial hash seed.
 * @return 0 on success, -1 on allocation failure.
 */
int aegis_hashmap_create(aegis_hashmap_t** out, size_t capacity, aegis_hash_fn hash, aegis_eq_fn eq,
                         uint64_t hash_seed);

/**
 * @brief Destroy the hash map. Does NOT free keys or values.
 */
void aegis_hashmap_destroy(aegis_hashmap_t* map);

/**
 * @brief Insert or update a key-value pair.
 *
 * If key already exists, the old value is replaced (old value is NOT freed —
 * caller must free it explicitly).
 *
 * @return 0 on success, -1 on failure.
 */
int aegis_hashmap_insert(aegis_hashmap_t* map, const void* key, size_t key_len, void* value);

/**
 * @brief Look up a value by key.
 *
 * @param[out] out_value   Receives pointer to value if found.
 * @return true if found, false otherwise.
 */
bool aegis_hashmap_get(const aegis_hashmap_t* map, const void* key, size_t key_len,
                       void** out_value);

/**
 * @brief Remove a key and its value.
 *
 * @return true if key was found and removed.
 */
bool aegis_hashmap_remove(aegis_hashmap_t* map, const void* key, size_t key_len);

/**
 * @brief Return the number of entries.
 */
size_t aegis_hashmap_len(const aegis_hashmap_t* map);

/**
 * @brief Return true if the map is empty.
 */
bool aegis_hashmap_is_empty(const aegis_hashmap_t* map);

/**
 * @brief Clear all entries without freeing keys or values.
 */
void aegis_hashmap_clear(aegis_hashmap_t* map);

#ifdef __cplusplus
}
#endif

#endif /* AEGIS_HASHMAP_H */
