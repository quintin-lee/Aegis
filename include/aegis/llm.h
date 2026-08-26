#ifndef AEGIS_LLM_H
#define AEGIS_LLM_H

#include "aegis/cancellation.h"
#include "aegis/provider.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file llm.h
 * @brief Typed dispatch interface for AEGIS_PROVIDER_LLM providers.
 *
 * Deliberately transport-agnostic: requests and responses are byte blobs.
 * No HTTP, JSON, or wire-format binding lives in this ABI — encoding is
 * the provider implementation's concern.
 *
 * Ownership: request memory is borrowed (const). The response OWNS its
 * buffer; release it with aegis_llm_response_destroy() (idempotent).
 *
 * Thread safety: dispatch resolves the provider under the registry lock,
 * then invokes the completion callback lock-free. Concurrent dispatch to
 * one provider must honor the provider's declared thread_model.
 */

/** Completion request (all fields borrowed / by-value). */
typedef struct aegis_llm_request {
    const void* prompt;      /**< Input bytes (borrowed; may be NULL if len 0). */
    size_t      prompt_len;  /**< Prompt length in bytes.                       */
    uint32_t    max_tokens;  /**< Generation budget hint (0 = provider default).*/
    float       temperature; /**< Sampling hint (provider-defined meaning).     */
} aegis_llm_request_t;

/** Completion response. @c data is owned by the holder of this struct. */
typedef struct aegis_llm_response {
    void*  data; /**< Output bytes (owned; NULL when nothing produced). */
    size_t len;  /**< Output length in bytes.                           */
} aegis_llm_response_t;

/**
 * @brief Free response payload and zero the struct. Idempotent.
 */
void aegis_llm_response_destroy(aegis_llm_response_t* resp);

/**
 * @brief Provider-side completion callback (invoked lock-free).
 *
 * On success fill @c out (taking ownership semantics: the dispatcher
 * hands the buffer to the caller). On failure return a non-OK status and
 * leave @c out zeroed.
 *
 * @param user   The registered def's user pointer (borrowed).
 * @param req    Request (borrowed).
 * @param token  Cancellation token (borrowed); cooperative checks required.
 * @param out    Response to fill on success.
 */
typedef aegis_status_t (*aegis_llm_complete_fn)(void* ctx, const aegis_llm_request_t* req,
                                                const aegis_cancellation_token_t* token,
                                                aegis_llm_response_t*             out);

/**
 * @brief Operation set published by an LLM provider.
 *
 * Register by setting def.user to a pointer to this struct (BORROWED,
 * typically a file-scope const object). @c ctx is the provider instance
 * state handed back to every callback. A NULL fn reports
 * AEGIS_ERR_PROVIDER from dispatch.
 */
typedef struct aegis_llm_ops {
    void*                 ctx;      /**< Provider instance state (borrowed). */
    aegis_llm_complete_fn complete; /**< Required for dispatch.             */
} aegis_llm_ops_t;

/**
 * @brief Dispatch a completion through the registry.
 *
 * Resolution gates, in order:
 *   1. unknown name        -> AEGIS_ERR_NOT_FOUND
 *   2. kind != LLM         -> AEGIS_ERR_INVALID
 *   3. not INITIALIZED     -> AEGIS_ERR_PERM
 *   4. missing ops/fn      -> AEGIS_ERR_PROVIDER
 *   5. token pre-cancelled -> AEGIS_ERR_CANCELLED (fn never runs)
 *   6. callback rc propagated verbatim; on failure @c out stays zeroed.
 *
 * @return AEGIS_OK or any status above.
 */
aegis_status_t aegis_llm_complete(const aegis_provider_registry_t* reg, const char* name,
                                  const aegis_llm_request_t*        req,
                                  const aegis_cancellation_token_t* token,
                                  aegis_llm_response_t*             out);

#ifdef __cplusplus
}
#endif

#endif /* AEGIS_LLM_H */
