#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include "aegis/autonomous_agent.h"
#include "aegis/provider/provider.h"
#include "aegis/provider/provider_llm_mock.h"
#include "aegis/tool/tool.h"
#include "aegis/common/cancellation/cancellation.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>

static const aegis_tool_schema_t k_no_schema = {NULL, 0};
static aegis_status_t slow_tool(void* u, const aegis_tool_args_t* a,
                                const aegis_cancellation_token_t* t, aegis_tool_result_t* o)
{
    (void)u;
    (void)a;
    (void)o;
    for (int i = 0; i < 5; i++) {
        if (aegis_cancellation_token_is_cancelled(t)) {
            return AEGIS_ERR_CANCELLED;
        }
        struct timespec ts = {0, 10000000};
        nanosleep(&ts, NULL);
    }
    return aegis_tool_result_set_string(o, "slow");
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
struct cancel_arg {
    aegis_autonomous_agent_t* aa;
};
static void* canceller(void* arg)
{
    struct timespec ts = {0, 50000000};
    nanosleep(&ts, NULL);
    aegis_autonomous_agent_cancel(((struct cancel_arg*)arg)->aa);
    return NULL;
}
static void test_cancellation(void)
{
    printf("[cancel] ...\n");
    const char*                resp   = "STEP|-1|tool||slow_step|slow\n";
    const char*                seq[1] = {resp};
    aegis_provider_registry_t* reg    = NULL;
    llm_mock_ctx_t*            ctx    = NULL;
    const aegis_llm_ops_t*     ops    = NULL;
    aegis_provider_def_t       def;
    setup(&reg, &ctx, &ops, &def, seq, 1);
    aegis_tool_registry_t* tr = NULL;
    expect_ok(aegis_tool_registry_create(&tr), "tr");
    aegis_tool_def_t d = {0};
    d.name             = "slow_step";
    d.schema           = k_no_schema;
    d.execute          = slow_tool;
    expect_ok(aegis_tool_registry_register(tr, &d), "reg");
    aegis_autonomous_agent_config_t cfg = {.provider_registry = reg,
                                           .llm_provider_name = def.name,
                                           .tool_registry     = tr,
                                           .max_iterations    = 3};
    aegis_autonomous_agent_t*       aa  = NULL;
    expect_ok(aegis_autonomous_agent_create(&aa, &cfg), "create");
    struct cancel_arg ca = {aa};
    pthread_t         th;
    pthread_create(&th, NULL, canceller, &ca);
    aegis_autonomous_result_t res = {0};
    aegis_status_t            rc  = aegis_autonomous_agent_run(aa, "cancel goal", &res);
    pthread_join(th, NULL);
    printf("  rc=%d cancelled=%d\n", (int)rc, (int)AEGIS_ERR_CANCELLED);
    assert(rc == AEGIS_ERR_CANCELLED || res.final_status == AEGIS_ERR_CANCELLED);
    aegis_autonomous_agent_destroy(aa);
    aegis_tool_registry_destroy(tr);
    teardown(reg, ctx, ops, def.name);
    printf("[cancel] PASS\n");
}
int main(void)
{
    test_cancellation();
    printf("cancellation e2e PASS\n");
    return 0;
}
