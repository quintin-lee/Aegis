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
#ifndef AEGIS_LLM_H
#define AEGIS_LLM_H

#include "aegis/common/cancellation/cancellation.h"
#include "aegis/provider/provider.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Completion request (all fields borrowed / by-value). */
typedef struct aegis_llm_request {
    const void* prompt;      /**< Input bytes (borrowed; may be NULL if len 0). */
    size_t      prompt_len;  /**< Prompt length in bytes.                       */
    uint32_t    max_tokens;  /**< Generation budget hint (0 = provider default).*/
    float       temperature; /**< Sampling hint (provider-defined meaning).     */
    int         stream;      /**< Non-zero to request streaming response.        */
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
 * @brief Stream chunk callback (invoked lock-free for each chunk).
 *
 * On failure return a non-OK status to abort streaming. Returning
 * AEGIS_OK continues to the next chunk.
 *
 * @param user   Opaque context passed to aegis_llm_stream().
 * @param chunk  Chunk data (borrowed; valid only for duration of call).
 * @param token  Cancellation token (borrowed).
 */
typedef aegis_status_t (*aegis_llm_stream_fn)(void* user, const void* chunk, size_t chunk_len,
                                              const aegis_cancellation_token_t* token);

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
 * @brief Provider-side streaming callback (invoked lock-free).
 *
 * On success invoke @p yield for each chunk. Return AEGIS_OK when done.
 * On failure return non-OK and stop streaming.
 *
 * @param user   The registered def's user pointer (borrowed).
 * @param req    Request (borrowed).
 * @param token  Cancellation token (borrowed).
 * @param yield  Chunk yield callback (borrowed).
 */
typedef aegis_status_t (*aegis_llm_stream_callback_fn)(void* ctx, const aegis_llm_request_t* req,
                                                       const aegis_cancellation_token_t* token,
                                                       aegis_llm_stream_fn yield, void* yield_user);

/**
 * @brief Operation set published by an LLM provider.
 *
 * Register by setting def.user to a pointer to this struct (BORROWED,
 * typically a file-scope const object). @c ctx is the provider instance
 * state handed back to every callback. A NULL fn reports
 * AEGIS_ERR_PROVIDER from dispatch.
 */
typedef struct aegis_llm_ops {
    void*                        ctx;      /**< Provider instance state (borrowed). */
    aegis_llm_complete_fn        complete; /**< Required for dispatch.             */
    aegis_llm_stream_callback_fn stream;   /**< Optional streaming callback.        */
} aegis_llm_ops_t;

/**
 * @brief Dispatch a completion through the registry (non-streaming).
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

/**
 * @brief Dispatch a streaming completion through the registry.
 *
 * If the provider does not implement stream, falls back to non-streaming
 * complete() and yields the entire response as a single chunk.
 *
 * Resolution gates follow aegis_llm_complete() with the additional:
 *   - stream callback missing AND provider declared thread_model=SINGLE_THREAD
 *     is still fine (falls back).
 *
 * @param yield_user Opaque context forwarded to each yield callback.
 * @return AEGIS_OK or error status (streaming stops on first non-OK yield).
 */
aegis_status_t aegis_llm_stream(const aegis_provider_registry_t* reg, const char* name,
                                const aegis_llm_request_t*        req,
                                const aegis_cancellation_token_t* token, aegis_llm_stream_fn yield,
                                void* yield_user);

#ifdef __cplusplus
}
#endif

#endif /* AEGIS_LLM_H */
