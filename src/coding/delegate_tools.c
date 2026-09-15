/**
 * @file delegate_tools.c
 * @brief The "task" tool: delegate a sub-goal to a synchronous child agent.
 */
#define _POSIX_C_SOURCE 200809L
#include "aegis/coding/delegate_tools.h"
#include "aegis/agent/loop.h"
#include "aegis/common/cancellation/cancellation.h"
#include "aegis/common/error.h"
#include "aegis/session/session.h"
#include <string.h>
#include <stdlib.h>

#define TASK_TOOL_NAME "task"
#define TASK_MAX_TURNS 10

/* Forward decl of the execute fn (static, wired into the def). */
static aegis_status_t task_tool_execute(void* user, const aegis_tool_args_t* args,
                                        const aegis_cancellation_token_t* token,
                                        aegis_tool_result_t* out);

/* Visitor: re-register every parent tool EXCEPT "task" into the child registry. */
struct copy_ctx {
    aegis_tool_registry_t* child;
};

static aegis_status_t copy_tool_visitor(const aegis_tool_def_t* def, void* user)
{
    struct copy_ctx* cc = user;
    if (def && strcmp(def->name, TASK_TOOL_NAME) != 0) {
        return aegis_tool_registry_register(cc->child, def);
    }
    return AEGIS_OK; /* skip "task" (recursion guard) */
}

aegis_status_t aegis_coding_delegate_tools_register(aegis_tool_registry_t* reg,
                                                    subagent_ctx_t*        ctx,
                                                    aegis_tool_def_t**    out_def)
{
    if (!reg || !out_def) {
        return AEGIS_ERR_INVALID;
    }
    *out_def = NULL;

    /* Owned name/description/params arrays (registry is shallow). */
    char* name = strdup(TASK_TOOL_NAME);
    char* desc = strdup("Delegate a focused sub-goal to a child agent. "
                        "The child has the same tools (except task) "
                        "and an isolated context. Returns its final answer.");
    if (!name || !desc) {
        free(name);
        free(desc);
        return AEGIS_ERR_NOMEM;
    }
    aegis_tool_param_spec_t* params = calloc(2, sizeof(*params));
    if (!params) {
        free(name);
        free(desc);
        return AEGIS_ERR_NOMEM;
    }
    char* goal_name = strdup("goal");
    char* goal_desc = strdup("The specific sub-goal for the child to complete.");
    char* desc_name = strdup("description");
    char* desc_desc = strdup("Optional human context for the sub-goal.");
    if (!goal_name || !goal_desc || !desc_name || !desc_desc) {
        free(goal_name);
        free(goal_desc);
        free(desc_name);
        free(desc_desc);
        free(params);
        free(name);
        free(desc);
        return AEGIS_ERR_NOMEM;
    }
    params[0] = (aegis_tool_param_spec_t){
        .name        = goal_name,
        .type        = AEGIS_TOOL_VAL_STRING,
        .required    = true,
        .description = goal_desc,
    };
    params[1] = (aegis_tool_param_spec_t){
        .name        = desc_name,
        .type        = AEGIS_TOOL_VAL_STRING,
        .required    = false,
        .description = desc_desc,
    };

    aegis_tool_def_t* def = calloc(1, sizeof(*def));
    if (!def) {
        free(goal_name);
        free(goal_desc);
        free(desc_name);
        free(desc_desc);
        free(params);
        free(name);
        free(desc);
        return AEGIS_ERR_NOMEM;
    }
    def->name            = name;
    def->description     = desc;
    def->schema.params   = params;
    def->schema.param_count = 2;
    def->execute         = task_tool_execute;
    def->user            = ctx;

    aegis_status_t st = aegis_tool_registry_register(reg, def);
    if (st != AEGIS_OK) {
        aegis_coding_delegate_tools_free(def, NULL);
        return st;
    }
    *out_def = def;
    return AEGIS_OK;
}

void aegis_coding_delegate_tools_free(aegis_tool_def_t* def, subagent_ctx_t* ctx)
{
    if (def) {
        /* param/name/description fields are const in the ABI, but we own them
         * here (heap-allocated in register); cast to free. */
        if (def->schema.params) {
            for (size_t i = 0; i < def->schema.param_count; ++i) {
                free((void*)def->schema.params[i].name);
                free((void*)def->schema.params[i].description);
            }
            free((void*)def->schema.params);
        }
        free((void*)def->name);
        free((void*)def->description);
        free(def);
    }
    free(ctx);
}

static aegis_status_t task_tool_execute(void* user, const aegis_tool_args_t* args,
                                        const aegis_cancellation_token_t* token,
                                        aegis_tool_result_t* out)
{
    (void)token;
    subagent_ctx_t* ctx = user;
    if (!ctx || !args || !out) {
        return AEGIS_ERR_INVALID;
    }
    /* Read "goal" (required). */
    const aegis_tool_value_t* gv = NULL;
    if (!aegis_tool_args_find(args, "goal", &gv) || !gv ||
        gv->type != AEGIS_TOOL_VAL_STRING) {
        return AEGIS_ERR_INVALID;
    }
    const char* goal = gv->as.str.ptr;

    /* (2) Fresh isolated child session. */
    aegis_session_t* child_session = NULL;
    aegis_status_t   st = aegis_session_create("/tmp", &child_session);
    if (st != AEGIS_OK) {
        return st;
    }

    /* (3) Child tool registry = parent tools minus "task". */
    aegis_tool_registry_t* child_tools = NULL;
    st = aegis_tool_registry_create(&child_tools);
    if (st != AEGIS_OK) {
        aegis_session_destroy(child_session);
        return st;
    }
    if (ctx->parent_tools) {
        struct copy_ctx cc = {.child = child_tools};
        st = aegis_tool_registry_visit(ctx->parent_tools, copy_tool_visitor, &cc);
        if (st != AEGIS_OK) {
            aegis_tool_registry_destroy(child_tools);
            aegis_session_destroy(child_session);
            return st;
        }
    }

    /* (4) Child loop with a fresh token, reactive strategy, capped turns. */
    aegis_cancellation_token_t* child_token = NULL;
    st = aegis_cancellation_token_create(&child_token);
    if (st != AEGIS_OK) {
        aegis_tool_registry_destroy(child_tools);
        aegis_session_destroy(child_session);
        return st;
    }
    aegis_agent_loop_config_t ccfg;
    memset(&ccfg, 0, sizeof(ccfg));
    ccfg.session            = child_session;
    ccfg.model              = ctx->model;
    ccfg.tools              = child_tools;
    ccfg.system_prompt      = ctx->system_prompt;
    ccfg.token              = child_token;
    ccfg.strategy           = NULL; /* reactive */
    ccfg.max_strategy_turns = TASK_MAX_TURNS;

    aegis_agent_loop_t* child_loop = NULL;
    st = aegis_agent_loop_create(&ccfg, &child_loop);
    if (st != AEGIS_OK) {
        aegis_cancellation_token_destroy(child_token);
        aegis_tool_registry_destroy(child_tools);
        aegis_session_destroy(child_session);
        return st;
    }

    /* (5) Run the child synchronously. */
    aegis_status_t run_st = aegis_agent_loop_run_turn(child_loop, goal);

    /* (6) Read the child's last message content as the answer. */
    size_t n = aegis_session_message_count(child_session);
    const char* answer = "";
    if (n > 0) {
        const aegis_message_t* last = aegis_session_message_at(child_session, n - 1);
        if (last) {
            answer = aegis_message_content(last) ? aegis_message_content(last) : "";
        }
    }
    st = aegis_tool_result_set_string(out, answer);

    /* (7) Tear down the child. */
    aegis_agent_loop_destroy(child_loop);
    aegis_session_destroy(child_session);
    aegis_tool_registry_destroy(child_tools);
    aegis_cancellation_token_destroy(child_token);

    /* (8) Propagate the result-set failure, else the child run status. */
    if (st != AEGIS_OK) {
        return st;
    }
    return run_st;
}
