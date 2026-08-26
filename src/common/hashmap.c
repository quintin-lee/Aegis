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

typedef struct Entry {
    void*  key;
    size_t key_len;
    void*  value;
    int    used; /* 1 = occupied, -1 = deleted tombstone */
} Entry;

struct aegis_hashmap {
    Entry*        entries;
    size_t        capacity;
    size_t        count;    /**< Live entries. */
    size_t        occupied; /**< Live entries + tombstones (slots probed past). */
    aegis_hash_fn hash;
    aegis_eq_fn   eq;
    uint64_t      seed;
};

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

bool aegis_eq_bytes(const void* a, const void* b, size_t len)
{
    return memcmp(a, b, len) == 0;
}

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

void aegis_hashmap_destroy(aegis_hashmap_t* map)
{
    free(map->entries);
    free(map);
}

/* Double the table, re-inserting live entries and purging tombstones.
 * @return 0 on success, -1 on allocation failure or capacity overflow. */
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

static size_t hashmap_index(const aegis_hashmap_t* map, uint64_t h)
{
    return h & (map->capacity - 1);
}

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

size_t aegis_hashmap_len(const aegis_hashmap_t* map)
{
    return map ? map->count : 0;
}

bool aegis_hashmap_is_empty(const aegis_hashmap_t* map)
{
    return !map || map->count == 0;
}

void aegis_hashmap_clear(aegis_hashmap_t* map)
{
    if (!map) {
        return;
    }
    memset(map->entries, 0, map->capacity * sizeof(Entry));
    map->count    = 0;
    map->occupied = 0;
}
