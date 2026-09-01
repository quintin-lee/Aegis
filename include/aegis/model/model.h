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

/* ── Client lifecycle ───────────────────────────────────────────────── */

aegis_status_t aegis_model_client_create(const char* model, aegis_model_client_t** out);
void           aegis_model_client_destroy(aegis_model_client_t* client);

/* ── Structured calls ───────────────────────────────────────────────── */

aegis_status_t aegis_model_complete(aegis_model_client_t*             client,
                                    const aegis_model_request_t*      request,
                                    const aegis_cancellation_token_t* token,
                                    aegis_model_response_t**          out);

aegis_status_t aegis_model_stream(aegis_model_client_t*             client,
                                  const aegis_model_request_t*      request,
                                  const aegis_cancellation_token_t* token,
                                  aegis_model_stream_callback_fn callback, void* user);

/* ── Capability ─────────────────────────────────────────────────────── */

aegis_model_capability_t aegis_model_capabilities(const aegis_model_client_t* client);

#ifdef __cplusplus
}
#endif

#endif /* AEGIS_MODEL_H */
