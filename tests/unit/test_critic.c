/**
 * @file test_critic.c
 * @brief Unit tests for the Critic module:
 *   - SUCCESS  : all tasks succeeded
 *   - PARTIAL  : mix of success and failure
 *   - FAILURE  : all tasks failed with no success
 *   - INVALID  : NULL graph, empty graph, NULL/empty goal
 *   - REPLAN_REQUIRED : cancellation/skipped with incomplete work, or terminal
 *                       state but unsatisfactory outcome
 */
#include "aegis/cancellation.h"
#include "aegis/critic.h"
#include "aegis/graph.h"
#include "aegis/plan.h"
#include "aegis/task.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Helpers ───────────────────────────────────────────────────────────────── */

static aegis_task_graph_t* make_graph_with_states(aegis_task_state_t state, size_t count)
{
    aegis_task_graph_t* g = NULL;
    assert(aegis_task_graph_create(&g) == AEGIS_OK);
    for (size_t i = 0; i < count; i++) {
        aegis_task_t* t = NULL;
        assert(aegis_task_create(&t, "step", "desc") == AEGIS_OK);
        aegis_task_set_state_for_test(t, state);
        assert(aegis_task_graph_add_task(g, t) == AEGIS_OK);
        /* Caller-owned task is now owned by graph; do NOT destroy t here. */
    }
    return g;
}

static void check(const aegis_critic_t* critic, const char* goal, const aegis_plan_t* plan,
                  const aegis_task_graph_t* graph, const aegis_cancellation_token_t* token,
                  aegis_critique_result_t expected, const char* tag)
{
    aegis_critique_t q  = {0};
    aegis_status_t   rc = aegis_critic_evaluate(critic, goal, plan, graph, token, &q);
    assert(rc == AEGIS_OK);
    assert(q.result == expected);
    (void)tag; /* tag used for human reading if assert fires */
}

/* ── INVALID cases ─────────────────────────────────────────────────────────── */

static void test_invalid_null_goal(void)
{
    aegis_critic_t* c = NULL;
    assert(aegis_critic_create(&c) == AEGIS_OK);
    aegis_task_graph_t* g = make_graph_with_states(AEGIS_TASK_SUCCESS, 1);

    aegis_critique_t q = {0};
    assert(aegis_critic_evaluate(c, NULL, NULL, g, NULL, &q) == AEGIS_OK);
    assert(q.result == AEGIS_CRITIQUE_INVALID);
    assert(q.feedback != NULL);

    aegis_task_graph_destroy(g);
    aegis_critic_destroy(c);
}

static void test_invalid_empty_goal(void)
{
    aegis_critic_t* c = NULL;
    assert(aegis_critic_create(&c) == AEGIS_OK);
    aegis_task_graph_t* g = make_graph_with_states(AEGIS_TASK_SUCCESS, 1);

    aegis_critique_t q = {0};
    assert(aegis_critic_evaluate(c, "", NULL, g, NULL, &q) == AEGIS_OK);
    assert(q.result == AEGIS_CRITIQUE_INVALID);

    aegis_task_graph_destroy(g);
    aegis_critic_destroy(c);
}

static void test_invalid_null_graph(void)
{
    aegis_critic_t* c = NULL;
    assert(aegis_critic_create(&c) == AEGIS_OK);

    aegis_critique_t q = {0};
    assert(aegis_critic_evaluate(c, "goal", NULL, NULL, NULL, &q) == AEGIS_OK);
    assert(q.result == AEGIS_CRITIQUE_INVALID);

    aegis_critic_destroy(c);
}

static void test_invalid_empty_graph(void)
{
    aegis_critic_t* c = NULL;
    assert(aegis_critic_create(&c) == AEGIS_OK);
    aegis_task_graph_t* g = NULL;
    assert(aegis_task_graph_create(&g) == AEGIS_OK);

    aegis_critique_t q = {0};
    assert(aegis_critic_evaluate(c, "goal", NULL, g, NULL, &q) == AEGIS_OK);
    assert(q.result == AEGIS_CRITIQUE_INVALID);

    aegis_task_graph_destroy(g);
    aegis_critic_destroy(c);
}

/* ── SUCCESS case ───────────────────────────────────────────────────────────── */

static void test_success_all_succeeded(void)
{
    aegis_critic_t* c = NULL;
    assert(aegis_critic_create(&c) == AEGIS_OK);
    aegis_task_graph_t* g = make_graph_with_states(AEGIS_TASK_SUCCESS, 3);
    aegis_plan_t*       p = NULL;
    assert(aegis_plan_create(&p, "build house") == AEGIS_OK);

    check(c, "build house", p, g, NULL, AEGIS_CRITIQUE_SUCCESS, "all-success");

    aegis_plan_destroy(p);
    aegis_task_graph_destroy(g);
    aegis_critic_destroy(c);
}

/* ── PARTIAL case ───────────────────────────────────────────────────────────── */

static void test_partial_mix_success_and_failure(void)
{
    aegis_critic_t* c = NULL;
    assert(aegis_critic_create(&c) == AEGIS_OK);

    /* Build a mixed graph: 2 success, 1 failed. */
    aegis_task_graph_t* g = NULL;
    assert(aegis_task_graph_create(&g) == AEGIS_OK);
    for (int i = 0; i < 2; i++) {
        aegis_task_t* t = NULL;
        assert(aegis_task_create(&t, "ok", NULL) == AEGIS_OK);
        aegis_task_set_state_for_test(t, AEGIS_TASK_SUCCESS);
        assert(aegis_task_graph_add_task(g, t) == AEGIS_OK);
    }
    {
        aegis_task_t* t = NULL;
        assert(aegis_task_create(&t, "fail", NULL) == AEGIS_OK);
        aegis_task_set_state_for_test(t, AEGIS_TASK_FAILED);
        assert(aegis_task_graph_add_task(g, t) == AEGIS_OK);
    }

    aegis_plan_t* p = NULL;
    assert(aegis_plan_create(&p, "do work") == AEGIS_OK);

    aegis_critique_t q = {0};
    assert(aegis_critic_evaluate(c, "do work", p, g, NULL, &q) == AEGIS_OK);
    assert(q.result == AEGIS_CRITIQUE_PARTIAL);
    assert(q.feedback != NULL && q.feedback[0] != '\0');
    /* Should mention success count. */
    assert(strstr(q.feedback, "2/3") != NULL);

    aegis_plan_destroy(p);
    aegis_task_graph_destroy(g);
    aegis_critic_destroy(c);
}

/* ── FAILURE case ───────────────────────────────────────────────────────────── */

static void test_failure_all_failed(void)
{
    aegis_critic_t* c = NULL;
    assert(aegis_critic_create(&c) == AEGIS_OK);
    aegis_task_graph_t* g = make_graph_with_states(AEGIS_TASK_FAILED, 2);
    aegis_plan_t*       p = NULL;
    assert(aegis_plan_create(&p, "impossible") == AEGIS_OK);

    check(c, "impossible", p, g, NULL, AEGIS_CRITIQUE_FAILURE, "all-failed");

    aegis_plan_destroy(p);
    aegis_task_graph_destroy(g);
    aegis_critic_destroy(c);
}

/* ── REPLAN_REQUIRED cases ─────────────────────────────────────────────────── */

static void test_replan_required_with_incomplete(void)
{
    /* In-flight: some done, some still pending (not all terminal). */
    aegis_critic_t* c = NULL;
    assert(aegis_critic_create(&c) == AEGIS_OK);

    aegis_task_graph_t* g = NULL;
    assert(aegis_task_graph_create(&g) == AEGIS_OK);
    {
        aegis_task_t* t = NULL;
        assert(aegis_task_create(&t, "done", NULL) == AEGIS_OK);
        aegis_task_set_state_for_test(t, AEGIS_TASK_SUCCESS);
        assert(aegis_task_graph_add_task(g, t) == AEGIS_OK);
    }
    {
        aegis_task_t* t = NULL;
        assert(aegis_task_create(&t, "pending", NULL) == AEGIS_OK);
        aegis_task_set_state_for_test(t, AEGIS_TASK_PENDING);
        assert(aegis_task_graph_add_task(g, t) == AEGIS_OK);
    }

    aegis_plan_t* p = NULL;
    assert(aegis_plan_create(&p, "goal") == AEGIS_OK);

    aegis_critique_t q = {0};
    assert(aegis_critic_evaluate(c, "goal", p, g, NULL, &q) == AEGIS_OK);
    assert(q.result == AEGIS_CRITIQUE_REPLAN_REQUIRED);
    assert(q.feedback != NULL);

    aegis_plan_destroy(p);
    aegis_task_graph_destroy(g);
    aegis_critic_destroy(c);
}

static void test_replan_required_cancelled_with_pending(void)
{
    /* All terminal but cancelled task signals abort — planner should retry. */
    aegis_critic_t* c = NULL;
    assert(aegis_critic_create(&c) == AEGIS_OK);

    aegis_task_graph_t* g = NULL;
    assert(aegis_task_graph_create(&g) == AEGIS_OK);
    {
        aegis_task_t* t = NULL;
        assert(aegis_task_create(&t, "ok", NULL) == AEGIS_OK);
        aegis_task_set_state_for_test(t, AEGIS_TASK_SUCCESS);
        assert(aegis_task_graph_add_task(g, t) == AEGIS_OK);
    }
    {
        aegis_task_t* t = NULL;
        assert(aegis_task_create(&t, "cancel", NULL) == AEGIS_OK);
        aegis_task_set_state_for_test(t, AEGIS_TASK_CANCELLED);
        assert(aegis_task_graph_add_task(g, t) == AEGIS_OK);
    }

    aegis_plan_t* p = NULL;
    assert(aegis_plan_create(&p, "goal") == AEGIS_OK);

    aegis_critique_t q = {0};
    assert(aegis_critic_evaluate(c, "goal", p, g, NULL, &q) == AEGIS_OK);
    assert(q.result == AEGIS_CRITIQUE_REPLAN_REQUIRED);

    aegis_plan_destroy(p);
    aegis_task_graph_destroy(g);
    aegis_critic_destroy(c);
}

/* ── Result string ─────────────────────────────────────────────────────────── */

static void test_result_str(void)
{
    assert(strcmp(aegis_critique_result_str(AEGIS_CRITIQUE_INVALID), "INVALID") == 0);
    assert(strcmp(aegis_critique_result_str(AEGIS_CRITIQUE_SUCCESS), "SUCCESS") == 0);
    assert(strcmp(aegis_critique_result_str(AEGIS_CRITIQUE_PARTIAL), "PARTIAL") == 0);
    assert(strcmp(aegis_critique_result_str(AEGIS_CRITIQUE_FAILURE), "FAILURE") == 0);
    assert(strcmp(aegis_critique_result_str(AEGIS_CRITIQUE_REPLAN_REQUIRED), "REPLAN_REQUIRED") ==
           0);
    assert(strcmp(aegis_critique_result_str((aegis_critique_result_t)99), "UNKNOWN") == 0);
}

/* ── NULL safety ──────────────────────────────────────────────────────────── */

static void test_null_safe_api(void)
{
    aegis_critic_destroy(NULL); /* no-op */
    assert(aegis_critic_create(NULL) == AEGIS_ERR_INVALID);
}

/* ── Main ──────────────────────────────────────────────────────────────────── */

int main(void)
{
    test_null_safe_api();
    test_invalid_null_goal();
    test_invalid_empty_goal();
    test_invalid_null_graph();
    test_invalid_empty_graph();
    test_success_all_succeeded();
    test_partial_mix_success_and_failure();
    test_failure_all_failed();
    test_replan_required_with_incomplete();
    test_replan_required_cancelled_with_pending();
    test_result_str();

    printf("critic: all tests passed\n");
    return 0;
}
