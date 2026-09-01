#define _POSIX_C_SOURCE 200809L
#include "structured_openai.h"
#include "structured_openai_test.h"
#include "aegis/model/model.h"
#include "aegis/message/message.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    aegis_openai_model_ctx_t* ctx = NULL;
    aegis_model_backend_t backend = {0};
    assert(aegis_openai_model_create("test-key", "http://127.0.0.1:1", "test-model",
                                     &ctx, &backend) == AEGIS_OK);
    assert(ctx != NULL);
    assert(backend.complete != NULL);
    assert(backend.stream != NULL);

    aegis_message_list_t* messages = NULL;
    assert(aegis_message_list_create(&messages) == AEGIS_OK);
    aegis_message_t* user = NULL;
    assert(aegis_message_create(AEGIS_MESSAGE_USER, &user) == AEGIS_OK);
    assert(aegis_message_set_content(user, "hello") == AEGIS_OK);
    assert(aegis_message_list_append(messages, user) == AEGIS_OK);

    aegis_model_request_t request = {
        .model = "test-model",
        .messages = messages,
        .stream = false,
    };
    const char* complete_json = "{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":\"hello\"}}],\"usage\":{\"prompt_tokens\":2,\"completion_tokens\":3,\"total_tokens\":5}}";
    aegis_model_response_t* parsed = NULL;
    assert(aegis_openai_parse_complete_response(complete_json, strlen(complete_json), &parsed) == AEGIS_OK);
    assert(parsed != NULL);
    assert(parsed->message != NULL);
    assert(strcmp(aegis_message_content(parsed->message), "hello") == 0);
    assert(parsed->usage.input_tokens == 2);
    assert(parsed->usage.output_tokens == 3);
    assert(parsed->usage.total_tokens == 5);
    aegis_model_response_destroy(parsed);

    const char* malformed_json = "{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":\"unterminated}}]";
    parsed = NULL;
    assert(aegis_openai_parse_complete_response(malformed_json, strlen(malformed_json), &parsed) == AEGIS_ERR_PROVIDER);
    assert(parsed == NULL);

    const char* missing_content = "{\"choices\":[{\"message\":{\"role\":\"assistant\"}}]}";
    assert(aegis_openai_parse_complete_response(missing_content, strlen(missing_content), &parsed) == AEGIS_ERR_PROVIDER);
    assert(parsed == NULL);

    aegis_model_response_t* response = NULL;
    aegis_cancellation_token_t* token = NULL;
    assert(aegis_cancellation_token_create(&token) == AEGIS_OK);
    aegis_cancellation_token_request_cancel(token);
    aegis_status_t status = backend.complete(backend.user, &request, token, &response);
    assert(status == AEGIS_ERR_CANCELLED);
    assert(response == NULL);
    aegis_cancellation_token_destroy(token);

    aegis_message_destroy(user);
    aegis_message_list_destroy(messages);
    aegis_openai_model_destroy(ctx);
    puts("structured_openai: PASS");
    return 0;
}
