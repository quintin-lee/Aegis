/**
 * @file strategy_registry.c
 * @brief Name-keyed strategy registry with owned entry storage.
 *
 * Mirrors provider_registry.c: hashmap for lookup plus an ownership
 * side-array because aegis_hashmap_destroy() never frees values.
 */
#include "strategy_internal.h"

#include <stdlib.h>
#include <string.h>

/* Hash/eq over NUL-terminated names; len parameters are ignored because
 * every stored key is a C string. */
/**
 * @brief Hash a strategy name with FNV-1a over its bytes.
 *
 * @param[in] key   NUL-terminated name (the length parameter is ignored).
 * @param[in] len   Ignored; kept for the hashmap signature.
 * @param[in] seed  Hash seed.
 * @return FNV-1a digest of the name bytes.
 */
static uint64_t strategy_name_hash(const void* key, size_t len, uint64_t seed)
{
    (void)len;
    return aegis_hash_fnv1a(key, strlen((const char*)key), seed);
}

/**
 * @brief Compare two strategy names with strcmp.
 *
 * @param[in] a    First NUL-terminated name (length ignored).
 * @param[in] b    Second NUL-terminated name.
 * @param[in] len  Ignored; kept for the hashmap signature.
 * @return True when the names are equal.
 */
static bool strategy_name_eq(const void* a, const void* b, size_t len)
{
    (void)len;
    return strcmp((const char*)a, (const char*)b) == 0;
}

static size_t g_seed_counter = 0; /* Not security-sensitive: collision resistance only. */

/**
 * @brief Validate a strategy definition: named, has a plan entry, matching ABI.
 *
 * @param[in] def  Definition to check.
 * @return AEGIS_OK when usable, AEGIS_ERR_INVALID for NULL/empty name,
 *         missing plan entry, or ABI version mismatch.
 */
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

/**
 * @brief Create an empty strategy registry (hashmap plus ownership side-array).
 *
 * The hashmap never frees values, so entries are additionally tracked in an
 * owned side-array released at destroy time. Each registry gets a distinct
 * hash seed from a process-wide counter (collision resistance only, not
 * security-sensitive).
 *
 * @param[out] out  Receives the new registry; set only on success.
 * @return AEGIS_OK on success, AEGIS_ERR_INVALID for NULL @p out,
 *         AEGIS_ERR_NOMEM on allocation/mutex failure.
 *
 * Ownership: the caller owns the returned registry and must release it with
 * aegis_strategy_registry_destroy(). Thread-safe after creation.
 */
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

/**
 * @brief Destroy a registry with all owned strategy entries.
 *
 * Frees every registered entry via the ownership side-array, then the
 * map, mutex, and registry itself. NULL is accepted and ignored.
 *
 * @param[in] reg  Registry to destroy, or NULL.
 */
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

/**
 * @brief Append an entry to the ownership side-array, growing geometrically.
 *
 * The registry lock must be held.
 *
 * @param[in] reg    Registry owning the side-array.
 * @param[in] entry  Entry to track.
 * @return 0 on success, -1 when growth fails.
 */
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

/**
 * @brief Register a strategy definition under its name (definition copied).
 *
 * Duplicate names are rejected with AEGIS_ERR_BUSY. Ownership is recorded
 * in the side-array before the map insert so a failed insert can roll the
 * ownership append back, keeping both stores consistent.
 *
 * @param[in] reg  Registry to extend.
 * @param[in] def  Definition to copy in (must pass aegis_strategy_def_check()).
 * @return AEGIS_OK on success, AEGIS_ERR_INVALID for NULL/bad definition,
 *         AEGIS_ERR_BUSY for a duplicate name, AEGIS_ERR_NOMEM on allocation
 *         or map-insert failure.
 *
 * Thread-safe.
 */
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

/**
 * @brief Look up a strategy by name and copy its definition into a view.
 *
 * The view holds a value copy, so callers never retain interior registry
 * pointers and need no lifetime pairing with the registry.
 *
 * @param[in]  reg   Registry to search.
 * @param[in]  name  Strategy name.
 * @param[out] view  Receives the definition copy on success.
 * @return AEGIS_OK on success, AEGIS_ERR_INVALID for NULL arguments,
 *         AEGIS_ERR_NOT_FOUND for an unknown name.
 *
 * Thread-safe.
 */
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

/**
 * @brief Remove a strategy by name and free its owned entry.
 *
 * Removes the map entry and swap-removes the entry from the ownership
 * side-array under the registry lock. Removing the entry the map just
 * returned cannot fail (AEGIS_ERR_INTERNAL guards the impossible path).
 *
 * @param[in] reg   Registry to shrink.
 * @param[in] name  Strategy name to remove.
 * @return AEGIS_OK on success, AEGIS_ERR_INVALID for NULL arguments,
 *         AEGIS_ERR_NOT_FOUND for an unknown name, AEGIS_ERR_INTERNAL when
 *         the just-found entry vanishes (cannot happen while holding the lock).
 *
 * Thread-safe.
 */
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

/**
 * @brief Return the number of strategies currently registered.
 *
 * @param[in] reg  Registry to inspect, or NULL.
 * @return Entry count, or 0 for NULL / missing map.
 *
 * Thread-safe (logical const: locking mutates no observable value).
 */
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
