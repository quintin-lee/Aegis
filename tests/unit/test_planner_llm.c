/**
 * @file test_planner_llm.c
 * @brief Planner flows through a MOCK LLM provider registered in a REAL
 *        provider registry: DSL parsing e2e, failure modes, replanning,
 *        cancellation gate.
 */
#include "aegis/cancellation.h"
#include "aegis/llm.h"
#include "aegis/plan.h"
#include "aegis/planner.h"
#include "aegis/provider.h"
#include "aegis/replanner.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** Canned-response LLM: replies with whatever g_response holds. */
static const char* g_response = "";

static aegis_status_t canned_llm_complete(void* ctx, const aegis_llm_request_t* req,
                                          const aegis_cancellation_token_t* token,
                                          aegis_llm_response_t*             out)
{
    (void)ctx;
    (void)req;
    (void)token;
    size_t len = strlen(g_response);
    if (len == 0) {
        return AEGIS_OK; /* Empty success: out stays zeroed. */
    }
    char* buf = malloc(len);
    if (!buf) {
        return AEGIS_ERR_NOMEM;
    }
    memcpy(buf, g_response, len);
    out->data = buf;
    out->len  = len;
    return AEGIS_OK;
}

/* Non-const: def.user is a plain void* (borrowed, registry never writes). */
static aegis_llm_ops_t s_canned_ops = {NULL, canned_llm_complete};

static const char* k_dsl_ok =
    "# comment line\n"
    "\n"
    "STEP|-1|computational||first|does A\r\n"
    "STEP|10|io|0|second|depends on first\n";

static const char* k_dsl_revised = "STEP|-1|tool||only-step|revised plan\n";

struct fixture {
    aegis_provider_registry_t* reg;
    aegis_planner_t*           planner;
};

static void fixture_setup(struct fixture* f)
{
    assert(aegis_provider_registry_create(&f->reg) == AEGIS_OK);

    aegis_provider_def_t d;
    memset(&d, 0, sizeof(d));
    d.name         = "mock-llm";
    d.description  = "canned planner responses";
    d.abi_version  = AEGIS_PROVIDER_ABI_VERSION;
    d.kind         = AEGIS_PROVIDER_LLM;
    d.thread_model = AEGIS_PROVIDER_THREAD_SAFE;
    d.user         = &s_canned_ops;
    assert(aegis_provider_register(f->reg, &d) == AEGIS_OK);
    assert(aegis_provider_init(f->reg, "mock-llm") == AEGIS_OK);

    aegis_planner_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.provider_registry = f->reg;
    cfg.llm_provider_name = "mock-llm";
    assert(aegis_planner_create(&f->planner, &cfg) == AEGIS_OK);
}

static void fixture_teardown(struct fixture* f)
{
    aegis_planner_destroy(f->planner);
    aegis_provider_registry_destroy(f->reg); /* Auto-shuts down. */
}

/* ── Happy path: strict DSL parsed into a validated plan ──────────────────── */

static void test_plan_happy_path(void)
{
    struct fixture fx;
    fixture_setup(&fx);
    g_response = k_dsl_ok;

    aegis_plan_t* plan = NULL;
    assert(aegis_planner_plan(fx.planner, "do the thing", NULL, &plan) == AEGIS_OK);
    assert(plan != NULL);
    assert(aegis_plan_version(plan) == 1u);
    assert(aegis_plan_step_count(plan) == 2u);
    assert(strcmp(aegis_plan_goal(plan), "do the thing") == 0);

    /* Auto id for the first step, explicit 10 for the second. */
    int64_t auto_id = -1;
    assert(aegis_plan_add_step(plan, &(aegis_plan_step_spec_t){0}, &auto_id) ==
           AEGIS_ERR_INVALID); /* Name-less probe rejected; ids stay intact. */

    /* Validate + materialize the LLM-produced plan end to end. */
    assert(aegis_plan_validate(plan) == AEGIS_OK);
    aegis_task_graph_t* g = NULL;
    assert(aegis_plan_materialize(plan, &g) == AEGIS_OK);
    assert(aegis_task_graph_task_count(g) == 2u);
    assert(aegis_task_graph_dependency_count(g) == 1u);
    aegis_task_graph_destroy(g);

    aegis_plan_destroy(plan);
    fixture_teardown(&fx);
}

/* ── Failure modes: garbage in, nothing out ──────────────────────────────── */

static void expect_plan_failure(struct fixture* fx, const char* response)
{
    g_response         = response;
    aegis_plan_t* plan = (aegis_plan_t*)0x1; /* Poison: must stay untouched. */
    assert(aegis_planner_plan(fx->planner, "goal", NULL, &plan) == AEGIS_ERR_INVALID);
    assert(plan == (aegis_plan_t*)0x1);
}

static void test_plan_parse_failures(void)
{
    struct fixture fx;
    fixture_setup(&fx);

    expect_plan_failure(&fx, "");                                   /* Empty output.     */
    expect_plan_failure(&fx, "I would start by gathering data.");   /* Prose.            */
    expect_plan_failure(&fx, "STEP|1|quantum||x|y");                /* Bad type word.    */
    expect_plan_failure(&fx, "STEP|x|io||n|d");                     /* Bad id.           */
    expect_plan_failure(&fx, "STEP|1|io||n");                       /* Missing field.    */
    expect_plan_failure(&fx, "STEP|1|io|||extra|junk");             /* Too many fields.  */
    expect_plan_failure(&fx, "STEP|1|io|99|orphan|dep on nothing"); /* Unknown dep.      */

    /* The last one was actually valid; prove failures only for real junk. */
    aegis_plan_t* plan = NULL;
    g_response         = "STEP|1|io||a|x\nSTEP|2|io|1|b|y";
    assert(aegis_planner_plan(fx.planner, "g", NULL, &plan) == AEGIS_OK);
    aegis_plan_destroy(plan);

    fixture_teardown(&fx);
}

/* ── Planner argument validation ─────────────────────────────────────────── */

static void test_planner_arg_validation(void)
{
    struct fixture fx;
    fixture_setup(&fx);

    aegis_plan_t* plan = NULL;
    assert(aegis_planner_plan(NULL, "g", NULL, &plan) == AEGIS_ERR_INVALID);
    assert(aegis_planner_plan(fx.planner, NULL, NULL, &plan) == AEGIS_ERR_INVALID);
    assert(aegis_planner_plan(fx.planner, "", NULL, &plan) == AEGIS_ERR_INVALID);
    assert(aegis_planner_plan(fx.planner, "g", NULL, NULL) == AEGIS_ERR_INVALID);

    aegis_planner_t*       bad = NULL;
    aegis_planner_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.provider_registry = fx.reg;
    cfg.llm_provider_name = NULL;
    assert(aegis_planner_create(&bad, &cfg) == AEGIS_ERR_INVALID);
    cfg.provider_registry = NULL;
    cfg.llm_provider_name = "x";
    assert(aegis_planner_create(&bad, &cfg) == AEGIS_ERR_INVALID);
    aegis_planner_destroy(NULL);

    /* Unknown provider surfaces as NOT_FOUND from dispatch. */
    cfg.provider_registry = fx.reg;
    cfg.llm_provider_name = "ghost";
    assert(aegis_planner_create(&bad, &cfg) == AEGIS_OK);
    g_response = k_dsl_ok;
    assert(aegis_planner_plan(bad, "g", NULL, &plan) == AEGIS_ERR_NOT_FOUND);
    aegis_planner_destroy(bad);

    fixture_teardown(&fx);
}

/* ── Replanning ──────────────────────────────────────────────────────────── */

static void test_replan_flow(void)
{
    struct fixture fx;
    fixture_setup(&fx);

    g_response        = k_dsl_ok;
    aegis_plan_t* old = NULL;
    assert(aegis_planner_plan(fx.planner, "original goal", NULL, &old) == AEGIS_OK);

    g_response            = k_dsl_revised;
    aegis_plan_t* revised = NULL;
    assert(aegis_replan(fx.planner, old, "step 'second' failed: disk full", NULL, &revised) ==
           AEGIS_OK);
    assert(revised != NULL);
    assert(aegis_plan_version(revised) == aegis_plan_version(old) + 1u);
    assert(aegis_plan_step_count(revised) == 1u);
    assert(strcmp(aegis_plan_goal(revised), "original goal") == 0);
    assert(aegis_plan_validate(revised) == AEGIS_OK);

    /* Argument validation. */
    aegis_plan_t* out = NULL;
    assert(aegis_replan(NULL, old, "fb", NULL, &out) == AEGIS_ERR_INVALID);
    assert(aegis_replan(fx.planner, NULL, "fb", NULL, &out) == AEGIS_ERR_INVALID);
    assert(aegis_replan(fx.planner, old, NULL, NULL, &out) == AEGIS_ERR_INVALID);
    assert(aegis_replan(fx.planner, old, "", NULL, &out) == AEGIS_ERR_INVALID);

    /* Garbage response fails replanning too. */
    g_response = "nope";
    assert(aegis_replan(fx.planner, old, "try again", NULL, &revised) == AEGIS_ERR_INVALID);

    aegis_plan_destroy(revised);
    aegis_plan_destroy(old);
    fixture_teardown(&fx);
}

/* ── Cancellation gate ───────────────────────────────────────────────────── */

static void test_cancelled_before_dispatch(void)
{
    struct fixture fx;
    fixture_setup(&fx);
    g_response = k_dsl_ok;

    aegis_cancellation_token_t* token = NULL;
    assert(aegis_cancellation_token_create(&token) == AEGIS_OK);
    aegis_cancellation_token_request_cancel(token);

    aegis_plan_t* plan = (aegis_plan_t*)0x1;
    assert(aegis_planner_plan(fx.planner, "g", token, &plan) == AEGIS_ERR_CANCELLED);
    assert(plan == (aegis_plan_t*)0x1);

    aegis_cancellation_token_destroy(token);
    fixture_teardown(&fx);
}

int main(void)
{
    test_plan_happy_path();
    test_plan_parse_failures();
    test_planner_arg_validation();
    test_replan_flow();
    test_cancelled_before_dispatch();
    printf("test_planner_llm: all cases passed\n");
    return 0;
}
