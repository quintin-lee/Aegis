/**
 * @file tool_registry.c
 * @brief Thread-safe name -> tool-definition registry.
 *
 * Stored definitions are shallow copies; borrowed strings remain owned
 * by the tool author and must outlive the registration. Lookup returns
 * a value copy so callers never hold interior pointers into map storage.
 * There is no unregister API: stored defs stay address-stable by design.
 *
 * Lock order: registry lock is a leaf lock (nothing acquired beneath it).
 */
#include "tool_internal.h"

#include <stdlib.h>
#include <string.h>

static uint64_t str_hash(const void* key, size_t len, uint64_t seed)
{
    (void)len;
    const char* s = (const char*)key;
    return aegis_hash_fnv1a(s, strlen(s), seed);
}

static bool str_eq(const void* a, const void* b, size_t len)
{
    (void)len;
    return strcmp((const char*)a, (const char*)b) == 0;
}

aegis_status_t aegis_tool_registry_create(aegis_tool_registry_t** out)
{
    if (!out) {
        return AEGIS_ERR_INVALID;
    }
    *out = NULL;

    aegis_tool_registry_t* reg = calloc(1, sizeof(*reg));
    if (!reg) {
        return AEGIS_ERR_NOMEM;
    }

    aegis_status_t st = AEGIS_OK;
    if (aegis_mutex_create(&reg->lock, AEGIS_MUTEX_PLAIN) != 0) {
        st = AEGIS_ERR_NOMEM;
        goto fail;
    }
    if (aegis_hashmap_create(&reg->map, 16, str_hash, str_eq, 0x9E3779B97F4A7C15ULL) != 0) {
        st = AEGIS_ERR_NOMEM;
        goto fail;
    }

    *out = reg;
    return AEGIS_OK;

fail:
    if (reg->lock) {
        aegis_mutex_destroy(reg->lock);
    }
    free(reg);
    return st;
}

void aegis_tool_registry_destroy(aegis_tool_registry_t* reg)
{
    if (!reg) {
        return;
    }
    aegis_mutex_lock(reg->lock);
    aegis_hashmap_destroy(reg->map);
    reg->map = NULL;
    for (size_t i = 0; i < reg->owned_len; i++) {
        free(reg->owned[i]);
    }
    free(reg->owned);
    reg->owned     = NULL;
    reg->owned_len = 0;
    aegis_mutex_unlock(reg->lock);
    aegis_mutex_destroy(reg->lock);
    free(reg);
}

aegis_status_t aegis_tool_registry_register(aegis_tool_registry_t* reg, const aegis_tool_def_t* def)
{
    if (!reg || !def || !def->name || def->name[0] == '\0' || !def->execute) {
        return AEGIS_ERR_INVALID;
    }

    aegis_status_t st = AEGIS_OK;
    aegis_mutex_lock(reg->lock);

    aegis_tool_def_t* existing = NULL;
    if (aegis_hashmap_get(reg->map, def->name, strlen(def->name), (void**)&existing)) {
        st = AEGIS_ERR_BUSY;
        goto out;
    }

    aegis_tool_def_t* copy = malloc(sizeof(*copy));
    if (!copy) {
        st = AEGIS_ERR_NOMEM;
        goto out;
    }
    *copy = *def;

    /* Track ownership first: if the map insert fails we can simply pop. */
    if (reg->owned_len == reg->owned_cap) {
        size_t             new_cap = reg->owned_cap ? reg->owned_cap * 2 : 8;
        aegis_tool_def_t** grown   = realloc(reg->owned, new_cap * sizeof(*reg->owned));
        if (!grown) {
            free(copy);
            st = AEGIS_ERR_NOMEM;
            goto out;
        }
        reg->owned     = grown;
        reg->owned_cap = new_cap;
    }

    if (aegis_hashmap_insert(reg->map, def->name, strlen(def->name), copy) != 0) {
        free(copy);
        st = AEGIS_ERR_NOMEM;
        goto out;
    }
    reg->owned[reg->owned_len++] = copy;

out:
    aegis_mutex_unlock(reg->lock);
    return st;
}

aegis_status_t aegis_tool_registry_find(aegis_tool_registry_t* reg, const char* name,
                                        aegis_tool_def_t* out_def)
{
    if (!reg || !name || !out_def) {
        return AEGIS_ERR_INVALID;
    }

    aegis_status_t st = AEGIS_ERR_NOT_FOUND;
    aegis_mutex_lock(reg->lock);

    aegis_tool_def_t* stored = NULL;
    if (aegis_hashmap_get(reg->map, name, strlen(name), (void**)&stored)) {
        *out_def = *stored;
        st       = AEGIS_OK;
    }

    aegis_mutex_unlock(reg->lock);
    return st;
}

size_t aegis_tool_registry_count(const aegis_tool_registry_t* reg)
{
    if (!reg) {
        return 0;
    }
    aegis_mutex_lock(((aegis_tool_registry_t*)(void*)reg)->lock);
    const size_t n = aegis_hashmap_len(reg->map);
    aegis_mutex_unlock(((aegis_tool_registry_t*)(void*)reg)->lock);
    return n;
}
