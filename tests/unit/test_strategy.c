/**
 * @file test_strategy.c
 * @brief Strategy interface + registry validation, the bundled
 *        plan_execute strategy end-to-end through a MOCK LLM provider,
 *        planner binding/routing semantics, and a compact register/find
 *        race case.
 */
#include "aegis/common/cancellation/cancellation.h"
#include "aegis/provider/llm.h"
#include "aegis/planner/plan.h"
#include "aegis/planner/planner.h"
#include "aegis/provider/provider.h"
#include "aegis/replanner/replanner.h"
#include "aegis/strategy/strategy.h"
#include "aegis/strategy/strategy_plan_execute.h"

#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Canned LLM provider (mirrors test_planner_llm.c) ─────────────────────── */

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

static aegis_provider_registry_t* make_provider_reg(void)
{
    aegis_provider_registry_t* reg = NULL;
    assert(aegis_provider_registry_create(&reg) == AEGIS_OK);

    aegis_provider_def_t d;
    memset(&d, 0, sizeof(d));
    d.name         = "mock-llm";
    d.description  = "canned strategy responses";
    d.abi_version  = AEGIS_PROVIDER_ABI_VERSION;
    d.kind         = AEGIS_PROVIDER_LLM;
    d.thread_model = AEGIS_PROVIDER_THREAD_SAFE;
    d.user         = &s_canned_ops;
    assert(aegis_provider_register(reg, &d) == AEGIS_OK);
    assert(aegis_provider_init(reg, "mock-llm") == AEGIS_OK);
    return reg;
}

static const char* k_dsl_two =
    "# two-step plan\n"
    "STEP|-1|computational||first|does A\n"
    "STEP|-1|io|0|second|depends on first\n";
static const char* k_dsl_garbage = "I would start by gathering data.";
static const char* k_dsl_revised = "STEP|-1|tool||only-step|revised plan\n";

struct fixture {
    aegis_provider_registry_t* prov_reg;
    aegis_planner_t*           planner;
};

static void fixture_setup(struct fixture* f)
{
    f->prov_reg = make_provider_reg();
    aegis_planner_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.provider_registry = f->prov_reg;
    cfg.llm_provider_name = "mock-llm";
    assert(aegis_planner_create(&f->planner, &cfg) == AEGIS_OK);
}

static void fixture_teardown(struct fixture* f)
{
    aegis_planner_destroy(f->planner);
    aegis_provider_registry_destroy(f->prov_reg); /* Auto-shuts down. */
}

/* ── Registry CRUD + definition validation ────────────────────────────────── */

/* Minimal well-typed plan-fn stand-in for validation-order checks;
 * never actually invoked by the registry CRUD case. */
static aegis_status_t noop_plan_fn(void* user, const aegis_strategy_input_t* input,
                                   const aegis_cancellation_token_t* token, aegis_plan_t** out)
{
    (void)user;
    (void)input;
    (void)token;
    (void)out;
    return AEGIS_ERR_INTERNAL;
}

static void test_registry_crud(void)
{
    assert(aegis_strategy_registry_create(NULL) == AEGIS_ERR_INVALID);

    aegis_strategy_registry_t* reg = NULL;
    assert(aegis_strategy_registry_create(&reg) == AEGIS_OK);

    aegis_strategy_def_t d;
    memset(&d, 0, sizeof(d));

    d.plan        = noop_plan_fn; /* Non-NULL stand-in for gating order checks. */
    d.abi_version = AEGIS_STRATEGY_ABI_VERSION;
    assert(aegis_strategy_register(reg, &d) == AEGIS_ERR_INVALID); /* name NULL     */
    d.name        = "";
    d.plan        = NULL;
    d.abi_version = AEGIS_STRATEGY_ABI_VERSION;
    assert(aegis_strategy_register(reg, &d) == AEGIS_ERR_INVALID); /* empty name    */
    d.name = "s";
    d.plan = NULL;
    assert(aegis_strategy_register(reg, &d) == AEGIS_ERR_INVALID); /* plan fn NULL  */
    d.plan        = noop_plan_fn;
    d.abi_version = 0u;
    assert(aegis_strategy_register(reg, &d) == AEGIS_ERR_INVALID); /* bad abi 0     */
    d.abi_version = AEGIS_STRATEGY_ABI_VERSION + 1u;
    assert(aegis_strategy_register(reg, &d) == AEGIS_ERR_INVALID); /* bad abi 2     */
    assert(aegis_strategy_count(reg) == 0u);

    d.abi_version = AEGIS_STRATEGY_ABI_VERSION;
    d.name        = "alpha";
    assert(aegis_strategy_register(reg, &d) == AEGIS_OK);
    assert(aegis_strategy_register(reg, &d) == AEGIS_ERR_BUSY); /* duplicate */
    assert(aegis_strategy_count(reg) == 1u);

    /* find returns a value copy: mutating it must not corrupt the entry. */
    aegis_strategy_view_t view;
    assert(aegis_strategy_find(reg, "alpha", &view) == AEGIS_OK);
    view.def.name = "tampered";
    aegis_strategy_view_t again;
    assert(aegis_strategy_find(reg, "alpha", &again) == AEGIS_OK);
    assert(strcmp(again.def.name, "alpha") == 0);
    assert(again.def.abi_version == AEGIS_STRATEGY_ABI_VERSION);

    assert(aegis_strategy_unregister(reg, "ghost") == AEGIS_ERR_NOT_FOUND);
    assert(aegis_strategy_unregister(reg, "alpha") == AEGIS_OK);
    assert(aegis_strategy_unregister(reg, "alpha") == AEGIS_ERR_NOT_FOUND);
    assert(aegis_strategy_register(reg, &d) == AEGIS_OK); /* Name reusable. */
    assert(aegis_strategy_count(reg) == 1u);

    assert(aegis_strategy_count(NULL) == 0u);
    aegis_strategy_registry_destroy(NULL); /* No-op. */
    aegis_strategy_registry_destroy(reg);
}

/* ── plan_execute end-to-end through the mock LLM ─────────────────────────── */

static void test_plan_execute_fresh(void)
{
    struct fixture fx;
    fixture_setup(&fx);

    aegis_strategy_registry_t* strat = NULL;
    assert(aegis_strategy_registry_create(&strat) == AEGIS_OK);

    const aegis_strategy_plan_execute_ctx_t ctx = {fx.prov_reg, "mock-llm"};
    aegis_strategy_def_t                    def;
    assert(aegis_strategy_plan_execute_def(NULL, &def) == AEGIS_ERR_INVALID);
    assert(aegis_strategy_plan_execute_def(&ctx, NULL) == AEGIS_ERR_INVALID);
    const aegis_strategy_plan_execute_ctx_t bad_ctx = {NULL, "mock-llm"};
    assert(aegis_strategy_plan_execute_def(&bad_ctx, &def) == AEGIS_ERR_INVALID);
    assert(aegis_strategy_plan_execute_def(&ctx, &def) == AEGIS_OK);
    assert(strcmp(def.name, "plan_execute") == 0);
    assert(def.abi_version == AEGIS_STRATEGY_ABI_VERSION);
    assert(aegis_strategy_register(strat, &def) == AEGIS_OK);

    assert(aegis_planner_attach_strategies(fx.planner, strat) == AEGIS_OK);
    assert(aegis_planner_use_strategy(fx.planner, "plan_execute") == AEGIS_OK);

    g_response         = k_dsl_two;
    aegis_plan_t* plan = NULL;
    assert(aegis_planner_plan(fx.planner, "build widget", NULL, &plan) == AEGIS_OK);
    assert(plan != NULL);
    assert(aegis_plan_version(plan) == 1u);
    assert(aegis_plan_step_count(plan) == 2u);
    assert(aegis_plan_validate(plan) == AEGIS_OK);
    aegis_plan_destroy(plan);

    /* Garbage model output fails the attempt without touching out. */
    g_response         = k_dsl_garbage;
    aegis_plan_t* dead = (aegis_plan_t*)0x1;
    assert(aegis_planner_plan(fx.planner, "g", NULL, &dead) == AEGIS_ERR_INVALID);
    assert(dead == (aegis_plan_t*)0x1);

    aegis_strategy_registry_destroy(strat);
    fixture_teardown(&fx);
}

/* ── Revision flow: previous plan + feedback -> version+1 ─────────────────── */

static void test_plan_execute_revision(void)
{
    struct fixture fx;
    fixture_setup(&fx);

    aegis_strategy_registry_t* strat = NULL;
    assert(aegis_strategy_registry_create(&strat) == AEGIS_OK);
    const aegis_strategy_plan_execute_ctx_t ctx = {fx.prov_reg, "mock-llm"};
    aegis_strategy_def_t                    def;
    assert(aegis_strategy_plan_execute_def(&ctx, &def) == AEGIS_OK);
    assert(aegis_strategy_register(strat, &def) == AEGIS_OK);
    assert(aegis_planner_attach_strategies(fx.planner, strat) == AEGIS_OK);
    assert(aegis_planner_use_strategy(fx.planner, "plan_execute") == AEGIS_OK);

    /* Old plan at version 3 with two steps. */
    aegis_plan_t* old = NULL;
    assert(aegis_plan_create(&old, "ship release") == AEGIS_OK);
    aegis_plan_step_spec_t s;
    memset(&s, 0, sizeof(s));
    s.step_id  = AEGIS_PLAN_STEP_ID_AUTO;
    s.name     = "build";
    s.type     = AEGIS_TASK_TYPE_COMPUTATIONAL;
    int64_t id = -1;
    assert(aegis_plan_add_step(old, &s, &id) == AEGIS_OK);
    s.name      = "upload";
    s.deps      = &id;
    s.dep_count = 1u;
    int64_t id2 = -1;
    assert(aegis_plan_add_step(old, &s, &id2) == AEGIS_OK);
    aegis_plan_set_version(old, 3u);

    g_response            = k_dsl_revised;
    aegis_plan_t* revised = NULL;
    assert(aegis_replan(fx.planner, old, "step 'build' failed: disk full", NULL, &revised) ==
           AEGIS_OK);
    assert(revised != NULL);
    assert(aegis_plan_version(revised) == 4u); /* Replanner stamps, not the strategy. */
    assert(aegis_plan_step_count(revised) == 1u);
    assert(strcmp(aegis_plan_goal(revised), "ship release") == 0);
    assert(aegis_plan_validate(revised) == AEGIS_OK);
    aegis_plan_destroy(revised);
    aegis_plan_destroy(old);

    aegis_strategy_registry_destroy(strat);
    fixture_teardown(&fx);
}

/* ── Pre-cancelled token gate ─────────────────────────────────────────────── */

static void test_pre_cancelled_token(void)
{
    struct fixture fx;
    fixture_setup(&fx);

    aegis_strategy_registry_t* strat = NULL;
    assert(aegis_strategy_registry_create(&strat) == AEGIS_OK);
    const aegis_strategy_plan_execute_ctx_t ctx = {fx.prov_reg, "mock-llm"};
    aegis_strategy_def_t                    def;
    assert(aegis_strategy_plan_execute_def(&ctx, &def) == AEGIS_OK);
    assert(aegis_strategy_register(strat, &def) == AEGIS_OK);
    assert(aegis_planner_attach_strategies(fx.planner, strat) == AEGIS_OK);
    assert(aegis_planner_use_strategy(fx.planner, "plan_execute") == AEGIS_OK);
    g_response = k_dsl_two;

    aegis_cancellation_token_t* token = NULL;
    assert(aegis_cancellation_token_create(&token) == AEGIS_OK);
    aegis_cancellation_token_request_cancel(token);

    aegis_plan_t* plan = (aegis_plan_t*)0x1;
    assert(aegis_planner_plan(fx.planner, "g", token, &plan) == AEGIS_ERR_CANCELLED);
    assert(plan == (aegis_plan_t*)0x1);

    aegis_cancellation_token_destroy(token);
    aegis_strategy_registry_destroy(strat);
    fixture_teardown(&fx);
}

/* ── Planner routing: bound strategy replaces the built-in path ───────────── */

static atomic_int g_sentinel_saw_prev = 0;
static char       g_sentinel_feedback[64];

/* Sentinel strategy: never consults the LLM; returns a fixed one-step plan. */
static aegis_status_t sentinel_plan(void* user, const aegis_strategy_input_t* input,
                                    const aegis_cancellation_token_t* token, aegis_plan_t** out)
{
    (void)user;
    (void)token;
    if (!input || !input->goal || input->goal[0] == '\0') {
        return AEGIS_ERR_INVALID;
    }
    atomic_store(&g_sentinel_saw_prev, input->previous_plan != NULL ? 1 : 0);
    if (input->feedback) {
        snprintf(g_sentinel_feedback, sizeof(g_sentinel_feedback), "%s", input->feedback);
    } else {
        g_sentinel_feedback[0] = '\0';
    }

    aegis_plan_t*  plan = NULL;
    aegis_status_t rc   = aegis_plan_create(&plan, input->goal);
    if (rc != AEGIS_OK) {
        return rc;
    }
    aegis_plan_step_spec_t s;
    memset(&s, 0, sizeof(s));
    s.step_id  = AEGIS_PLAN_STEP_ID_AUTO;
    s.name     = "custom-strategy-plan";
    s.type     = AEGIS_TASK_TYPE_COMPUTATIONAL;
    int64_t id = -1;
    rc         = aegis_plan_add_step(plan, &s, &id);
    if (rc != AEGIS_OK) {
        aegis_plan_destroy(plan);
        return rc;
    }
    *out = plan;
    return AEGIS_OK;
}

static void test_planner_routing(void)
{
    struct fixture fx;
    fixture_setup(&fx);

    aegis_strategy_registry_t* strat = NULL;
    assert(aegis_strategy_registry_create(&strat) == AEGIS_OK);
    aegis_strategy_def_t sentinel;
    memset(&sentinel, 0, sizeof(sentinel));
    sentinel.name        = "sentinel";
    sentinel.description = "test double";
    sentinel.abi_version = AEGIS_STRATEGY_ABI_VERSION;
    sentinel.plan        = sentinel_plan;
    assert(aegis_strategy_register(strat, &sentinel) == AEGIS_OK);

    /* Selecting a strategy with nothing attached fails up front. */
    assert(aegis_planner_use_strategy(fx.planner, "sentinel") == AEGIS_ERR_INVALID);

    assert(aegis_planner_attach_strategies(fx.planner, strat) == AEGIS_OK);

    /* Unknown name selects fine; resolution happens per planning call. */
    assert(aegis_planner_use_strategy(fx.planner, "ghost") == AEGIS_OK);
    g_response         = k_dsl_two; /* Would succeed on the built-in path. */
    aegis_plan_t* dead = (aegis_plan_t*)0x1;
    assert(aegis_planner_plan(fx.planner, "g", NULL, &dead) == AEGIS_ERR_NOT_FOUND);
    assert(dead == (aegis_plan_t*)0x1);

    /* Bound sentinel: LLM response would be garbage, yet planning succeeds. */
    assert(aegis_planner_use_strategy(fx.planner, "sentinel") == AEGIS_OK);
    g_response           = k_dsl_garbage; /* Proves the LLM is never consulted. */
    aegis_plan_t* routed = NULL;
    assert(aegis_planner_plan(fx.planner, "routed goal", NULL, &routed) == AEGIS_OK);
    assert(routed != NULL);
    assert(aegis_plan_step_count(routed) == 1u);
    assert(strcmp(aegis_plan_goal(routed), "routed goal") == 0);
    aegis_plan_destroy(routed);

    /* Deselecting restores the built-in DSL path. */
    assert(aegis_planner_use_strategy(fx.planner, NULL) == AEGIS_OK);
    g_response            = k_dsl_two;
    aegis_plan_t* builtin = NULL;
    assert(aegis_planner_plan(fx.planner, "g", NULL, &builtin) == AEGIS_OK);
    assert(aegis_plan_step_count(builtin) == 2u);
    aegis_plan_destroy(builtin);

    /* Replan routes through the sentinel and carries previous plan + feedback;
     * version stamping stays with the replanner. First obtain a valid old
     * plan on the BUILT-IN path, then flip back to the sentinel. */
    assert(aegis_planner_use_strategy(fx.planner, NULL) == AEGIS_OK);
    g_response         = k_dsl_two;
    aegis_plan_t* base = NULL;
    assert(aegis_planner_plan(fx.planner, "base goal", NULL, &base) == AEGIS_OK);
    atomic_store(&g_sentinel_saw_prev, 0);
    g_sentinel_feedback[0] = '\0';

    assert(aegis_planner_use_strategy(fx.planner, "sentinel") == AEGIS_OK);
    g_response         = k_dsl_garbage;
    aegis_plan_t* next = NULL;
    assert(aegis_replan(fx.planner, base, "fix the broken step", NULL, &next) == AEGIS_OK);
    assert(next != NULL);
    assert(atomic_load(&g_sentinel_saw_prev) == 1);
    assert(strstr(g_sentinel_feedback, "fix the broken step") != NULL);
    assert(aegis_plan_version(next) == aegis_plan_version(base) + 1u);
    assert(strcmp(aegis_plan_goal(next), "base goal") == 0);
    aegis_plan_destroy(next);
    aegis_plan_destroy(base);

    /* Argument validation still applies on the strategy path. */
    assert(aegis_replan(NULL, base, "fb", NULL, &next) == AEGIS_ERR_INVALID);
    assert(aegis_replan(fx.planner, NULL, "fb", NULL, &next) == AEGIS_ERR_INVALID);
    assert(aegis_replan(fx.planner, base, "", NULL, &next) == AEGIS_ERR_INVALID);

    aegis_strategy_registry_destroy(strat);
    fixture_teardown(&fx);
}

/* ── Compact register/find race ───────────────────────────────────────────── */

#define RACE_READERS 4
#define RACE_WRITERS 4
#define RACE_ROUNDS  200

struct race_arg {
    aegis_strategy_registry_t* reg;
    long                       tid;
};

static void* race_reader(void* p)
{
    struct race_arg*   a       = p;
    static const char* names[] = {"a", "b", "c", "d"};
    for (int i = 0; i < 500; i++) {
        for (unsigned n = 0; n < 4u; n++) {
            aegis_strategy_view_t v;
            if (aegis_strategy_find(a->reg, names[n], &v) == AEGIS_OK) {
                if (v.def.abi_version != AEGIS_STRATEGY_ABI_VERSION ||
                    strcmp(v.def.name, names[n]) != 0) {
                    return (void*)1; /* Torn read. */
                }
            }
        }
    }
    return NULL;
}

static void* race_writer(void* p)
{
    struct race_arg* a = p;
    for (int i = 0; i < RACE_ROUNDS; i++) {
        char name[32];
        snprintf(name, sizeof(name), "dyn-%ld-%d", a->tid, i);
        aegis_strategy_def_t d;
        memset(&d, 0, sizeof(d));
        d.name            = name;
        d.description     = "transient";
        d.abi_version     = AEGIS_STRATEGY_ABI_VERSION;
        d.plan            = sentinel_plan;
        aegis_status_t rc = aegis_strategy_register(a->reg, &d);
        if (rc == AEGIS_OK) {
            assert(aegis_strategy_unregister(a->reg, name) == AEGIS_OK);
        } else {
            assert(rc == AEGIS_ERR_BUSY);
        }
    }
    return NULL;
}

static void test_registry_race_compact(void)
{
    aegis_strategy_registry_t* reg = NULL;
    assert(aegis_strategy_registry_create(&reg) == AEGIS_OK);

    static const char* fixed[] = {"a", "b", "c", "d"};
    for (unsigned n = 0; n < 4u; n++) {
        aegis_strategy_def_t d;
        memset(&d, 0, sizeof(d));
        d.name        = fixed[n];
        d.description = "stable";
        d.abi_version = AEGIS_STRATEGY_ABI_VERSION;
        d.plan        = sentinel_plan;
        assert(aegis_strategy_register(reg, &d) == AEGIS_OK);
    }

    pthread_t       readers[RACE_READERS];
    pthread_t       writers[RACE_WRITERS];
    struct race_arg rargs[RACE_READERS];
    struct race_arg wargs[RACE_WRITERS];
    for (long t = 0; t < RACE_READERS; t++) {
        rargs[t].reg = reg;
        rargs[t].tid = t;
        assert(pthread_create(&readers[t], NULL, race_reader, &rargs[t]) == 0);
    }
    for (long t = 0; t < RACE_WRITERS; t++) {
        wargs[t].reg = reg;
        wargs[t].tid = t;
        assert(pthread_create(&writers[t], NULL, race_writer, &wargs[t]) == 0);
    }
    for (int t = 0; t < RACE_READERS; t++) {
        assert(pthread_join(readers[t], NULL) == 0);
    }
    for (int t = 0; t < RACE_WRITERS; t++) {
        assert(pthread_join(writers[t], NULL) == 0);
    }

    /* All transients gone; exactly the four fixed entries remain. */
    assert(aegis_strategy_count(reg) == 4u);
    aegis_strategy_registry_destroy(reg);
}

int main(void)
{
    atomic_init(&g_sentinel_saw_prev, 0);
    test_registry_crud();
    test_plan_execute_fresh();
    test_plan_execute_revision();
    test_pre_cancelled_token();
    test_planner_routing();
    test_registry_race_compact();
    printf("test_strategy: all cases passed\n");
    return 0;
}
