/**
 * @file openai_llm.h
 * @brief OpenAI-compatible Chat Completions provider (gpt-4, gpt-3.5-turbo, etc.).
 *
 * Talks to any OpenAI-compatible REST endpoint (OpenAI itself, Azure OpenAI,
 * vLLM, LiteLLM, Ollama with openai compat layer, etc.) via HTTPS.
 *
 * Required env vars / config:
 *   OPENAI_API_KEY         — OpenAI API key (required unless --api-key passed)
 *   AEGIS_OPENAI_BASE_URL  — base URL (default https://api.openai.com/v1)
 *
 * CLI usage:
 *   aegis run --provider llm-openai --model gpt-4o --api-key sk-...
 */
#ifndef AEGIS_PROVIDER_OPENAI_LLM_H
#define AEGIS_PROVIDER_OPENAI_LLM_H

#include "aegis/provider/llm.h"
#include "aegis/provider/provider.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Opaque OpenAI LLM provider context. */
typedef struct openai_llm_ctx openai_llm_ctx_t;

/**
 * @brief Create an OpenAI-compatible LLM provider instance.
 *
 * @param[out] out_ctx Receives the context (ownership: transferred).
 * @param[out] out_ops Receives the ops struct (BORROWED — owned by registry).
 * @param[out] out_def Receives a shallow copy of the provider def.
 * @return AEGIS_OK or AEGIS_ERR_NOMEM.
 */
aegis_status_t aegis_openai_llm_create(openai_llm_ctx_t** out_ctx,
                                       const aegis_llm_ops_t** out_ops,
                                       aegis_provider_def_t* out_def);

/**
 * @brief Destroy the OpenAI LLM context.
 *
 * Safe to call with NULL ctx. Does NOT free ops (it's borrowed from registry).
 */
void aegis_openai_llm_destroy(openai_llm_ctx_t* ctx, const aegis_llm_ops_t* ops);

/**
 * @brief Configure the provider with credentials and model.
 *
 * @param ctx     Context (borrowed).
 * @param api_key NUL-terminated API key (borrowed). May be NULL to read
 *                from OPENAI_API_KEY env var at dispatch time.
 * @param base_url NUL-terminated base URL (borrowed). Default: https://api.openai.com/v1
 * @param model   NUL-terminated model id (borrowed). Default: gpt-4o-mini
 */
void aegis_openai_llm_configure(openai_llm_ctx_t* ctx,
                                const char* api_key,
                                const char* base_url,
                                const char* model);

#ifdef __cplusplus
}
#endif

#endif /* AEGIS_PROVIDER_OPENAI_LLM_H */
