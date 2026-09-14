/**
 * @file storage.c
 * @brief Typed dispatch for storage providers.
 */
#include "aegis/storage/storage.h"

#include "cancellation_internal.h"
#include "provider_internal.h"

#include <stdlib.h>
#include <string.h>

/**
 * Shared gate sequence for all storage ops: resolve by name, then kind
 * and state gates. On success returns the provider's ops pointer.
 */
static aegis_status_t storage_resolve(const aegis_provider_registry_t* reg, const char* name,
                                      const aegis_storage_ops_t** ops)
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
    if (entry->def.kind != AEGIS_PROVIDER_STORAGE) {
        aegis_mutex_unlock(reg->lock);
        return AEGIS_ERR_INVALID;
    }
    if (entry->state != AEGIS_PROVIDER_INITIALIZED) {
        aegis_mutex_unlock(reg->lock);
        return AEGIS_ERR_PERM;
    }
    *ops = (const aegis_storage_ops_t*)entry->def.user;
    aegis_mutex_unlock(reg->lock);
    return AEGIS_OK;
}

static bool token_cancelled(const aegis_cancellation_token_t* token)
{
    return token && aegis_cancellation_token_is_cancelled(token);
}

/**
 * @brief Destroy a storage blob, freeing its data buffer. NULL-safe.
 */
void aegis_storage_blob_destroy(aegis_storage_blob_t* blob)
{
    if (!blob) {
        return;
    }
    free(blob->data);
    blob->data = NULL;
    blob->len  = 0;
}

/**
 * @brief Store a key-value pair in a named storage provider.
 *
 * @param[in] reg     Provider registry.
 * @param[in] name    Storage provider name.
 * @param[in] key     Key bytes.
 * @param[in] key_len Key length.
 * @param[in] value   Value bytes (may be NULL when value_len is 0).
 * @param[in] value_len Value length.
 * @param[in] token   Cancellation token (may be NULL).
 * @return AEGIS_OK on success, AEGIS_ERR_INVALID for NULL/bad args,
 *         AEGIS_ERR_NOT_FOUND if provider missing, AEGIS_ERR_PERM if
 *         not initialized, AEGIS_ERR_CANCELLED if token is cancelled.
 */
aegis_status_t aegis_storage_put(const aegis_provider_registry_t* reg, const char* name,
                                 const void* key, size_t key_len, const void* value,
                                 size_t value_len, const aegis_cancellation_token_t* token)
{
    if (!key || key_len == 0 || (!value && value_len > 0)) {
        return AEGIS_ERR_INVALID;
    }

    const aegis_storage_ops_t* ops = NULL;
    aegis_status_t             rc  = storage_resolve(reg, name, &ops);
    if (rc != AEGIS_OK) {
        return rc;
    }
    if (!ops || !ops->put) {
        return AEGIS_ERR_PROVIDER;
    }
    if (token_cancelled(token)) {
        return AEGIS_ERR_CANCELLED;
    }
    return ops->put(ops->ctx, key, key_len, value, value_len, token);
}
/**
 * @brief Retrieve a value from a named storage provider.
 *
 * @param[in] reg     Provider registry.
 * @param[in] name    Storage provider name.
 * @param[in] key     Key bytes.
 * @param[in] key_len Key length.
 * @param[in] token   Cancellation token (may be NULL).
 * @param[out] out    Receives the blob (call aegis_storage_blob_destroy to free).
 * @return AEGIS_OK on success, AEGIS_ERR_NOT_FOUND if key missing,
 *         other errors per provider.
 */
aegis_status_t aegis_storage_get(const aegis_provider_registry_t* reg, const char* name,
                                 const void* key, size_t key_len,
                                 const aegis_cancellation_token_t* token, aegis_storage_blob_t* out)
{
    if (!key || key_len == 0 || !out) {
        return AEGIS_ERR_INVALID;
    }
    memset(out, 0, sizeof(*out));

    const aegis_storage_ops_t* ops = NULL;
    aegis_status_t             rc  = storage_resolve(reg, name, &ops);
    if (rc != AEGIS_OK) {
        return rc;
    }
    if (!ops || !ops->get) {
        return AEGIS_ERR_PROVIDER;
    }
    if (token_cancelled(token)) {
        return AEGIS_ERR_CANCELLED;
    }
    return ops->get(ops->ctx, key, key_len, token, out);
}

/**
 * @brief Delete a key from a named storage provider.
 *
 * @param[in] reg     Provider registry.
 * @param[in] name    Storage provider name.
 * @param[in] key     Key bytes.
 * @param[in] key_len Key length.
 * @param[in] token   Cancellation token (may be NULL).
 * @return AEGIS_OK on success, AEGIS_ERR_NOT_FOUND if key missing,
 *         other errors per provider.
 */
aegis_status_t aegis_storage_delete(const aegis_provider_registry_t* reg, const char* name,
                                    const void* key, size_t key_len,
                                    const aegis_cancellation_token_t* token)
{
    if (!key || key_len == 0) {
        return AEGIS_ERR_INVALID;
    }

    const aegis_storage_ops_t* ops = NULL;
    aegis_status_t             rc  = storage_resolve(reg, name, &ops);
    if (rc != AEGIS_OK) {
        return rc;
    }
    if (!ops || !ops->del) {
        return AEGIS_ERR_PROVIDER;
    }
    if (token_cancelled(token)) {
        return AEGIS_ERR_CANCELLED;
    }
    return ops->del(ops->ctx, key, key_len, token);
}
