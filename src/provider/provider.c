/**
 * @file provider.c
 * @brief Provider ABI validation and lifecycle transitions.
 */
#include "provider_internal.h"

#include <string.h>

aegis_status_t aegis_provider_def_check(const aegis_provider_def_t* def)
{
    if (!def || !def->name || def->name[0] == '\0') {
        return AEGIS_ERR_INVALID;
    }
    if (def->abi_version != AEGIS_PROVIDER_ABI_VERSION) {
        return AEGIS_ERR_INVALID;
    }
    return AEGIS_OK;
}

void aegis_provider_entry_view(const aegis_provider_entry_t* entry, aegis_provider_view_t* view)
{
    view->def   = entry->def;
    view->state = entry->state;
}

aegis_status_t aegis_provider_init(aegis_provider_registry_t* reg, const char* name)
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
    if (entry->state == AEGIS_PROVIDER_INITIALIZED) {
        aegis_mutex_unlock(reg->lock);
        return AEGIS_ERR_BUSY;
    }
    /* Claim INITIALIZED before running the callback so concurrent init
     * attempts observe BUSY instead of racing duplicate setup. */
    entry->state = AEGIS_PROVIDER_INITIALIZED;

    aegis_provider_init_fn init_fn = entry->def.init;
    void*                  user    = entry->def.user;
    aegis_mutex_unlock(reg->lock);

    if (!init_fn) {
        return AEGIS_OK; /* Stateless provider: trivial lifecycle. */
    }

    /* Runs lock-free per contract. */
    aegis_status_t rc = init_fn(user);
    if (rc != AEGIS_OK) {
        /* Roll the claim back on failure. */
        aegis_mutex_lock(reg->lock);
        entry->state = AEGIS_PROVIDER_REGISTERED;
        aegis_mutex_unlock(reg->lock);
    }
    return rc;
}

aegis_status_t aegis_provider_shutdown(aegis_provider_registry_t* reg, const char* name)
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
    const bool was_initialized = (entry->state == AEGIS_PROVIDER_INITIALIZED);
    entry->state               = AEGIS_PROVIDER_REGISTERED;

    aegis_provider_shutdown_fn shutdown_fn = entry->def.shutdown;
    void*                      user        = entry->def.user;
    aegis_mutex_unlock(reg->lock);

    if (was_initialized && shutdown_fn) {
        shutdown_fn(user); /* Lock-free per contract. */
    }
    return AEGIS_OK;
}
