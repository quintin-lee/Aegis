/**
 * @file hashmap.c
 * @brief Open-addressing hash map with linear probing.
 *
 * Keys are stored as borrowed pointers — the caller must ensure keys
 * outlive the map. Tombstone entries (used == -1) are used on removal
 * to preserve probe sequences. The table grows (rehash) once the
 * occupied-slot load factor reaches 75%, so insert/get probes always
 * terminate; rehashing also purges tombstones.
 */
#define _POSIX_C_SOURCE 200809L
#include "aegis/common/hashmap.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/** Single hash-table slot: borrowed key plus owned probing state. */
typedef struct Entry {
    void*  key;     /**< Borrowed key pointer (caller-owned). */
    size_t key_len; /**< Key length in bytes. */
    void*  value;   /**< Borrowed value pointer (caller-owned). */
    int    used;    /**< 1 = occupied, -1 = deleted tombstone, 0 = never used. */
} Entry;

/** Open-addressing table with linear probing and power-of-two capacity. */
struct aegis_hashmap {
    Entry*        entries;  /**< Slot array (capacity entries). */
    size_t        capacity; /**< Slot count, always a power of two. */
    size_t        count;    /**< Live entries. */
    size_t        occupied; /**< Live entries + tombstones (slots probed past). */
    aegis_hash_fn hash;     /**< Hash function (borrowed). */
    aegis_eq_fn   eq;       /**< Equality function (borrowed). */
    uint64_t      seed;     /**< Seed passed to the hash function. */
};

/**
 * @brief FNV-1a hash over @p len bytes, seeded with @p seed.
 *
 * @param data Bytes to hash (borrowed).
 * @param len  Number of bytes.
 * @param seed Initial hash value.
 * @return 64-bit digest.
 */
uint64_t aegis_hash_fnv1a(const void* data, size_t len, uint64_t seed)
{
    const uint8_t* p = (const uint8_t*)data;
    uint64_t       h = seed;
    for (size_t i = 0; i < len; i++) {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

/**
 * @brief Byte-wise equality over @p len bytes.
 *
 * @param a First buffer (borrowed).
 * @param b Second buffer (borrowed).
 * @param len Bytes to compare.
 * @return true when the ranges are identical.
 */
bool aegis_eq_bytes(const void* a, const void* b, size_t len)
{
    return memcmp(a, b, len) == 0;
}

/**
 * @brief Create a map with at least @p capacity slots.
 *
 * Zero capacity selects 16; non-power-of-two capacities round up so the
 * bitmask probe arithmetic stays valid. Keys/values are borrowed — the
 * caller must keep them alive while in the map.
 *
 * @param[out] out Receives the handle (ownership: transferred).
 * @param capacity Requested slot count (0 = default 16).
 * @param hash Hash function (borrowed; must be non-NULL).
 * @param eq   Equality function (borrowed; must be non-NULL).
 * @param hash_seed Seed forwarded to @p hash.
 * @return 0 on success, -1 on NULL args or allocation failure.
 */
int aegis_hashmap_create(aegis_hashmap_t** out, size_t capacity, aegis_hash_fn hash, aegis_eq_fn eq,
                         uint64_t hash_seed)
{
    if (!out || !hash || !eq) {
        return -1;
    }
    if (capacity == 0) {
        capacity = 16;
    }
    if ((capacity & (capacity - 1)) != 0) {
        /* Round up to next power of two */
        capacity = 1u << (32 - __builtin_clz(capacity - 1));
    }
    aegis_hashmap_t* map = calloc(1, sizeof(*map));
    if (!map) {
        return -1;
    }
    map->entries = (Entry*)calloc(capacity, sizeof(Entry));
    if (!map->entries) {
        free(map);
        return -1;
    }
    map->capacity = capacity;
    map->hash     = hash;
    map->eq       = eq;
    map->seed     = hash_seed;
    *out          = map;
    return 0;
}

/**
 * @brief Destroy a map and its slot array (keys/values are borrowed, not freed).
 *
 * @param map Handle to destroy (ownership: consumed; must be non-NULL).
 */
void aegis_hashmap_destroy(aegis_hashmap_t* map)
{
    free(map->entries);
    free(map);
}

/**
 * @brief Double the table, re-inserting live entries and purging tombstones.
 *
 * Probe sequences stay valid because every live entry is rehashed into the
 * larger power-of-two table; @c occupied is reset to @c count afterwards.
 *
 * @param map Table to grow (borrowed).
 * @return 0 on success, -1 on allocation failure or capacity overflow.
 */
static int hashmap_rehash(aegis_hashmap_t* map)
{
    if (map->capacity > SIZE_MAX / 2) {
        return -1;
    }
    const size_t new_capacity = map->capacity * 2;
    Entry*       grown        = calloc(new_capacity, sizeof(Entry));
    if (!grown) {
        return -1;
    }
    for (size_t i = 0; i < map->capacity; i++) {
        if (map->entries[i].used != 1) {
            continue;
        }
        uint64_t h = map->hash(map->entries[i].key, map->entries[i].key_len, map->seed);
        size_t   j = (size_t)h & (new_capacity - 1);
        while (grown[j].used != 0) {
            j = (j + 1) & (new_capacity - 1);
        }
        grown[j] = map->entries[i];
    }
    free(map->entries);
    map->entries  = grown;
    map->capacity = new_capacity;
    map->occupied = map->count;
    return 0;
}

/**
 * @brief Map a 64-bit hash to a start slot via capacity bitmask.
 *
 * Valid only because capacity is always a power of two.
 *
 * @param map Table (borrowed).
 * @param h   Full hash value.
 * @return Start slot index.
 */
static size_t hashmap_index(const aegis_hashmap_t* map, uint64_t h)
{
    return h & (map->capacity - 1);
}

/**
 * @brief Insert or update a key/value pair (key stored borrowed).
 *
 * Rehashes first when the occupied load would reach 75%, keeping probe
 * loops terminating. Updating an existing key replaces the value in place
 * without growing the table.
 *
 * @param map     Table (borrowed).
 * @param key     Key pointer stored borrowed (must outlive the map entry).
 * @param key_len Key length in bytes.
 * @param value   Value stored borrowed.
 * @return 0 on success, -1 on NULL map or rehash failure.
 */
int aegis_hashmap_insert(aegis_hashmap_t* map, const void* key, size_t key_len, void* value)
{
    if (!map) {
        return -1;
    }
    /* Keep occupied load below 75% so probe loops always terminate. */
    if ((map->occupied + 1) * 4 >= map->capacity * 3) {
        if (hashmap_rehash(map) != 0) {
            return -1;
        }
    }
    uint64_t h = map->hash(key, key_len, map->seed);
    size_t   i = hashmap_index(map, h);
    while (map->entries[i].used != 0) {
        if (map->entries[i].used == 1 && map->eq(key, map->entries[i].key, key_len)) {
            /* Update existing */
            map->entries[i].value = value;
            return 0;
        }
        i = (i + 1) & (map->capacity - 1);
    }
    map->entries[i].key     = (void*)key;
    map->entries[i].key_len = key_len;
    map->entries[i].value   = value;
    if (map->entries[i].used == 0) {
        map->occupied++;
    }
    map->entries[i].used = 1;
    map->count++;
    return 0;
}

/**
 * @brief Look up a key with linear probing past tombstones.
 *
 * @param map      Table (borrowed).
 * @param key      Key to find (borrowed).
 * @param key_len  Key length in bytes.
 * @param[out] out_value Receives the borrowed value (may be NULL to skip).
 * @return true on hit, false on miss or NULL map.
 */
bool aegis_hashmap_get(const aegis_hashmap_t* map, const void* key, size_t key_len,
                       void** out_value)
{
    if (!map) {
        return false;
    }
    uint64_t h = map->hash(key, key_len, map->seed);
    size_t   i = hashmap_index(map, h);
    while (map->entries[i].used != 0) {
        if (map->entries[i].used == 1 && map->eq(key, map->entries[i].key, key_len)) {
            if (out_value) {
                *out_value = map->entries[i].value;
            }
            return true;
        }
        i = (i + 1) & (map->capacity - 1);
    }
    return false;
}

/**
 * @brief Remove a key, leaving a tombstone to preserve probe sequences.
 *
 * @param map     Table (borrowed).
 * @param key     Key to remove (borrowed).
 * @param key_len Key length in bytes.
 * @return true when an entry was removed, false on miss or NULL map.
 */
bool aegis_hashmap_remove(aegis_hashmap_t* map, const void* key, size_t key_len)
{
    if (!map) {
        return false;
    }
    uint64_t h = map->hash(key, key_len, map->seed);
    size_t   i = hashmap_index(map, h);
    while (map->entries[i].used != 0) {
        if (map->entries[i].used == 1 && map->eq(key, map->entries[i].key, key_len)) {
            map->entries[i].used  = -1;
            map->entries[i].key   = NULL;
            map->entries[i].value = NULL;
            map->count--;
            return true;
        }
        i = (i + 1) & (map->capacity - 1);
    }
    return false;
}

/**
 * @brief Return the live-entry count (0 for NULL input).
 *
 * @param map Table (borrowed).
 * @return Number of live entries.
 */
size_t aegis_hashmap_len(const aegis_hashmap_t* map)
{
    return map ? map->count : 0;
}

/**
 * @brief Test whether the map holds no live entries (NULL counts as empty).
 *
 * @param map Table (borrowed).
 * @return true when empty or NULL, false otherwise.
 */
bool aegis_hashmap_is_empty(const aegis_hashmap_t* map)
{
    return !map || map->count == 0;
}

/**
 * @brief Drop all entries including tombstones, keeping the allocation.
 *
 * No-op for NULL input. Borrowed keys/values are not freed.
 *
 * @param map Table to clear (borrowed).
 */
void aegis_hashmap_clear(aegis_hashmap_t* map)
{
    if (!map) {
        return;
    }
    memset(map->entries, 0, map->capacity * sizeof(Entry));
    map->count    = 0;
    map->occupied = 0;
}
