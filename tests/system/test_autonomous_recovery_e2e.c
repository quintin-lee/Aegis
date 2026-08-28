/**
 * @file test_autonomous_recovery_e2e.c
 * @brief E2E crash recovery: execute, checkpoint, simulate crash, restore, resume.
 */
#define _POSIX_C_SOURCE 200809L
#include "aegis/autonomous_agent.h"
#include "aegis/checkpoint/checkpoint.h"
#include "aegis/provider/provider.h"
#include "aegis/provider/provider_llm_mock.h"
#include "aegis/tool/tool.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

static const aegis_tool_schema_t k_no_schema = {NULL, 0};

static aegis_status_t echo_tool(void* user, const aegis_tool_args_t* args,
                                const aegis_cancellation_token_t* token, aegis_tool_result_t* out)
{
    (void)user;
    (void)args;
    (void)token;
    return aegis_tool_result_set_string(out, "echo ok");
}

static void expect_ok(aegis_status_t rc, const char* msg)
{
    if (rc != AEGIS_OK) {
        fprintf(stderr, "FAIL %s: %d (%s)\n", msg, (int)rc, aegis_status_str(rc));
        abort();
    }
}

static void setup_registry(aegis_provider_registry_t** reg, llm_mock_ctx_t** ctx,
                           const aegis_llm_ops_t** ops, aegis_provider_def_t* def,
                           const char* const* responses, size_t n)
{
    expect_ok(aegis_provider_registry_create(reg), "create reg");
    expect_ok(aegis_llm_mock_create(ctx, ops, def), "create mock");
    if (responses && n > 0) {
        expect_ok(aegis_llm_mock_set_responses(*ctx, responses, n), "set responses");
    }
    expect_ok(aegis_provider_register(*reg, def), "register");
    expect_ok(aegis_provider_init(*reg, def->name), "init");
}

static void teardown_registry(aegis_provider_registry_t* reg, llm_mock_ctx_t* ctx,
                              const aegis_llm_ops_t* ops, const char* name)
{
    aegis_provider_unregister(reg, name);
    aegis_llm_mock_destroy(ctx, ops);
    aegis_provider_registry_destroy(reg);
}

static void test_crash_recovery(void)
{
    printf("[test] crash_recovery ...\n");
    char  tmpl[] = "/tmp/aegis_recovery_XXXXXX";
    char* tmpdir = mkdtemp(tmpl);
    assert(tmpdir);
    char ckpt_path[1024];
    snprintf(ckpt_path, sizeof(ckpt_path), "%s/checkpoint.bin", tmpdir);

    const char* resp   = "STEP|-1|tool||echo_step|echo hello\n";
    const char* seq[1] = {resp};

    aegis_provider_registry_t* reg = NULL;
    llm_mock_ctx_t*            ctx = NULL;
    const aegis_llm_ops_t*     ops = NULL;
    aegis_provider_def_t       def;
    setup_registry(&reg, &ctx, &ops, &def, seq, 1);

    aegis_tool_registry_t* tool_reg = NULL;
    expect_ok(aegis_tool_registry_create(&tool_reg), "tool reg");
    aegis_tool_def_t d;
    memset(&d, 0, sizeof(d));
    d.name    = "echo_step";
    d.schema  = k_no_schema;
    d.execute = echo_tool;
    expect_ok(aegis_tool_registry_register(tool_reg, &d), "register echo");

    aegis_autonomous_agent_config_t cfg = {
        .provider_registry = reg,
        .llm_provider_name = def.name,
        .checkpoint_path   = ckpt_path,
        .tool_registry     = tool_reg,
        .max_iterations    = 2,
    };
    aegis_autonomous_agent_t* aa = NULL;
    expect_ok(aegis_autonomous_agent_create(&aa, &cfg), "create aa1");
    aegis_autonomous_result_t res1 = {0};
    aegis_status_t            rc   = aegis_autonomous_agent_run(aa, "recovery goal", &res1);
    printf("  first run rc=%d tasks=%u iter=%u\n", (int)rc, res1.tasks_executed, res1.iterations);
    struct stat st;
    assert(stat(ckpt_path, &st) == 0);
    aegis_checkpoint_t*       ckpt = NULL;
    aegis_checkpoint_status_t cst;
    expect_ok(aegis_checkpoint_read(ckpt_path, &ckpt, &cst), "read ckpt");
    assert(cst == AEGIS_CHECKPOINT_OK);
    assert(aegis_checkpoint_version(ckpt) > 0);
    assert(aegis_checkpoint_task_count(ckpt) >= 1);
    printf("  checkpoint v=%u tasks=%zu state=%s goal=%s PASS\n", aegis_checkpoint_version(ckpt),
           aegis_checkpoint_task_count(ckpt), aegis_checkpoint_agent_state(ckpt),
           aegis_checkpoint_goal(ckpt));
    aegis_checkpoint_destroy(ckpt);

    aegis_autonomous_agent_destroy(aa);

    aegis_provider_registry_t* reg2 = NULL;
    llm_mock_ctx_t*            ctx2 = NULL;
    const aegis_llm_ops_t*     ops2 = NULL;
    aegis_provider_def_t       def2;
    setup_registry(&reg2, &ctx2, &ops2, &def2, seq, 1);
    aegis_tool_registry_t* tool_reg2 = NULL;
    expect_ok(aegis_tool_registry_create(&tool_reg2), "tool reg2");
    aegis_tool_def_t d2;
    memset(&d2, 0, sizeof(d2));
    d2.name    = "echo_step";
    d2.schema  = k_no_schema;
    d2.execute = echo_tool;
    expect_ok(aegis_tool_registry_register(tool_reg2, &d2), "register echo2");

    aegis_autonomous_agent_config_t cfg2 = {
        .provider_registry = reg2,
        .llm_provider_name = def2.name,
        .checkpoint_path   = ckpt_path,
        .tool_registry     = tool_reg2,
        .max_iterations    = 2,
    };
    aegis_autonomous_agent_t* aa2 = NULL;
    expect_ok(aegis_autonomous_agent_create(&aa2, &cfg2), "create aa2");
    expect_ok(aegis_autonomous_agent_restore(aa2, ckpt_path), "restore");
    aegis_autonomous_result_t res2 = {0};
    rc                             = aegis_autonomous_agent_run(aa2, "recovery goal", &res2);
    printf("  second run rc=%d tasks=%u iter=%u recovered=%d\n", (int)rc, res2.tasks_executed,
           res2.iterations, res2.recovered_from_checkpoint);
    assert(res2.recovered_from_checkpoint == true);
    printf("  recovery resumed PASS\n");

    aegis_autonomous_agent_destroy(aa2);
    aegis_tool_registry_destroy(tool_reg2);
    teardown_registry(reg2, ctx2, ops2, def2.name);
    aegis_tool_registry_destroy(tool_reg);
    teardown_registry(reg, ctx, ops, def.name);
    unlink(ckpt_path);
    rmdir(tmpdir);
    printf("[test] crash_recovery OK\n");
}

int main(void)
{
    test_crash_recovery();
    printf("all recovery e2e PASS\n");
    return 0;
}
