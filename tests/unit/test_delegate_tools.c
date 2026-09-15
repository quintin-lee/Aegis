/**
 * @file test_delegate_tools.c
 * @brief Unit test for the "task" delegation tool (mock model, no network).
 */
#define _POSIX_C_SOURCE 200809L
#include "aegis/coding/delegate_tools.h"
#include "aegis/agent/loop.h"
#include "aegis/tool/tool.h"
#include "aegis/model/model.h"
#include "aegis/common/cancellation/cancellation.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void)
{
    /* Mock model: deterministic "mock response to: <last user content>". */
    aegis_model_client_t* model = NULL;
    assert(aegis_model_client_create("mock", &model) == AEGIS_OK);

    /* Parent registry with the "task" tool. */
    aegis_tool_registry_t* reg = NULL;
    assert(aegis_tool_registry_create(&reg) == AEGIS_OK);

    subagent_ctx_t* ctx = calloc(1, sizeof(*ctx));
    assert(ctx != NULL);
    ctx->model         = model;
    ctx->parent_tools  = reg;
    ctx->system_prompt = "You are a focused sub-agent. Complete the goal.";

    aegis_tool_def_t* task = NULL;
    assert(aegis_coding_delegate_tools_register(reg, ctx, &task) == AEGIS_OK);

    /* Build args: goal = "do X". */
    aegis_tool_args_t* args = NULL;
    assert(aegis_tool_args_create(&args) == AEGIS_OK);
    assert(aegis_tool_args_add_string(args, "goal", "do X") == AEGIS_OK);

    aegis_cancellation_token_t* token = NULL;
    assert(aegis_cancellation_token_create(&token) == AEGIS_OK);

    aegis_tool_result_t result = {0};
    aegis_status_t      st     = aegis_tool_execute(reg, "task", args, token, &result);
    assert(st == AEGIS_OK);
    assert(result.value.type == AEGIS_TOOL_VAL_STRING);
    /* Child runs one reactive turn via the streaming path; the mock stream
     * emits "mock stream for: <last user content>" as its final answer. */
    assert(strcmp(result.value.as.str.ptr, "mock stream for: do X") == 0);

    aegis_tool_result_destroy(&result);
    aegis_tool_args_destroy(args);
    aegis_cancellation_token_destroy(token);
    aegis_coding_delegate_tools_free(task, ctx);
    aegis_tool_registry_destroy(reg);
    aegis_model_client_destroy(model);
    printf("All delegate tools tests PASS\n");
    return 0;
}
