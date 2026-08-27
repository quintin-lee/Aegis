#include "aegis/autonomous_agent.h"
#include "aegis/provider_llm_mock.h"
#include "aegis/provider.h"
#include "aegis/llm.h"

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void expect_ok(aegis_status_t rc, const char* msg) {
    if (rc != AEGIS_OK) {
        fprintf(stderr, "FAIL %s: got %d expected OK\n", msg, (int)rc);
        abort();
    }
}

static void setup_registry(aegis_provider_registry_t** reg, llm_mock_ctx_t** ctx,
                           const aegis_llm_ops_t** ops, aegis_provider_def_t* def,
                           const char* const* responses, size_t n) {
    assert(aegis_provider_registry_create(reg) == AEGIS_OK);
    assert(aegis_llm_mock_create(ctx, ops, def) == AEGIS_OK);
    if (responses && n > 0) {
        assert(aegis_llm_mock_set_responses(*ctx, responses, n) == AEGIS_OK);
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

static void test_happy_path(void) {
    printf("[test] happy_path ...\n");
    const char* resp = "STEP|-1|computational||step1|do step1\n"
                       "STEP|-1|computational||step2|do step2\n"
                       "STEP|-1|computational||step3|do step3\n";
    const char* seq[1] = {resp};
    aegis_provider_registry_t* reg = NULL;
    llm_mock_ctx_t* ctx = NULL;
    const aegis_llm_ops_t* ops = NULL;
    aegis_provider_def_t def;
    setup_registry(&reg, &ctx, &ops, &def, seq, 1);

    aegis_autonomous_agent_config_t cfg = {
        .provider_registry = reg,
        .llm_provider_name = def.name,
        .checkpoint_path = NULL,
        .cancel_token = NULL,
        .max_iterations = 3,
        .default_task_timeout_ns = 0,
    };
    aegis_autonomous_agent_t* aa = NULL;
    expect_ok(aegis_autonomous_agent_create(&aa, &cfg), "create aa");
    aegis_autonomous_result_t res = {0};
    aegis_status_t rc = aegis_autonomous_agent_run(aa, "happy goal", &res);
    assert(rc == AEGIS_OK);
    assert(res.final_status == AEGIS_OK);
    assert(res.tasks_executed == 3);
    assert(res.iterations == 1 || res.iterations == 2);
    printf("  tasks_executed=%u iterations=%u PASS\n", res.tasks_executed, res.iterations);
    aegis_autonomous_agent_destroy(aa);
    teardown_registry(reg, ctx, ops, def.name);
}

static void test_retry_then_replan(void) {
    printf("[test] retry_then_replan ...\n");
    const char* first =
        "STEP|-1|computational||step_ok|ok step\n"
        "STEP|-1|computational||fail_step|will fail\n"
        "STEP|-1|computational||step3|third\n";
    const char* second =
        "STEP|-1|computational||step_a|a\n"
        "STEP|-1|computational||step_b|b\n";
    const char* seq[2] = {first, second};
    aegis_provider_registry_t* reg = NULL;
    llm_mock_ctx_t* ctx = NULL;
    const aegis_llm_ops_t* ops = NULL;
    aegis_provider_def_t def;
    setup_registry(&reg, &ctx, &ops, &def, seq, 2);

    aegis_autonomous_agent_config_t cfg = {
        .provider_registry = reg,
        .llm_provider_name = def.name,
        .max_iterations = 5,
    };
    aegis_autonomous_agent_t* aa = NULL;
    expect_ok(aegis_autonomous_agent_create(&aa, &cfg), "create");
    aegis_autonomous_result_t res = {0};
    aegis_status_t rc = aegis_autonomous_agent_run(aa, "replan goal", &res);
    assert(rc == AEGIS_OK);
    assert(res.iterations >= 2);
    assert(res.tasks_executed >= 3);
    printf("  iterations=%u tasks=%u PASS\n", res.iterations, res.tasks_executed);
    aegis_autonomous_agent_destroy(aa);
    teardown_registry(reg, ctx, ops, def.name);
}

static void test_timeout(void) {
    printf("[test] timeout ...\n");
    const char* resp =
        "STEP|-1|computational||slow_task|slow will sleep\n"
        "STEP|-1|computational||step2|normal\n";
    const char* seq[1] = {resp};
    aegis_provider_registry_t* reg = NULL;
    llm_mock_ctx_t* ctx = NULL;
    const aegis_llm_ops_t* ops = NULL;
    aegis_provider_def_t def;
    setup_registry(&reg, &ctx, &ops, &def, seq, 1);
    aegis_autonomous_agent_config_t cfg = {
        .provider_registry = reg,
        .llm_provider_name = def.name,
        .max_iterations = 3,
        .default_task_timeout_ns = 30 * 1000 * 1000ULL,
    };
    aegis_autonomous_agent_t* aa = NULL;
    expect_ok(aegis_autonomous_agent_create(&aa, &cfg), "create");
    aegis_autonomous_result_t res = {0};
    aegis_status_t rc = aegis_autonomous_agent_run(aa, "timeout goal", &res);
    assert(rc == AEGIS_ERR_TIMEOUT);
    assert(res.final_status == AEGIS_ERR_TIMEOUT);
    printf("  timeout rc=%d PASS\n", (int)rc);
    aegis_autonomous_agent_destroy(aa);
    teardown_registry(reg, ctx, ops, def.name);
}

struct cancel_arg {
    aegis_autonomous_agent_t* aa;
};

static void* canceller(void* arg) {
    struct cancel_arg* ca = (struct cancel_arg*)arg;
    usleep(80 * 1000);
    aegis_autonomous_agent_cancel(ca->aa);
    return NULL;
}

static void test_cancellation(void) {
    printf("[test] cancellation ...\n");
    const char* resp =
        "STEP|-1|computational||slow_task|slow1\n"
        "STEP|-1|computational||slow_task2|slow2\n"
        "STEP|-1|computational||step3|third\n";
    const char* seq[1] = {resp};
    aegis_provider_registry_t* reg = NULL;
    llm_mock_ctx_t* ctx = NULL;
    const aegis_llm_ops_t* ops = NULL;
    aegis_provider_def_t def;
    setup_registry(&reg, &ctx, &ops, &def, seq, 1);
    aegis_autonomous_agent_config_t cfg = {
        .provider_registry = reg,
        .llm_provider_name = def.name,
        .max_iterations = 5,
    };
    aegis_autonomous_agent_t* aa = NULL;
    expect_ok(aegis_autonomous_agent_create(&aa, &cfg), "create");
    struct cancel_arg ca = {.aa = aa};
    pthread_t th;
    pthread_create(&th, NULL, canceller, &ca);
    aegis_autonomous_result_t res = {0};
    aegis_status_t rc = aegis_autonomous_agent_run(aa, "cancel goal", &res);
    pthread_join(th, NULL);
    assert(rc == AEGIS_ERR_CANCELLED);
    printf("  cancel rc=%d PASS\n", (int)rc);
    aegis_autonomous_agent_destroy(aa);
    teardown_registry(reg, ctx, ops, def.name);
}

static void test_checkpoint_recovery(void) {
    printf("[test] checkpoint_recovery ...\n");
    const char* resp =
        "STEP|-1|computational||step1|first\n"
        "STEP|-1|computational||step2|second\n"
        "STEP|-1|computational||step3|third\n";
    const char* seq[1] = {resp};
    const char* path = "/tmp/aegis_autonomous_chk_test.bin";
    unlink(path);
    aegis_provider_registry_t* reg = NULL;
    llm_mock_ctx_t* ctx = NULL;
    const aegis_llm_ops_t* ops = NULL;
    aegis_provider_def_t def;
    setup_registry(&reg, &ctx, &ops, &def, seq, 1);
    aegis_autonomous_agent_config_t cfg = {
        .provider_registry = reg,
        .llm_provider_name = def.name,
        .checkpoint_path = path,
        .max_iterations = 3,
    };
    aegis_autonomous_agent_t* aa = NULL;
    expect_ok(aegis_autonomous_agent_create(&aa, &cfg), "create");
    aegis_autonomous_result_t res = {0};
    aegis_status_t rc = aegis_autonomous_agent_run(aa, "checkpoint goal", &res);
    assert(rc == AEGIS_OK);
    assert(access(path, F_OK) == 0);
    printf("  checkpoint written PASS\n");
    expect_ok(aegis_autonomous_agent_checkpoint_save(aa, NULL), "save");
    expect_ok(aegis_autonomous_agent_restore(aa, path), "restore");
    assert(res.recovered_from_checkpoint == false);
    aegis_autonomous_agent_destroy(aa);

    const char* seq2[1] = {resp};
    llm_mock_ctx_t* ctx2 = NULL;
    const aegis_llm_ops_t* ops2 = NULL;
    aegis_provider_def_t def2;
    aegis_provider_registry_t* reg2 = NULL;
    setup_registry(&reg2, &ctx2, &ops2, &def2, seq2, 1);
    aegis_autonomous_agent_config_t cfg2 = {
        .provider_registry = reg2,
        .llm_provider_name = def2.name,
        .checkpoint_path = path,
        .max_iterations = 3,
    };
    aegis_autonomous_agent_t* aa2 = NULL;
    expect_ok(aegis_autonomous_agent_create(&aa2, &cfg2), "create2");
    expect_ok(aegis_autonomous_agent_restore(aa2, path), "restore2");
    aegis_autonomous_result_t res2 = {0};
    rc = aegis_autonomous_agent_run(aa2, "checkpoint goal", &res2);
    assert(rc == AEGIS_OK);
    assert(res2.recovered_from_checkpoint == true);
    printf("  recovery run ok PASS\n");
    aegis_autonomous_agent_destroy(aa2);
    teardown_registry(reg2, ctx2, ops2, def2.name);
    teardown_registry(reg, ctx, ops, def.name);
    unlink(path);
}

static void test_boundaries(void) {
    printf("[test] boundaries ...\n");
    const char* resp = "STEP|-1|computational||step1|x\n";
    const char* seq[1] = {resp};
    aegis_provider_registry_t* reg = NULL;
    llm_mock_ctx_t* ctx = NULL;
    const aegis_llm_ops_t* ops = NULL;
    aegis_provider_def_t def;
    setup_registry(&reg, &ctx, &ops, &def, seq, 1);
    aegis_autonomous_agent_config_t cfg = {
        .provider_registry = reg,
        .llm_provider_name = def.name,
        .max_iterations = 3,
    };
    aegis_autonomous_agent_t* aa = NULL;
    expect_ok(aegis_autonomous_agent_create(&aa, &cfg), "create");

    assert(aegis_autonomous_agent_run(aa, NULL, NULL) == AEGIS_ERR_INVALID);
    assert(aegis_autonomous_agent_run(aa, "", NULL) == AEGIS_ERR_INVALID);
    assert(aegis_autonomous_agent_run(NULL, "goal", NULL) == AEGIS_ERR_INVALID);
    assert(aegis_autonomous_agent_create(NULL, &cfg) == AEGIS_ERR_INVALID);
    assert(aegis_autonomous_agent_checkpoint_save(aa, NULL) == AEGIS_ERR_INVALID);

    aegis_autonomous_agent_destroy(aa);

    const char* fail_first =
        "STEP|-1|computational||fail_step|fail\n";
    const char* fail_second =
        "STEP|-1|computational||fail_step|fail again\n";
    const char* seq2[2] = {fail_first, fail_second};
    llm_mock_ctx_t* ctx2 = NULL;
    const aegis_llm_ops_t* ops2 = NULL;
    aegis_provider_def_t def2;
    aegis_provider_registry_t* reg2 = NULL;
    setup_registry(&reg2, &ctx2, &ops2, &def2, seq2, 2);
    aegis_autonomous_agent_config_t cfg2 = {
        .provider_registry = reg2,
        .llm_provider_name = def2.name,
        .max_iterations = 1,
    };
    aegis_autonomous_agent_t* aa2 = NULL;
    expect_ok(aegis_autonomous_agent_create(&aa2, &cfg2), "create2");
    aegis_autonomous_result_t res = {0};
    aegis_status_t rc = aegis_autonomous_agent_run(aa2, "bounded goal", &res);
    assert(rc == AEGIS_ERR_BUSY || rc == AEGIS_OK || rc == AEGIS_ERR_TOOL);
    printf("  max_iterations bound rc=%d PASS\n", (int)rc);
    aegis_autonomous_agent_destroy(aa2);
    teardown_registry(reg2, ctx2, ops2, def2.name);
    teardown_registry(reg, ctx, ops, def.name);
    printf("  boundaries PASS\n");
}

int main(void) {
    test_happy_path();
    test_retry_then_replan();
    test_timeout();
    test_cancellation();
    test_checkpoint_recovery();
    test_boundaries();
    printf("ALL_SYSTEM_TESTS PASSED\n");
    return 0;
}
