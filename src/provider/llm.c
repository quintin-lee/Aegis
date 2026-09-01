/**
 * @file llm.c
 * @brief Typed dispatch for LLM providers.
 */
#include "aegis/provider/llm.h"

#include "cancellation_internal.h"
#include "provider_internal.h"

#include <stdlib.h>
#include <string.h>

void aegis_llm_response_destroy(aegis_llm_response_t* resp)
{
    if (!resp) {
        return;
    }
    free(resp->data);
    resp->data = NULL;
    resp->len  = 0;
}

aegis_status_t aegis_llm_complete(const aegis_provider_registry_t* reg, const char* name,
                                  const aegis_llm_request_t*        req,
                                  const aegis_cancellation_token_t* token,
                                  aegis_llm_response_t*             out)
{
    if (!reg || !name || !req || !out) {
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
    if (entry->def.kind != AEGIS_PROVIDER_LLM) {
        aegis_mutex_unlock(reg->lock);
        return AEGIS_ERR_INVALID;
    }
    if (entry->state != AEGIS_PROVIDER_INITIALIZED) {
        aegis_mutex_unlock(reg->lock);
        return AEGIS_ERR_PERM;
    }
    /* def.user is the borrowed ops struct (see aegis_llm_ops_t). */
    const aegis_llm_ops_t* ops = (const aegis_llm_ops_t*)entry->def.user;
    aegis_mutex_unlock(reg->lock);

    if (!ops || !ops->complete) {
        return AEGIS_ERR_PROVIDER;
    }
    if (token && aegis_cancellation_token_is_cancelled(token)) {
        return AEGIS_ERR_CANCELLED;
    }

    return ops->complete(ops->ctx, req, token, out);
    if (!ops || !ops->complete) {
        fprintf(stderr, "error: invalid LLM ops or complete callback\n");
        return AEGIS_ERR_PROVIDER;
    }
    if (!ops->ctx) {
        fprintf(stderr, "error: invalid LLM context\n");
        return AEGIS_ERR_PROVIDER;
    }
    return ops->complete(ops->ctx, req, token, out);
}
}

aegis_status_t aegis_llm_stream(const aegis_provider_registry_t* reg, const char* name,
                                const aegis_llm_request_t*        req,
                                const aegis_cancellation_token_t* token, aegis_llm_stream_fn yield,
                                void* yield_user)
{
    if (!reg || !name || !req || !yield) {
        return AEGIS_ERR_INVALID;
    }

    /* Resolve under the leaf lock; invoke callback lock-free. */
    aegis_mutex_lock(reg->lock);
    aegis_provider_entry_t* entry = NULL;
    if (!aegis_hashmap_get(reg->map, name, strlen(name), (void**)&entry)) {
        aegis_mutex_unlock(reg->lock);
        return AEGIS_ERR_NOT_FOUND;
    }
    if (entry->def.kind != AEGIS_PROVIDER_LLM) {
        aegis_mutex_unlock(reg->lock);
        return AEGIS_ERR_INVALID;
    }
    if (entry->state != AEGIS_PROVIDER_INITIALIZED) {
        aegis_mutex_unlock(reg->lock);
        return AEGIS_ERR_PERM;
    }
    const aegis_llm_ops_t* ops = (const aegis_llm_ops_t*)entry->def.user;
    aegis_mutex_unlock(reg->lock);

    if (!ops) {
        return AEGIS_ERR_PROVIDER;
    }
    if (token && aegis_cancellation_token_is_cancelled(token)) {
        return AEGIS_ERR_CANCELLED;
    }

    /* If provider supports native streaming, use it. */
    if (ops->stream) {
        return ops->stream(ops->ctx, req, token, yield, yield_user);
    }

    /* Fallback: non-streaming complete, yield as single chunk. */
    if (!ops->complete) {
        return AEGIS_ERR_PROVIDER;
    }
    aegis_llm_response_t resp;
    aegis_status_t       rc = ops->complete(ops->ctx, req, token, &resp);
    if (rc != AEGIS_OK) {
        return rc;
    }
    if (resp.data && resp.len > 0) {
        rc = yield(yield_user, resp.data, resp.len, token);
    }
    aegis_llm_response_destroy(&resp);
    return rc;
}
