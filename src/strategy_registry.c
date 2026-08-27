/**
 * @file strategy_registry.c
 * @brief Name-keyed strategy registry with owned entry storage.
 *
 * Mirrors provider_registry.c: hashmap for lookup plus an ownership
 * side-array because aegis_hashmap_destroy() never frees values.
 */
#include "internal/strategy_internal.h"

#include <stdlib.h>
#include <string.h>

/* Hash/eq over NUL-terminated names; len parameters are ignored because
 * every stored key is a C string. */
static uint64_t strategy_name_hash(const void* key, size_t len, uint64_t seed)
{
    (void)len;
    return aegis_hash_fnv1a(key, strlen((const char*)key), seed);
}

static bool strategy_name_eq(const void* a, const void* b, size_t len)
{
    (void)len;
    return strcmp((const char*)a, (const char*)b) == 0;
}

static size_t g_seed_counter = 0; /* Not security-sensitive: collision resistance only. */

aegis_status_t aegis_strategy_def_check(const aegis_strategy_def_t* def)
{
    if (!def || !def->name || def->name[0] == '\0' || !def->plan) {
        return AEGIS_ERR_INVALID;
    }
    if (def->abi_version != AEGIS_STRATEGY_ABI_VERSION) {
        return AEGIS_ERR_INVALID;
    }
    return AEGIS_OK;
}

aegis_status_t aegis_strategy_registry_create(aegis_strategy_registry_t** out)
{
    if (!out) {
        return AEGIS_ERR_INVALID;
    }

    aegis_strategy_registry_t* reg = calloc(1, sizeof(*reg));
    if (!reg) {
        return AEGIS_ERR_NOMEM;
    }
    if (aegis_mutex_create(&reg->lock, AEGIS_MUTEX_PLAIN) != AEGIS_OK) {
        free(reg);
        return AEGIS_ERR_NOMEM;
    }
    if (aegis_hashmap_create(&reg->map, 16, strategy_name_hash, strategy_name_eq,
                             0x9E3779B97F4A7C15ULL + (uint64_t)g_seed_counter++) != AEGIS_OK) {
        aegis_mutex_destroy(reg->lock);
        free(reg);
        return AEGIS_ERR_NOMEM;
    }
    *out = reg;
    return AEGIS_OK;
}

void aegis_strategy_registry_destroy(aegis_strategy_registry_t* reg)
{
    if (!reg) {
        return;
    }

    aegis_mutex_lock(reg->lock);
    for (size_t i = 0; i < reg->owned_len; i++) {
        free(reg->owned[i]);
    }
    free(reg->owned);
    aegis_hashmap_destroy(reg->map);
    reg->map = NULL;
    aegis_mutex_unlock(reg->lock);

    aegis_mutex_destroy(reg->lock);
    free(reg);
}

static int owned_append(aegis_strategy_registry_t* reg, aegis_strategy_entry_t* entry)
{
    if (reg->owned_len == reg->owned_cap) {
        size_t                   cap   = reg->owned_cap ? reg->owned_cap * 2 : 8;
        aegis_strategy_entry_t** grown = realloc(reg->owned, cap * sizeof(*grown));
        if (!grown) {
            return -1;
        }
        reg->owned     = grown;
        reg->owned_cap = cap;
    }
    reg->owned[reg->owned_len++] = entry;
    return 0;
}

aegis_status_t aegis_strategy_register(aegis_strategy_registry_t*  reg,
                                       const aegis_strategy_def_t* def)
{
    aegis_status_t rc = aegis_strategy_def_check(def);
    if (!reg || rc != AEGIS_OK) {
        return (rc != AEGIS_OK) ? rc : AEGIS_ERR_INVALID;
    }

    aegis_strategy_entry_t* entry = calloc(1, sizeof(*entry));
    if (!entry) {
        return AEGIS_ERR_NOMEM;
    }
    entry->def = *def;

    aegis_mutex_lock(reg->lock);
    void* existing = NULL;
    if (aegis_hashmap_get(reg->map, def->name, strlen(def->name), &existing)) {
        aegis_mutex_unlock(reg->lock);
        free(entry);
        return AEGIS_ERR_BUSY;
    }
    /* Own the entry before publishing: map insert cannot fail after this
     * without us rolling the ownership append back. */
    if (owned_append(reg, entry) != 0) {
        aegis_mutex_unlock(reg->lock);
        free(entry);
        return AEGIS_ERR_NOMEM;
    }
    if (aegis_hashmap_insert(reg->map, def->name, strlen(def->name), entry) != AEGIS_OK) {
        reg->owned_len--; /* Roll back ownership. */
        aegis_mutex_unlock(reg->lock);
        free(entry);
        return AEGIS_ERR_NOMEM;
    }
    aegis_mutex_unlock(reg->lock);
    return AEGIS_OK;
}

aegis_status_t aegis_strategy_find(const aegis_strategy_registry_t* reg, const char* name,
                                   aegis_strategy_view_t* view)
{
    if (!reg || !name || !view) {
        return AEGIS_ERR_INVALID;
    }

    aegis_mutex_lock(reg->lock);
    aegis_strategy_entry_t* entry = NULL;
    if (!aegis_hashmap_get(reg->map, name, strlen(name), (void**)&entry)) {
        aegis_mutex_unlock(reg->lock);
        return AEGIS_ERR_NOT_FOUND;
    }
    view->def = entry->def; /* Value copy; callers never hold interior pointers. */
    aegis_mutex_unlock(reg->lock);
    return AEGIS_OK;
}

aegis_status_t aegis_strategy_unregister(aegis_strategy_registry_t* reg, const char* name)
{
    if (!reg || !name) {
        return AEGIS_ERR_INVALID;
    }

    aegis_mutex_lock(reg->lock);
    aegis_strategy_entry_t* entry = NULL;
    if (!aegis_hashmap_get(reg->map, name, strlen(name), (void**)&entry)) {
        aegis_mutex_unlock(reg->lock);
        return AEGIS_ERR_NOT_FOUND;
    }

    if (!aegis_hashmap_remove(reg->map, name, strlen(name))) {
        /* Cannot happen: get() just found it and we hold the lock. */
        aegis_mutex_unlock(reg->lock);
        return AEGIS_ERR_INTERNAL;
    }
    for (size_t i = 0; i < reg->owned_len; i++) {
        if (reg->owned[i] == entry) {
            reg->owned[i] = reg->owned[--reg->owned_len]; /* Swap-remove. */
            break;
        }
    }
    aegis_mutex_unlock(reg->lock);

    free(entry);
    return AEGIS_OK;
}

size_t aegis_strategy_count(const aegis_strategy_registry_t* reg)
{
    if (!reg || !reg->map) {
        return 0;
    }
    aegis_mutex_lock(((aegis_strategy_registry_t*)(void*)reg)->lock); /* Logical const. */
    size_t n = aegis_hashmap_len(reg->map);
    aegis_mutex_unlock(((aegis_strategy_registry_t*)(void*)reg)->lock);
    return n;
}
