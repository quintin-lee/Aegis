/**
 * @file test_planner.c
 * @brief Unit tests: plan core, validation, materialization, reflection.
 *
 * Pure planner-module tests - no LLM provider involved (see
 * test_planner_llm.c for the model-backed flows).
 */
#include "aegis/graph.h"
#include "aegis/plan.h"
#include "aegis/reflection.h"
#include "aegis/task.h"
#include "aegis/executor.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Plan lifecycle ────────────────────────────────────────────────────────── */

static void test_plan_lifecycle(void)
{
    aegis_plan_t* p = NULL;
    assert(aegis_plan_create(NULL, "g") == AEGIS_ERR_INVALID);
    assert(aegis_plan_create(&p, NULL) == AEGIS_ERR_INVALID);
    assert(aegis_plan_create(&p, "") == AEGIS_ERR_INVALID);

    char goal[] = "build a house";
    assert(aegis_plan_create(&p, goal) == AEGIS_OK);
    goal[0] = 'X';                                            /* Mutate the caller buffer... */
    assert(strcmp(aegis_plan_goal(p), "build a house") == 0); /* ...plan keeps its copy. */
    assert(aegis_plan_version(p) == 1u);
    assert(aegis_plan_step_count(p) == 0u);
    aegis_plan_set_version(p, 7u);
    assert(aegis_plan_version(p) == 7u);

    aegis_plan_destroy(p);
    aegis_plan_destroy(NULL); /* No-op. */
}

/* ── Step construction rules ──────────────────────────────────────────────── */

static aegis_plan_step_spec_t base_spec(const char* name)
{
    aegis_plan_step_spec_t s;
    memset(&s, 0, sizeof(s));
    s.step_id = AEGIS_PLAN_STEP_ID_AUTO;
    s.name    = name;
    s.type    = AEGIS_TASK_TYPE_COMPUTATIONAL;
    return s;
}

static void test_add_step_rules(void)
{
    aegis_plan_t* p = NULL;
    assert(aegis_plan_create(&p, "goal") == AEGIS_OK);

    aegis_plan_step_spec_t nullplan = base_spec("x");
    assert(aegis_plan_add_step(NULL, &nullplan, NULL) == AEGIS_ERR_INVALID);
    assert(aegis_plan_add_step(p, NULL, NULL) == AEGIS_ERR_INVALID);

    aegis_plan_step_spec_t noname = base_spec("");
    assert(aegis_plan_add_step(p, &noname, NULL) == AEGIS_ERR_INVALID);
    noname.name = NULL;
    assert(aegis_plan_add_step(p, &noname, NULL) == AEGIS_ERR_INVALID);

    int64_t                a = -1, b = -1;
    aegis_plan_step_spec_t sa = base_spec("a");
    aegis_plan_step_spec_t sb = base_spec("b");
    assert(aegis_plan_add_step(p, &sa, &a) == AEGIS_OK);
    assert(aegis_plan_add_step(p, &sb, &b) == AEGIS_OK);
    assert(a != b);
    assert(aegis_plan_step_count(p) == 2u);
    assert(strcmp(aegis_plan_goal(p), "goal") == 0);

    /* Explicit duplicate id. */
    aegis_plan_step_spec_t dup = base_spec("dup");
    dup.step_id                = a;
    assert(aegis_plan_add_step(p, &dup, NULL) == AEGIS_ERR_BUSY);

    /* Unknown dependency. */
    int64_t                unknown_dep = 424242;
    aegis_plan_step_spec_t orphan      = base_spec("orphan");
    orphan.deps                        = &unknown_dep;
    orphan.dep_count                   = 1;
    assert(aegis_plan_add_step(p, &orphan, NULL) == AEGIS_ERR_INVALID);

    /* Self dependency. */
    int64_t self = a;
    aegis_plan_destroy(p);
    assert(aegis_plan_create(&p, "goal2") == AEGIS_OK);
    int64_t                x  = -1;
    aegis_plan_step_spec_t sx = base_spec("x");
    assert(aegis_plan_add_step(p, &sx, &x) == AEGIS_OK);
    aegis_plan_step_spec_t selfy = base_spec("selfy");
    selfy.step_id                = 50;
    selfy.deps                   = &self;
    selfy.dep_count              = 0; /* deps ignored when count is 0. */
    assert(aegis_plan_add_step(p, &selfy, NULL) == AEGIS_OK);

    /* Self-dependency via explicit dep on own id. */
    aegis_plan_step_spec_t loop = base_spec("loop");
    loop.step_id                = 51;
    int64_t own                 = 51;
    loop.deps                   = &own;
    loop.dep_count              = 1;
    assert(aegis_plan_add_step(p, &loop, NULL) == AEGIS_ERR_INVALID);

    aegis_plan_destroy(p);
}

/* ── Validation ───────────────────────────────────────────────────────────── */

static void test_validate(void)
{
    aegis_plan_t* p = NULL;
    assert(aegis_plan_create(&p, "g") == AEGIS_OK);
    assert(aegis_plan_validate(p) == AEGIS_ERR_INVALID); /* Empty plan. */

    int64_t                first  = -1;
    aegis_plan_step_spec_t sfetch = base_spec("fetch");
    assert(aegis_plan_add_step(p, &sfetch, &first) == AEGIS_OK);
    assert(aegis_plan_validate(p) == AEGIS_OK); /* Single step. */

    aegis_plan_step_spec_t second = base_spec("process");
    second.deps                   = &first;
    second.dep_count              = 1;
    int64_t sid                   = -1;
    assert(aegis_plan_add_step(p, &second, &sid) == AEGIS_OK);
    assert(aegis_plan_validate(p) == AEGIS_OK);
    assert(aegis_plan_step_dep_count(p, sid) == 1u);
    assert(aegis_plan_step_dep_count(p, 999) == 0u);

    aegis_plan_destroy(p);
}

/* ── Materialization ──────────────────────────────────────────────────────── */

static const aegis_task_t* find_by_name(const aegis_task_graph_t* g, const char* name)
{
    aegis_task_t** tasks = NULL;
    size_t         count = 0;
    assert(aegis_task_graph_tasks(g, &tasks, &count) == AEGIS_OK);
    const aegis_task_t* found = NULL;
    for (size_t i = 0; i < count; i++) {
        if (strcmp(aegis_task_name(tasks[i]), name) == 0) {
            found = tasks[i];
        }
    }
    free(tasks);
    return found;
}

static void test_materialize(void)
{
    aegis_plan_t* p = NULL;
    assert(aegis_plan_create(&p, "pipeline") == AEGIS_OK);

    int64_t                fetch   = -1;
    aegis_plan_step_spec_t s_fetch = base_spec("fetch");
    assert(aegis_plan_add_step(p, &s_fetch, &fetch) == AEGIS_OK);

    aegis_plan_step_spec_t mid = base_spec("transform");
    mid.deps                   = &fetch;
    mid.dep_count              = 1;
    mid.type                   = AEGIS_TASK_TYPE_TOOL;
    mid.priority               = 5;
    mid.tool_name              = "transformer";
    mid.timeout_ms             = 1500;
    mid.retry.max_attempts     = 2;
    mid.retry.delay_ms         = 10;
    const char input[]         = "raw-bytes";
    mid.input                  = input;
    mid.input_len              = strlen(input);
    int64_t mid_id             = -1;
    assert(aegis_plan_add_step(p, &mid, &mid_id) == AEGIS_OK);

    aegis_plan_step_spec_t last = base_spec("store");
    last.deps                   = &mid_id;
    last.dep_count              = 1;
    last.type                   = AEGIS_TASK_TYPE_IO;
    int64_t last_id             = -1;
    assert(aegis_plan_add_step(p, &last, &last_id) == AEGIS_OK);

    assert(aegis_plan_validate(p) == AEGIS_OK);

    aegis_task_graph_t* g = NULL;
    assert(aegis_plan_materialize(NULL, &g) == AEGIS_ERR_INVALID);
    assert(aegis_plan_materialize(p, NULL) == AEGIS_ERR_INVALID);
    assert(aegis_plan_materialize(p, &g) == AEGIS_OK);
    assert(aegis_task_graph_task_count(g) == 3u);
    assert(aegis_task_graph_dependency_count(g) == 2u);

    const aegis_task_t* t_fetch = find_by_name(g, "fetch");
    const aegis_task_t* t_mid   = find_by_name(g, "transform");
    const aegis_task_t* t_last  = find_by_name(g, "store");
    assert(t_fetch && t_mid && t_last);
    assert(aegis_task_state(t_fetch) == AEGIS_TASK_PENDING);
    assert(aegis_task_type(t_mid) == AEGIS_TASK_TYPE_TOOL);
    assert(aegis_task_priority(t_mid) == 5);
    assert(aegis_task_timeout_ms(t_mid) == 1500);
    assert(aegis_task_retry_policy(t_mid).max_attempts == 2);
    size_t out_len = 0;
    assert(aegis_task_output(t_mid, &out_len) == NULL && out_len == 0);
    const void* in_data = aegis_task_input(t_mid, &out_len);
    assert(in_data && out_len == strlen(input) && memcmp(in_data, input, out_len) == 0);
    const char* tool_meta = aegis_task_get_metadata(t_mid, "tool");
    assert(tool_meta && strcmp(tool_meta, "transformer") == 0);
    assert(aegis_task_get_metadata(t_last, "tool") == NULL);

    /* Plan stays reusable: materialize again into an independent graph. */
    aegis_task_graph_t* g2 = NULL;
    assert(aegis_plan_materialize(p, &g2) == AEGIS_OK);
    assert(aegis_task_graph_task_count(g2) == 3u);
    aegis_task_graph_destroy(g2);

    aegis_task_graph_destroy(g); /* Destroys owned tasks. */
    aegis_plan_destroy(p);
}

/* ── Serialization ────────────────────────────────────────────────────────── */

static void test_serialize(void)
{
    aegis_plan_t* p = NULL;
    assert(aegis_plan_create(&p, "demo") == AEGIS_OK);
    int64_t                one   = -1;
    aegis_plan_step_spec_t s_one = base_spec("one");
    assert(aegis_plan_add_step(p, &s_one, &one) == AEGIS_OK);
    aegis_plan_step_spec_t two = base_spec("two");
    two.desc                   = "with desc";
    two.type                   = AEGIS_TASK_TYPE_SHELL;
    two.deps                   = &one;
    two.dep_count              = 1;
    assert(aegis_plan_add_step(p, &two, NULL) == AEGIS_OK);

    char* str = NULL;
    assert(aegis_plan_serialize(p, &str) == AEGIS_OK);
    assert(str);
    assert(strstr(str, "PLAN|1\n") == str);
    assert(strstr(str, "|one|") != NULL);
    assert(strstr(str, "shell|0|two|with desc") != NULL); /* Auto ids are 0-based. */
    free(str);

    aegis_plan_destroy(p);
}

/* ── Reflection ───────────────────────────────────────────────────────────── */

static void test_reflection_counts(void)
{
    aegis_task_graph_t* g = NULL;
    assert(aegis_task_graph_create(&g) == AEGIS_OK);

    aegis_task_t* ok1  = NULL;
    aegis_task_t* bad  = NULL;
    aegis_task_t* gone = NULL;
    aegis_task_t* skip = NULL;
    aegis_task_t* pend = NULL;
    assert(aegis_task_create(&ok1, "ok", NULL) == AEGIS_OK);
    assert(aegis_task_create(&bad, "bad", NULL) == AEGIS_OK);
    assert(aegis_task_create(&gone, "gone", NULL) == AEGIS_OK);
    assert(aegis_task_create(&skip, "skip", NULL) == AEGIS_OK);
    assert(aegis_task_create(&pend, "pend", NULL) == AEGIS_OK);
    assert(aegis_task_graph_add_task(g, ok1) == AEGIS_OK);
    assert(aegis_task_graph_add_task(g, bad) == AEGIS_OK);
    assert(aegis_task_graph_add_task(g, gone) == AEGIS_OK);
    assert(aegis_task_graph_add_task(g, skip) == AEGIS_OK);
    assert(aegis_task_graph_add_task(g, pend) == AEGIS_OK);

    aegis_task_set_state_for_test(ok1, AEGIS_TASK_SUCCESS);
    aegis_task_set_state_for_test(bad, AEGIS_TASK_FAILED);
    aegis_task_set_state_for_test(gone, AEGIS_TASK_CANCELLED);
    aegis_task_set_state_for_test(skip, AEGIS_TASK_SKIPPED);
    /* pend stays PENDING -> incomplete. */

    aegis_reflection_t* r = NULL;
    assert(aegis_reflection_create(NULL, g) == AEGIS_ERR_INVALID);
    assert(aegis_reflection_create(&r, NULL) == AEGIS_ERR_INVALID);
    assert(aegis_reflection_create(&r, g) == AEGIS_OK);

    assert(aegis_reflection_success_count(r) == 1u);
    assert(aegis_reflection_failed_count(r) == 1u);
    assert(aegis_reflection_cancelled_count(r) == 1u);
    assert(aegis_reflection_skipped_count(r) == 1u);
    assert(aegis_reflection_incomplete_count(r) == 1u);
    /* FAILED task has no error message here -> first_error is NULL. */
    assert(aegis_reflection_first_error(r) == NULL);

    const char* fb = aegis_reflection_feedback(r);
    assert(fb && strstr(fb, "1 succeeded") && strstr(fb, "1 failed") && strstr(fb, "1 incomplete"));
    aegis_reflection_destroy(r);
    aegis_reflection_destroy(NULL); /* No-op. */

    aegis_task_graph_destroy(g);
}

/** Realistic failure path: run one failing task through the executor so the
 * task ends FAILED with an error message attached. */
static aegis_status_t always_fail_work(aegis_task_t* task, const aegis_cancellation_token_t* token,
                                       void* user)
{
    (void)task;
    (void)token;
    (void)user;
    return AEGIS_ERR_PROVIDER;
}

static void test_reflection_first_error_via_executor(void)
{
    aegis_executor_t*       ex  = NULL;
    aegis_executor_config_t cfg = {0};
    cfg.worker_count            = 1;
    cfg.queue_capacity          = 8;
    assert(aegis_executor_create(&ex, &cfg) == AEGIS_OK);

    aegis_task_graph_t* g = NULL;
    assert(aegis_task_graph_create(&g) == AEGIS_OK);

    aegis_task_t* doomed = NULL;
    assert(aegis_task_create(&doomed, "doomed", NULL) == AEGIS_OK);
    aegis_task_set_timeout_ms(doomed, 5000);
    assert(aegis_task_graph_add_task(g, doomed) == AEGIS_OK);

    assert(aegis_executor_submit(ex, doomed, always_fail_work, NULL) == AEGIS_OK);
    aegis_exec_result_t res;
    memset(&res, 0, sizeof(res));
    assert(aegis_executor_wait(ex, aegis_task_id(doomed), &res, -1) == AEGIS_OK);
    assert(res.outcome == AEGIS_EXEC_FAILED);
    assert(aegis_task_state(doomed) == AEGIS_TASK_FAILED);
    assert(aegis_task_error(doomed) != NULL && aegis_task_error(doomed)[0] != '\0');

    aegis_reflection_t* r = NULL;
    assert(aegis_reflection_create(&r, g) == AEGIS_OK);
    assert(aegis_reflection_failed_count(r) == 1u);
    const char* err = aegis_reflection_first_error(r);
    assert(err && err[0] != '\0');
    const char* fb = aegis_reflection_feedback(r);
    assert(fb && strstr(fb, "'doomed'")); /* Feedback names the failing step. */
    aegis_reflection_destroy(r);

    aegis_executor_destroy(ex); /* Graph still owns the task. */
    aegis_task_graph_destroy(g);
}

int main(void)
{
    test_plan_lifecycle();
    test_add_step_rules();
    test_validate();
    test_materialize();
    test_serialize();
    test_reflection_counts();
    test_reflection_first_error_via_executor();
    printf("test_planner: all cases passed\n");
    return 0;
}
