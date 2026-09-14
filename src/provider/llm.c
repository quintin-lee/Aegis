/**
 * @file llm.c
 * @brief Typed dispatch for LLM providers.
 */
#include "aegis/provider/llm.h"

#include "cancellation_internal.h"
#include "provider_internal.h"

#include <stdlib.h>
#include <string.h>

/**
 * @brief Release the data buffer owned by an LLM response.
 *
 * Frees @c data and zeroes the struct. Safe to call on a zeroed or
 * already-freed response (NULL is a no-op).
 *
 * @param[in] resp Response to destroy, or NULL.
 */
void aegis_llm_response_destroy(aegis_llm_response_t* resp)
{
    if (!resp) {
        return;
    }
    free(resp->data);
    resp->data = NULL;
    resp->len  = 0;
}

/**
 * @brief Send a non-streaming LLM completion request through a named
 *        provider.
 *
 * Resolves the provider under the registry lock, then invokes the
 * complete callback lock-free per the provider ABI. The output response
 * is zeroed before the call; on success it owns a data buffer that the
 * caller must release via @ref aegis_llm_response_destroy.
 *
 * @param[in]  reg   Provider registry.
 * @param[in]  name  Registered LLM provider name.
 * @param[in]  req   Completion request (messages, sampling params).
 * @param[in]  token Cancellation token, or NULL to disable checks.
 * @param[out] out   Receives the response on success.
 * @return AEGIS_OK on success, AEGIS_ERR_INVALID for NULL args,
 *   AEGIS_ERR_NOT_FOUND when the provider is unknown, AEGIS_ERR_PERM if
 *   not yet initialised, AEGIS_ERR_CANCELLED when the token is tripped,
 *   else the provider callback error.
 */
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
    if (!ops->ctx) {
        return AEGIS_ERR_PROVIDER;
    }
    if (token && aegis_cancellation_token_is_cancelled(token)) {
        return AEGIS_ERR_CANCELLED;
    }

    return ops->complete(ops->ctx, req, token, out);
}

/**
 * @brief Stream an LLM completion through a named provider.
 *
 * Prefers the provider's native streaming hook when present. Providers
 * that only implement @c complete are transparently adapted: the full
 * response is fetched and yielded as a single chunk. In both paths the
 * provider callback runs lock-free per the ABI.
 *
 * @param[in] reg        Provider registry.
 * @param[in] name       Registered LLM provider name.
 * @param[in] req        Completion request.
 * @param[in] token      Cancellation token, or NULL to disable checks.
 * @param[in] yield      Chunk callback; invoked outside the registry lock.
 * @param[in] yield_user Opaque pointer passed to @p yield.
 * @return AEGIS_OK when all chunks were yielded, AEGIS_ERR_INVALID for
 *   NULL args, AEGIS_ERR_NOT_FOUND / AEGIS_ERR_PERM / AEGIS_ERR_PROVIDER
 *   as in @ref aegis_llm_complete, AEGIS_ERR_CANCELLED when tripped.
 */
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
        if (!ops->ctx) {
            return AEGIS_ERR_PROVIDER;
        }
        return ops->stream(ops->ctx, req, token, yield, yield_user);
    }

    /* Fallback: non-streaming complete, yield as single chunk. */
    if (!ops->complete) {
        return AEGIS_ERR_PROVIDER;
    }
    if (!ops->ctx) {
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
