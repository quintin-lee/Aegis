#ifndef AEGIS_MODEL_H
#define AEGIS_MODEL_H

#include "aegis/model/request.h"
#include "aegis/model/response.h"
#include "aegis/model/stream.h"
#include "aegis/model/capability.h"
#include "aegis/common/cancellation/cancellation.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file model.h
 * @brief Unified Model client ABI — structured + streaming.
 *
 * Provider implements this ABI; Agent Loop consumes it.
 * Old blob `provider/llm.h` remains as compat adapter:
 *   message_list → single prompt string → llm dispatch → response.
 */

typedef struct aegis_model_client aegis_model_client_t;

/** One-shot structured call; @p out receives owned response on success. */
typedef aegis_status_t (*aegis_model_complete_fn)(void* user, const aegis_model_request_t* request,
                                                  const aegis_cancellation_token_t* token,
                                                  aegis_model_response_t**          out);
/** Streaming call; @p callback receives events until END/ERROR, @p callback_user passed through. */
typedef aegis_status_t (*aegis_model_stream_fn)(void* user, const aegis_model_request_t* request,
                                                const aegis_cancellation_token_t* token,
                                                aegis_model_stream_callback_fn    callback,
                                                void*                             callback_user);

typedef struct aegis_model_backend {
    void*                    user;         /**< Borrowed provider context. */
    aegis_model_complete_fn  complete;     /**< May be NULL if stream set. */
    aegis_model_stream_fn    stream;       /**< May be NULL if complete set. */
    aegis_model_capability_t capabilities; /**< Advertised capability bitmask. */
} aegis_model_backend_t;

/* ── Client lifecycle ───────────────────────────────────────────────── */

/**
 * Named mock client (deterministic canned responses, no backend).
 * NULL model/out → INVALID.
 */
aegis_status_t aegis_model_client_create(const char* model, aegis_model_client_t** out);
/**
 * Client backed by @p backend (struct is copied; need not outlive the call).
 * Requires model, backend, out non-NULL and at least one of
 * complete/stream set, else INVALID.
 */
aegis_status_t aegis_model_client_create_with_backend(const char*                  model,
                                                      const aegis_model_backend_t* backend,
                                                      aegis_model_client_t**       out);
/** Destroy client. Safe with NULL. */
void aegis_model_client_destroy(aegis_model_client_t* client);

/* ── Structured calls ───────────────────────────────────────────────── */

/**
 * Run one request; delegates to backend.complete or the mock fallback.
 * NULL client/req/out → INVALID; pre-cancelled token → CANCELLED.
 * @p out is zeroed on entry and receives an owned response on success.
 */
aegis_status_t aegis_model_complete(aegis_model_client_t*             client,
                                    const aegis_model_request_t*      request,
                                    const aegis_cancellation_token_t* token,
                                    aegis_model_response_t**          out);

/**
 * Stream one request; delegates to backend.stream or the mock fallback
 * emitting canned text deltas. NULL client/req/cb → INVALID;
 * pre-cancelled token → CANCELLED.
 */
aegis_status_t aegis_model_stream(aegis_model_client_t*             client,
                                  const aegis_model_request_t*      request,
                                  const aegis_cancellation_token_t* token,
                                  aegis_model_stream_callback_fn callback, void* user);

/* ── Capability ─────────────────────────────────────────────────────── */

/** Advertised capabilities; 0 when client is NULL. */
aegis_model_capability_t aegis_model_capabilities(const aegis_model_client_t* client);

#ifdef __cplusplus
}
#endif

#endif /* AEGIS_MODEL_H */
