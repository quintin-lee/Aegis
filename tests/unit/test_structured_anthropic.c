/**
 * @file test_structured_anthropic.c
 * @brief Unit tests for the Anthropic provider: context factory, capability
 * flags, complete-response parsing seam, and pre-cancelled token handling.
 */
#define _POSIX_C_SOURCE 200809L
#include "structured_anthropic.h"
#include "structured_anthropic_test.h"
#include "aegis/common/cancellation/cancellation.h"
#include "aegis/common/error.h"
#include "aegis/common/result.h"
#include "aegis/message/message.h"
#include "aegis/message/role.h"
#include "aegis/model/model.h"
#include "aegis/model/request.h"
#include "aegis/model/response.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void)
{
    aegis_anthropic_model_ctx_t* ctx     = NULL;
    aegis_model_backend_t        backend = {0};
    aegis_status_t st = aegis_anthropic_model_create("test-key", "http://127.0.0.1:1", "test-model",
                                                     &ctx, &backend);
    assert(st == AEGIS_OK && ctx && backend.user == ctx);
    assert((backend.capabilities & AEGIS_MODEL_CAP_REASONING) != 0);
    assert(backend.complete && backend.stream);

    /* Valid complete response: text block + usage. */
    const char* json =
        "{\"content\":[{\"type\":\"text\",\"text\":\"hello\"}],\"usage\":{\"input_tokens\":2,"
        "\"output_tokens\":3}}";
    aegis_model_response_t* parsed = NULL;
    st = aegis_anthropic_parse_complete_response(json, strlen(json), &parsed);
    assert(st == AEGIS_OK && parsed && parsed->message);
    assert(parsed->usage.input_tokens == 2);
    assert(parsed->usage.output_tokens == 3);
    assert(parsed->usage.total_tokens == 5);
    assert(strcmp(aegis_message_content(parsed->message), "hello") == 0);
    aegis_model_response_destroy(parsed);

    /* Body with no content blocks: parser yields an empty assistant message. */
    parsed = NULL;
    st     = aegis_anthropic_parse_complete_response("{\"foo\":1}", strlen("{\"foo\":1}"), &parsed);
    assert(st == AEGIS_OK && parsed && parsed->message);
    assert(aegis_message_content(parsed->message) == NULL);
    aegis_model_response_destroy(parsed);

    /* Empty / NULL inputs. */
    st = aegis_anthropic_parse_complete_response(NULL, 0, &parsed);
    assert(st == AEGIS_ERR_INVALID);
    st = aegis_anthropic_parse_complete_response(json, strlen(json), NULL);
    assert(st == AEGIS_ERR_INVALID);

    /* Pre-cancelled token is detected by complete(). */
    aegis_cancellation_token_t* token = NULL;
    assert(aegis_cancellation_token_create(&token) == AEGIS_OK);
    aegis_cancellation_token_request_cancel(token);
    aegis_message_list_t* msgs = NULL;
    aegis_message_t*      user = NULL;
    assert(aegis_message_list_create(&msgs) == AEGIS_OK);
    assert(aegis_message_create(AEGIS_MESSAGE_USER, &user) == AEGIS_OK);
    assert(aegis_message_set_content(user, "hi") == AEGIS_OK);
    assert(aegis_message_list_append(msgs, user) == AEGIS_OK);
    aegis_model_request_t req = {
        .model    = "test-model",
        .messages = msgs,
        .stream   = false,
    };
    aegis_model_response_t* out = NULL;
    st                          = backend.complete(backend.user, &req, token, &out);
    assert(st == AEGIS_ERR_CANCELLED && out == NULL);

    aegis_message_list_destroy(msgs);
    aegis_cancellation_token_destroy(token);
    aegis_anthropic_model_destroy(ctx);
    puts("PASS");
    return 0;
}
