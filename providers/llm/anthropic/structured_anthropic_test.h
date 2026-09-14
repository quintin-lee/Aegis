/**
 * @file structured_anthropic_test.h
 * @brief Test seam: expose the complete-response JSON parser.
 *
 * Compiled only when AEGIS_ANTHROPIC_TEST_API is defined; mirrors
 * structured_openai_test.h so unit tests can feed canned Anthropic
 * response bodies to aegis_anthropic_parse_complete_response without
 * a live API.
 */
#ifndef AEGIS_STRUCTURED_ANTHROPIC_TEST_H
#define AEGIS_STRUCTURED_ANTHROPIC_TEST_H

#include "aegis/model/model.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Parse a raw Anthropic Messages-API JSON response body into an owned
 * response. Content-block text is concatenated into the message content;
 * thinking blocks into the reasoning field; tool_use blocks into the
 * tool-call list. Usage input/output tokens are copied.
 *
 * @param[in]  json  NUL-terminated Anthropic response body.
 * @param[in]  len   Byte length of @p json.
 * @param[out] out   Receives the new response on success (caller frees).
 * @return AEGIS_OK on success, otherwise the parse/provider error.
 */
aegis_status_t aegis_anthropic_parse_complete_response(const char* json, size_t len,
                                                       aegis_model_response_t** out);

#ifdef __cplusplus
}
#endif

#endif /* AEGIS_STRUCTURED_ANTHROPIC_TEST_H */
