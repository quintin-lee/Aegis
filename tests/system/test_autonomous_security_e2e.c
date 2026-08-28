#define _POSIX_C_SOURCE 200809L
#include "aegis/autonomous_agent.h"
#include "aegis/provider/provider.h"
#include "aegis/provider/provider_llm_mock.h"
#include "aegis/tool/tool.h"
#include "aegis/security/security.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static const aegis_tool_schema_t k_no_schema = {NULL, 0};
static aegis_status_t ok_tool(void* u, const aegis_tool_args_t* a,
                              const aegis_cancellation_token_t* t, aegis_tool_result_t* o)
{
    (void)u;
    (void)a;
    (void)t;
    return aegis_tool_result_set_string(o, "ok");
}
static void expect_ok(aegis_status_t rc, const char* m)
{
    if (rc != AEGIS_OK) {
        fprintf(stderr, "FAIL %s %d\n", m, (int)rc);
        abort();
    }
}
static void setup(aegis_provider_registry_t** r, llm_mock_ctx_t** c, const aegis_llm_ops_t** o,
                  aegis_provider_def_t* d, const char* const* s, size_t n)
{
    expect_ok(aegis_provider_registry_create(r), "reg");
    expect_ok(aegis_llm_mock_create(c, o, d), "mock");
    if (s && n) {
        expect_ok(aegis_llm_mock_set_responses(*c, s, n), "resp");
    }
    expect_ok(aegis_provider_register(*r, d), "reg");
    expect_ok(aegis_provider_init(*r, d->name), "init");
}
static void teardown(aegis_provider_registry_t* r, llm_mock_ctx_t* c, const aegis_llm_ops_t* o,
                     const char* n)
{
    aegis_provider_unregister(r, n);
    aegis_llm_mock_destroy(c, o);
    aegis_provider_registry_destroy(r);
}
static void test_security_deny(void)
{
    printf("[security] deny ...\n");
    const char*                resp   = "STEP|-1|tool||sec_step|do\n";
    const char*                seq[1] = {resp};
    aegis_provider_registry_t* reg    = NULL;
    llm_mock_ctx_t*            ctx    = NULL;
    const aegis_llm_ops_t*     ops    = NULL;
    aegis_provider_def_t       def;
    setup(&reg, &ctx, &ops, &def, seq, 1);
    aegis_tool_registry_t* tr = NULL;
    expect_ok(aegis_tool_registry_create(&tr), "tr");
    aegis_tool_def_t d = {0};
    d.name             = "sec_step";
    d.schema           = k_no_schema;
    d.execute          = ok_tool;
    d.capabilities     = AEGIS_CAP_SHELL;
    expect_ok(aegis_tool_registry_register(tr, &d), "reg");
    aegis_security_policy_t* pol = NULL;
    expect_ok(aegis_security_policy_create(&pol), "pol");
    // no rule for SHELL, so default deny
    aegis_autonomous_agent_config_t cfg = {.provider_registry = reg,
                                           .llm_provider_name = def.name,
                                           .tool_registry     = tr,
                                           .security_policy   = pol,
                                           .max_iterations    = 2};
    aegis_autonomous_agent_t*       aa  = NULL;
    expect_ok(aegis_autonomous_agent_create(&aa, &cfg), "create");
    aegis_autonomous_result_t res = {0};
    aegis_status_t            rc  = aegis_autonomous_agent_run(aa, "sec goal", &res);
    printf("  rc=%d perm=%d\n", (int)rc, (int)AEGIS_ERR_PERM);
    assert(rc == AEGIS_ERR_PERM || res.final_status == AEGIS_ERR_PERM);
    // ensure tool was not executed (would have succeeded if bypass)
    aegis_autonomous_agent_destroy(aa);
    aegis_security_policy_destroy(pol);
    aegis_tool_registry_destroy(tr);
    teardown(reg, ctx, ops, def.name);
    printf("[security] deny PASS\n");
}
static void test_security_allow(void)
{
    printf("[security] allow ...\n");
    const char*                resp   = "STEP|-1|tool||sec_step|do\n";
    const char*                seq[1] = {resp};
    aegis_provider_registry_t* reg    = NULL;
    llm_mock_ctx_t*            ctx    = NULL;
    const aegis_llm_ops_t*     ops    = NULL;
    aegis_provider_def_t       def;
    setup(&reg, &ctx, &ops, &def, seq, 1);
    aegis_tool_registry_t* tr = NULL;
    expect_ok(aegis_tool_registry_create(&tr), "tr");
    aegis_tool_def_t d = {0};
    d.name             = "sec_step";
    d.schema           = k_no_schema;
    d.execute          = ok_tool;
    d.capabilities     = AEGIS_CAP_SHELL;
    expect_ok(aegis_tool_registry_register(tr, &d), "reg");
    aegis_security_policy_t* pol = NULL;
    expect_ok(aegis_security_policy_create(&pol), "pol");
    expect_ok(aegis_security_policy_add_rule(pol, "*",
                                             AEGIS_CAP_SHELL | AEGIS_CAP_READ_FILE |
                                                 AEGIS_CAP_WRITE_FILE | AEGIS_CAP_NETWORK |
                                                 AEGIS_CAP_RUN_PROCESS | AEGIS_CAP_ACCESS_CRED),
              "add rule");
    aegis_autonomous_agent_config_t cfg = {.provider_registry = reg,
                                           .llm_provider_name = def.name,
                                           .tool_registry     = tr,
                                           .security_policy   = pol,
                                           .max_iterations    = 2};
    aegis_autonomous_agent_t*       aa  = NULL;
    expect_ok(aegis_autonomous_agent_create(&aa, &cfg), "create");
    aegis_autonomous_result_t res = {0};
    aegis_status_t            rc  = aegis_autonomous_agent_run(aa, "sec goal", &res);
    printf("  rc=%d tasks=%u\n", (int)rc, res.tasks_executed);
    assert(res.tasks_executed >= 1);
    aegis_autonomous_agent_destroy(aa);
    aegis_security_policy_destroy(pol);
    aegis_tool_registry_destroy(tr);
    teardown(reg, ctx, ops, def.name);
    printf("[security] allow PASS\n");
}
int main(void)
{
    test_security_deny();
    test_security_allow();
    printf("security e2e PASS\n");
    return 0;
}
