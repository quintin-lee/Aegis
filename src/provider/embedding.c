/**
 * @file embedding.c
 * @brief Typed dispatch for embedding providers.
 */
#include "aegis/provider/embedding.h"

#include "cancellation_internal.h"
#include "provider_internal.h"

#include <stdlib.h>
#include <string.h>

/**
 * @brief Release the vector owned by an embedding result.
 *
 * Frees the @c vector buffer and zeroes the struct. Safe to call on a
 * zeroed or already-freed result (NULL is a no-op).
 *
 * @param[in] res Result to destroy, or NULL.
 */
void aegis_embedding_result_destroy(aegis_embedding_result_t* res)
{
    if (!res) {
        return;
    }
    free(res->vector);
    res->vector = NULL;
    res->dim    = 0;
}

/**
 * @brief Embed a text buffer through a named embedding provider.
 *
 * Resolves the provider under the registry lock, then invokes the
 * embedding callback lock-free per the provider ABI. The output result
 * is zeroed before the call; on success it owns a freshly-allocated
 * vector that the caller must release via
 * @ref aegis_embedding_result_destroy.
 *
 * @param[in]  reg     Provider registry.
 * @param[in]  name    Registered embedding provider name.
 * @param[in]  text    Text to embed; may be NULL only when @p text_len
 *   is zero.
 * @param[in]  text_len Byte length of @p text.
 * @param[in]  token   Cancellation token, or NULL to disable checks.
 * @param[out] out     Receives the embedding vector on success.
 * @return AEGIS_OK on success, AEGIS_ERR_INVALID for NULL args,
 *   AEGIS_ERR_NOT_FOUND when the provider is unknown, AEGIS_ERR_PERM if
 *   not yet initialised, AEGIS_ERR_CANCELLED when the token is tripped.
 */
aegis_status_t aegis_embed(const aegis_provider_registry_t* reg, const char* name, const char* text,
                           size_t text_len, const aegis_cancellation_token_t* token,
                           aegis_embedding_result_t* out)
{
    if (!reg || !name || !out || (!text && text_len > 0)) {
        return AEGIS_ERR_INVALID;
    }
    memset(out, 0, sizeof(*out));

    /* Resolve under the leaf lock; invoke callback lock-free. */
    aegis_mutex_lock(reg->lock);
    aegis_provider_entry_t* entry = NULL;
    if (!aegis_hashmap_get(reg->map, name, strlen(name), (void**)&entry)) {
        aegis_mutex_unlock(reg->lock);
        return AEGIS_ERR_NOT_FOUND;
    }
    if (entry->def.kind != AEGIS_PROVIDER_EMBEDDING) {
        aegis_mutex_unlock(reg->lock);
        return AEGIS_ERR_INVALID;
    }
    if (entry->state != AEGIS_PROVIDER_INITIALIZED) {
        aegis_mutex_unlock(reg->lock);
        return AEGIS_ERR_PERM;
    }
    const aegis_embedding_ops_t* ops = (const aegis_embedding_ops_t*)entry->def.user;
    aegis_mutex_unlock(reg->lock);

    if (!ops || !ops->embed) {
        return AEGIS_ERR_PROVIDER;
    }
    if (token && aegis_cancellation_token_is_cancelled(token)) {
        return AEGIS_ERR_CANCELLED;
    }

    return ops->embed(ops->ctx, text, text_len, token, out);
}
