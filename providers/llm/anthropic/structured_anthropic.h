/**
 * @file structured_anthropic.h
 * @brief Anthropic (Claude) structured/streaming model backend (libcurl).
 *
 * Mirrors structured_openai.h but speaks the Anthropic Messages API
 * (POST {base}/v1/messages) instead of OpenAI chat/completions. The two
 * paths share the aegis_llm_shared transport/JSON plumbing; only the wire
 * format (body building, SSE event interpretation, response decoding) is
 * Anthropic-specific here.
 */
#ifndef AEGIS_STRUCTURED_ANTHROPIC_H
#define AEGIS_STRUCTURED_ANTHROPIC_H

#include "aegis/model/model.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Opaque per-call context; owns api_key / base_url / model strings. */
typedef struct aegis_anthropic_model_ctx aegis_anthropic_model_ctx_t;

/**
 * Create the Anthropic backend context and fill @p backend with the
 * provider function pointers.
 *
 * @param[in]  api_key  x-api-key header value; NULL falls back to the
 *   ANTHROPIC_API_KEY environment variable.
 * @param[in]  base_url API base (default https://api.anthropic.com); the
 *   /v1/messages path is appended.
 * @param[in]  model    Default model name (default claude-sonnet-4-5).
 * @param[out] out      Receives the new context (NULL on failure).
 * @param[out] backend  Receives the backend struct (user + complete +
 *   stream + capabilities).
 * @return AEGIS_OK on success, otherwise the allocation/validation error.
 */
aegis_status_t aegis_anthropic_model_create(const char* api_key, const char* base_url,
                                            const char* model, aegis_anthropic_model_ctx_t** out,
                                            aegis_model_backend_t* backend);

/** Free the context and its owned strings. NULL is a no-op. */
void aegis_anthropic_model_destroy(aegis_anthropic_model_ctx_t* ctx);

#ifdef __cplusplus
}
#endif

#endif /* AEGIS_STRUCTURED_ANTHROPIC_H */
