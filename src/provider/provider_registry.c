/**
 * @file provider_registry.c
 * @brief Name-keyed provider registry with owned entry storage.
 */
#include "provider_internal.h"

#include <stdlib.h>
#include <string.h>

/* Hash/eq over NUL-terminated names; len parameters are ignored because
 * every stored key is a C string. */
static uint64_t provider_name_hash(const void* key, size_t len, uint64_t seed)
{
    (void)len;
    return aegis_hash_fnv1a(key, strlen((const char*)key), seed);
}

static bool provider_name_eq(const void* a, const void* b, size_t len)
{
    (void)len;
    return strcmp((const char*)a, (const char*)b) == 0;
}

static size_t g_seed_counter = 0; /* Not security-sensitive: collision resistance only. */

/**
 * @brief Create an empty provider registry with a fresh mutex and a
 *        seeded FNV-1a hashmap.
 *
 * Each create call increments a module-global seed counter so that hash
 * collisions across registries are unlikely (no security claim — collision
 * resistance is the only goal). The caller owns the registry and must
 * destroy it with aegis_provider_registry_destroy.
 *
 * @param[out] out Receives the new registry; untouched on failure.
 * @return AEGIS_OK on success, AEGIS_ERR_INVALID for NULL @p out,
 *   AEGIS_ERR_NOMEM on allocation/mutex/hashmap failure.
 */
aegis_status_t aegis_provider_registry_create(aegis_provider_registry_t** out)
{
    if (!out) {
        return AEGIS_ERR_INVALID;
    }

    aegis_provider_registry_t* reg = calloc(1, sizeof(*reg));
    if (!reg) {
        return AEGIS_ERR_NOMEM;
    }
    if (aegis_mutex_create(&reg->lock, AEGIS_MUTEX_PLAIN) != AEGIS_OK) {
        free(reg);
        return AEGIS_ERR_NOMEM;
    }
    if (aegis_hashmap_create(&reg->map, 16, provider_name_hash, provider_name_eq,
                             0x9E3779B97F4A7C15ULL + (uint64_t)g_seed_counter++) != AEGIS_OK) {
        aegis_mutex_destroy(reg->lock);
        free(reg);
        return AEGIS_ERR_NOMEM;
    }
    *out = reg;
    return AEGIS_OK;
}

static void registry_shutdown_all(aegis_provider_registry_t* reg)
{
    /* Caller holds no lock on entry; takes the lock itself per entry so
     * callbacks never run under it. */
    for (size_t i = 0; i < reg->owned_len; i++) {
        aegis_provider_entry_t* entry = reg->owned[i];
        aegis_mutex_lock(reg->lock);
        const bool was_initialized             = (entry->state == AEGIS_PROVIDER_INITIALIZED);
        entry->state                           = AEGIS_PROVIDER_REGISTERED;
        aegis_provider_shutdown_fn shutdown_fn = entry->def.shutdown;
        void*                      user        = entry->def.user;
        aegis_mutex_unlock(reg->lock);

        if (was_initialized && shutdown_fn) {
            shutdown_fn(user);
        }
    }
}

/**
 * @brief Destroy a provider registry, shutting down every entry and
 *        freeing owned structures.
 *
 * NULL is a no-op. Entries that were initialised are shut down first
 * (running their shutdown hook lock-free), then all entry structs and
 * the registry itself are freed.
 *
 * @param[in] reg Registry to destroy, or NULL.
 */
void aegis_provider_registry_destroy(aegis_provider_registry_t* reg)
{
    if (!reg) {
        return;
    }

    registry_shutdown_all(reg);

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

static int owned_append(aegis_provider_registry_t* reg, aegis_provider_entry_t* entry)
{
    if (reg->owned_len == reg->owned_cap) {
        size_t                   cap   = reg->owned_cap ? reg->owned_cap * 2 : 8;
        aegis_provider_entry_t** grown = realloc(reg->owned, cap * sizeof(*grown));
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
 * @brief Register a provider under its name.
 *
 * Validates the definition via @ref aegis_provider_def_check, allocates
 * an owned entry, and publishes it into the registry. Duplicate names
 * are rejected with AEGIS_ERR_BUSY so the existing registration stays
 * authoritative. The def's @c init hook is not called here; call
 * @ref aegis_provider_init afterwards to transition to INITIALIZED.
 *
 * @param[in] reg Registry to register into.
 * @param[in] def Provider definition to store (name, ABI version, hooks).
 * @return AEGIS_OK on success, AEGIS_ERR_INVALID when the def is
 *   missing or its ABI mismatches, AEGIS_ERR_BUSY on duplicate name,
 *   AEGIS_ERR_NOMEM on allocation failure.
 *
 * Thread-safe: the registry lock is held from the duplicate check
 * through the map insert.
 */
aegis_status_t aegis_provider_register(aegis_provider_registry_t*  reg,
                                       const aegis_provider_def_t* def)
{
    aegis_status_t rc = aegis_provider_def_check(def);
    if (!reg || rc != AEGIS_OK) {
        return (rc != AEGIS_OK) ? rc : AEGIS_ERR_INVALID;
    }

    aegis_provider_entry_t* entry = calloc(1, sizeof(*entry));
    if (!entry) {
        return AEGIS_ERR_NOMEM;
    }
    entry->def   = *def;
    entry->state = AEGIS_PROVIDER_REGISTERED;

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
 * @brief Look up a registered provider by name into a read-only view.
 *
 * @param[in]  reg  Registry to search.
 * @param[in]  name Provider name to look up.
 * @param[out] view Receives the def and current lifecycle state.
 * @return AEGIS_OK on success, AEGIS_ERR_INVALID for NULL args,
 *   AEGIS_ERR_NOT_FOUND when the provider is unknown.
 *
 * Thread-safe: the registry lock is held during the lookup.
 */
aegis_status_t aegis_provider_find(const aegis_provider_registry_t* reg, const char* name,
                                   aegis_provider_view_t* view)
{
    if (!reg || !name || !view) {
        return AEGIS_ERR_INVALID;
    }

    aegis_mutex_lock(reg->lock);
    aegis_provider_entry_t* entry = NULL;
    if (!aegis_hashmap_get(reg->map, name, strlen(name), (void**)&entry)) {
        aegis_mutex_unlock(reg->lock);
        return AEGIS_ERR_NOT_FOUND;
    }
    aegis_provider_entry_view(entry, view);
    aegis_mutex_unlock(reg->lock);
    return AEGIS_OK;
}

/**
 * @brief Remove a provider from the registry and tear it down.
 *
 * The entry is delinked from the map and the owned array (swap-remove),
 * the shutdown hook is run lock-free when it was initialised, and the
 * entry struct is freed. The def's borrowed strings remain the
 * provider author's responsibility.
 *
 * @param[in] reg  Registry to unregister from.
 * @param[in] name Provider name to remove.
 * @return AEGIS_OK on success, AEGIS_ERR_INVALID for NULL args,
 *   AEGIS_ERR_NOT_FOUND when the provider is unknown,
 *   AEGIS_ERR_INTERNAL if the map remove unexpectedly fails.
 *
 * Thread-safe: the registry lock is held from the lookup through the
 * array swap-remove; the shutdown hook runs after the unlock.
 */
aegis_status_t aegis_provider_unregister(aegis_provider_registry_t* reg, const char* name)
{
    if (!reg || !name) {
        return AEGIS_ERR_INVALID;
    }

    aegis_mutex_lock(reg->lock);
    aegis_provider_entry_t* entry = NULL;
    if (!aegis_hashmap_get(reg->map, name, strlen(name), (void**)&entry)) {
        aegis_mutex_unlock(reg->lock);
        return AEGIS_ERR_NOT_FOUND;
    }

    const bool was_initialized             = (entry->state == AEGIS_PROVIDER_INITIALIZED);
    entry->state                           = AEGIS_PROVIDER_REGISTERED;
    aegis_provider_shutdown_fn shutdown_fn = entry->def.shutdown;
    void*                      user        = entry->def.user;

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

    /* Lock-free teardown per lifecycle contract. */
    if (was_initialized && shutdown_fn) {
        shutdown_fn(user);
    }
    free(entry);
    return AEGIS_OK;
}

/**
 * @brief Return the number of registered providers.
 *
 * @param[in] reg Registry to count, or NULL (returns 0).
 * @return Number of entries currently in the registry.
 *
 * Thread-safe: the lock is held only for the hashmap length query.
 */
size_t aegis_provider_count(const aegis_provider_registry_t* reg)
{
    if (!reg || !reg->map) {
        return 0;
    }
    aegis_mutex_lock(((aegis_provider_registry_t*)(void*)reg)->lock); /* Logical const. */
    size_t n = aegis_hashmap_len(reg->map);
    aegis_mutex_unlock(((aegis_provider_registry_t*)(void*)reg)->lock);
    return n;
}
