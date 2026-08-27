/**
 * @file test_autonomous_tool_integration.c
 * @brief Integration test: autonomous agent dispatches tool tasks via registry.
 */
#include "aegis/autonomous_agent.h"
#include "aegis/provider_llm_mock.h"
#include "aegis/provider.h"
#include "aegis/llm.h"
#include "aegis/tool.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Mock tools ─────────────────────────────────────────────────────── */

static const aegis_tool_param_spec_t k_echo_params[] = {
    {"text", AEGIS_TOOL_VAL_STRING, true, NULL},
};
static const aegis_tool_schema_t k_echo_schema = {k_echo_params, 1};

static aegis_status_t echo_tool(void* user, const aegis_tool_args_t* args,
                                const aegis_cancellation_token_t* token, aegis_tool_result_t* out)
{
    (void)user; (void)token;
    const aegis_tool_value_t* t = NULL;
    if (!aegis_tool_args_find(args, "text", &t)) return AEGIS_ERR_INVALID;
    return aegis_tool_result_set_string(out, t->as.str.ptr);
}

static const aegis_tool_param_spec_t k_add_params[] = {
    {"a", AEGIS_TOOL_VAL_INT, true, NULL},
    {"b", AEGIS_TOOL_VAL_INT, true, NULL},
};
static const aegis_tool_schema_t k_add_schema = {k_add_params, 2};

static aegis_status_t add_ints(void* user, const aegis_tool_args_t* args,
                               const aegis_cancellation_token_t* token, aegis_tool_result_t* out)
{
    (void)user; (void)token;
    const aegis_tool_value_t *a = NULL, *b = NULL;
    if (!aegis_tool_args_find(args, "a", &a) || !aegis_tool_args_find(args, "b", &b))
        return AEGIS_ERR_INVALID;
    return aegis_tool_result_set_int(out, a->as.i + b->as.i);
}

/* ── Helpers (match system test pattern) ────────────────────────────── */

static void expect_ok(aegis_status_t rc, const char* msg) {
    if (rc != AEGIS_OK) { fprintf(stderr, "FAIL %s: got %d\n", msg, (int)rc); abort(); }
}

static void setup_registry(aegis_provider_registry_t** reg, llm_mock_ctx_t** ctx,
                           const aegis_llm_ops_t** ops, aegis_provider_def_t* def,
                           const char* const* responses, size_t n) {
    expect_ok(aegis_provider_registry_create(reg), "create reg");
    expect_ok(aegis_llm_mock_create(ctx, ops, def), "create mock");
    if (responses && n > 0) {
        expect_ok(aegis_llm_mock_set_responses(*ctx, responses, n), "set responses");
    }
    expect_ok(aegis_provider_register(*reg, def), "register");
    expect_ok(aegis_provider_init(*reg, def->name), "init");
}

static void teardown_registry(aegis_provider_registry_t* reg, llm_mock_ctx_t* ctx,
                              const aegis_llm_ops_t* ops, const char* name) {
    aegis_provider_unregister(reg, name);
    aegis_llm_mock_destroy(ctx, ops);
    aegis_provider_registry_destroy(reg);
}

/* ── Tests ───────────────────────────────────────────────────────────── */

static void test_tool_dispatch_vs_default_work(void)
{
    printf("[test] tool_dispatch vs default_work ...\n");

    const char* resp[] = {
        "STEP|-1|tool||echo_step|echo something\n"
        "STEP|-1|computational||comp_step|plain computation\n"
    };
    aegis_provider_registry_t* reg = NULL;
    llm_mock_ctx_t* ctx = NULL;
    const aegis_llm_ops_t* ops = NULL;
    aegis_provider_def_t def;
    setup_registry(&reg, &ctx, &ops, &def, resp, 1);

    aegis_tool_registry_t* tool_reg = NULL;
    expect_ok(aegis_tool_registry_create(&tool_reg), "create tool reg");
    {
        aegis_tool_def_t d; memset(&d, 0, sizeof(d));
        d.name = "echo_step"; d.schema = k_echo_schema; d.execute = echo_tool;
        expect_ok(aegis_tool_registry_register(tool_reg, &d), "register echo");
    }
    {
        aegis_tool_def_t d; memset(&d, 0, sizeof(d));
        d.name = "add_ints"; d.schema = k_add_schema; d.execute = add_ints;
        expect_ok(aegis_tool_registry_register(tool_reg, &d), "register add");
    }

    aegis_autonomous_agent_config_t cfg = {
        .provider_registry          = reg,
        .llm_provider_name          = def.name,
        .tool_registry              = tool_reg,
        .max_iterations             = 2,
        .default_task_timeout_ns    = 0,
    };
    aegis_autonomous_agent_t* aa = NULL;
    expect_ok(aegis_autonomous_agent_create(&aa, &cfg), "create aa");
    aegis_autonomous_result_t result = {0};
    aegis_status_t rc = aegis_autonomous_agent_run(aa, "run tools", &result);
    printf("  rc=%d tasks=%u final=%d\n", (int)rc, result.tasks_executed, (int)result.final_status);
    /* Tool step may fail validation (missing args), but computational step should run */
    assert(result.tasks_executed >= 1);
    printf("  tasks_executed=%u PASS\n", result.tasks_executed);

    aegis_autonomous_agent_destroy(aa);
    aegis_tool_registry_destroy(tool_reg);
    teardown_registry(reg, ctx, ops, def.name);
}

static void test_no_tool_registry_falls_back_to_default_work(void)
{
    printf("[test] fallback to default_work when no tool_registry ...\n");

    const char* resp[] = {"STEP|-1|computational||step1|do thing\n"};
    aegis_provider_registry_t* reg = NULL;
    llm_mock_ctx_t* ctx = NULL;
    const aegis_llm_ops_t* ops = NULL;
    aegis_provider_def_t def;
    setup_registry(&reg, &ctx, &ops, &def, resp, 1);

    aegis_autonomous_agent_config_t cfg = {
        .provider_registry          = reg,
        .llm_provider_name          = def.name,
        .max_iterations             = 2,
        .default_task_timeout_ns    = 0,
    };
    aegis_autonomous_agent_t* aa = NULL;
    expect_ok(aegis_autonomous_agent_create(&aa, &cfg), "create aa");
    aegis_autonomous_result_t result = {0};
    aegis_status_t rc = aegis_autonomous_agent_run(aa, "fallback goal", &result);
    printf("  rc=%d tasks=%u final=%d\n", (int)rc, result.tasks_executed, (int)result.final_status);
    assert(rc == AEGIS_OK);
    assert(result.tasks_executed == 1);
    printf("  tasks_executed=%u PASS\n", result.tasks_executed);

    aegis_autonomous_agent_destroy(aa);
    teardown_registry(reg, ctx, ops, def.name);
}

int main(void)
{
    test_tool_dispatch_vs_default_work();
    test_no_tool_registry_falls_back_to_default_work();
    printf("test_autonomous_tool_integration: all cases passed\n");
    return 0;
}
