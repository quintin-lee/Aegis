/**
 * @file provider_llm_mock.h
 * @brief Factory for the mock LLM provider (testing/dev only).
 */
#ifndef AEGIS_PROVIDER_LLM_MOCK_H
#define AEGIS_PROVIDER_LLM_MOCK_H

#include "aegis/llm.h"
#include "aegis/provider.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Opaque mock LLM context. */
typedef struct llm_mock_ctx llm_mock_ctx_t;

/**
 * @brief Create a mock LLM provider instance and its registry definition.
 *
 * @param[out] out_ctx Receives the mock context. Ownership: transferred.
 * @param[out] out_ops Receives the ops struct (BORROWED — owned by registry entry).
 * @param[out] out_def Receives a shallow copy of the provider def.
 * @return AEGIS_OK or AEGIS_ERR_NOMEM.
 */
aegis_status_t aegis_llm_mock_create(llm_mock_ctx_t** out_ctx, const aegis_llm_ops_t** out_ops,
                                     aegis_provider_def_t* out_def);

/**
 * @brief Destroy the mock LLM context.
 *
 * Safe to call with NULL ctx. Does NOT free ops (it's borrowed from registry).
 *
 * @param ctx Context to destroy (ownership: consumed).
 * @param ops Ops struct (borrowed; untouched).
 */
void aegis_llm_mock_destroy(llm_mock_ctx_t* ctx, const aegis_llm_ops_t* ops);

/**
 * @brief Set the failure threshold for the mock LLM provider.
 *
 * @param ctx        Mock context (borrowed).
 * @param fail_after Number of successful calls before failing (0 = never fail).
 */
void aegis_llm_mock_set_fail_after(llm_mock_ctx_t* ctx, int fail_after);

/**
 * @brief Install a sequence of canned responses returned in order.
 *
 * When a sequence is installed, llm_complete() returns the next canned
 * response verbatim (ignoring the prompt) until exhausted, then falls back
 * to echo mode. Each response is borrowed for registration and copied
 * internally. Pass NULL/0 to clear.
 *
 * @param ctx       Mock context (borrowed).
 * @param responses Array of NUL-terminated strings (borrowed).
 * @param count     Number of entries (0 to clear).
 * @return AEGIS_OK or AEGIS_ERR_NOMEM.
 */
aegis_status_t aegis_llm_mock_set_responses(llm_mock_ctx_t* ctx, const char* const* responses,
                                            size_t count);

/**
 * @brief Install a single canned response returned for every call.
 *
 * Convenience for single-response scenarios; equivalent to a length-1
 * sequence that repeats forever.
 *
 * @param ctx      Mock context (borrowed).
 * @param response NUL-terminated response (borrowed, may be NULL to clear).
 * @return AEGIS_OK or AEGIS_ERR_NOMEM.
 */
aegis_status_t aegis_llm_mock_set_response(llm_mock_ctx_t* ctx, const char* response);

#ifdef __cplusplus
}
#endif

#endif /* AEGIS_PROVIDER_LLM_MOCK_H */
