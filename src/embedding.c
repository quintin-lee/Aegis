/**
 * @file embedding.c
 * @brief Typed dispatch for embedding providers.
 */
#include "aegis/embedding.h"

#include "internal/cancellation_internal.h"
#include "internal/provider_internal.h"

#include <stdlib.h>
#include <string.h>

void aegis_embedding_result_destroy(aegis_embedding_result_t* res)
{
    if (!res) {
        return;
    }
    free(res->vector);
    res->vector = NULL;
    res->dim    = 0;
}

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
