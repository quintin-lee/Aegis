/**
 * @file test_scheduler.c
 * @brief Unit tests for the task scheduler: lifecycle, ordering,
 *        dependency gating, duplicate-dispatch prevention, policy hook.
 */
#include "aegis/scheduler.h"
#include "aegis/task.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* ── Helpers ───────────────────────────────────────────────────────────────── */

static aegis_task_t* make_task(aegis_task_graph_t* g, const char* name)
{
    aegis_task_t* t = NULL;
    assert(aegis_task_create(&t, name, NULL) == AEGIS_OK);
    assert(aegis_task_graph_add_task(g, t) == AEGIS_OK);
    return t;
}

/* ── Lifecycle ─────────────────────────────────────────────────────────────── */

static void test_lifecycle(void)
{
    aegis_scheduler_t* s = NULL;
    assert(aegis_scheduler_create(&s) == AEGIS_OK);
    assert(s != NULL);
    aegis_scheduler_destroy(s);
    aegis_scheduler_destroy(NULL);
    assert(aegis_scheduler_create(NULL) == AEGIS_ERR_INVALID);

    assert(aegis_scheduler_pending_count(NULL) == 0);
    assert(aegis_scheduler_inflight_count(NULL) == 0);
}

static void test_attach_validation(void)
{
    aegis_scheduler_t* s = NULL;
    aegis_task_graph_t* g = NULL;
    assert(aegis_scheduler_create(&s) == AEGIS_OK);
    assert(aegis_task_graph_create(&g) == AEGIS_OK);

    assert(aegis_scheduler_attach(NULL, g) == AEGIS_ERR_INVALID);
    assert(aegis_scheduler_attach(s, NULL) == AEGIS_ERR_INVALID);

    /* Poll before attach → INVALID */
    size_t n = 99;
    assert(aegis_scheduler_poll(s, &n) == AEGIS_ERR_INVALID);
    assert(n == 0);

    assert(aegis_scheduler_attach(s, g) == AEGIS_OK);
    assert(aegis_scheduler_attach(s, g) == AEGIS_ERR_BUSY); /* already bound */

    aegis_scheduler_destroy(s);
    aegis_task_graph_destroy(g);
}

static void test_next_empty(void)
{
    aegis_scheduler_t* s = NULL;
    aegis_task_graph_t* g = NULL;
    assert(aegis_scheduler_create(&s) == AEGIS_OK);
    assert(aegis_task_graph_create(&g) == AEGIS_OK);
    assert(aegis_scheduler_attach(s, g) == AEGIS_OK);

    aegis_task_t* t = NULL;
    assert(aegis_scheduler_next(s, &t) == AEGIS_ERR_NOT_FOUND);
    assert(t == NULL);
    assert(aegis_scheduler_next(s, NULL) == AEGIS_ERR_INVALID);
    assert(aegis_scheduler_next(NULL, &t) == AEGIS_ERR_INVALID);
    assert(aegis_scheduler_poll(NULL, NULL) == AEGIS_ERR_INVALID);
    assert(aegis_scheduler_notify_complete(s, NULL) == AEGIS_ERR_INVALID);

    aegis_scheduler_destroy(s);
    aegis_task_graph_destroy(g);
}

/* ── Dispatch lifecycle + duplicate prevention ─────────────────────────────── */

static void test_single_task_dispatch_cycle(void)
{
    aegis_scheduler_t* s = NULL;
    aegis_task_graph_t* g = NULL;
    assert(aegis_scheduler_create(&s) == AEGIS_OK);
    assert(aegis_task_graph_create(&g) == AEGIS_OK);
    assert(aegis_scheduler_attach(s, g) == AEGIS_OK);

    aegis_task_t* a = make_task(g, "a");
    assert(aegis_task_state(a) == AEGIS_TASK_PENDING);

    /* No dependencies → promoted to READY and enqueued on first poll. */
    size_t enqueued = 0;
    assert(aegis_scheduler_poll(s, &enqueued) == AEGIS_OK);
    assert(enqueued == 1);
    assert(aegis_task_state(a) == AEGIS_TASK_READY);
    assert(aegis_scheduler_pending_count(s) == 1);

    /* Re-poll must not enqueue duplicates. */
    assert(aegis_scheduler_poll(s, &enqueued) == AEGIS_OK);
    assert(enqueued == 0);
    assert(aegis_scheduler_pending_count(s) == 1);

    /* Hand out. */
    aegis_task_t* got = NULL;
    assert(aegis_scheduler_next(s, &got) == AEGIS_OK);
    assert(got == a);
    assert(aegis_scheduler_inflight_count(s) == 1);
    assert(aegis_scheduler_pending_count(s) == 0);

    /* In-flight tasks are never re-dispatched. */
    assert(aegis_scheduler_next(s, &got) == AEGIS_ERR_NOT_FOUND);
    assert(aegis_scheduler_poll(s, &enqueued) == AEGIS_OK);
    assert(enqueued == 0);
    assert(aegis_scheduler_next(s, &got) == AEGIS_ERR_NOT_FOUND);

    /* Simulate executor: RUNNING → SUCCESS, then report completion. */
    aegis_task_set_state_for_test(a, AEGIS_TASK_RUNNING);
    assert(aegis_task_state(a) == AEGIS_TASK_RUNNING);
    aegis_task_set_state_for_test(a, AEGIS_TASK_SUCCESS);

    assert(aegis_scheduler_notify_complete(s, a) == AEGIS_OK);
    assert(aegis_scheduler_inflight_count(s) == 0);

    /* Terminal task is never re-scheduled. */
    assert(aegis_scheduler_poll(s, &enqueued) == AEGIS_OK);
    assert(enqueued == 0);
    assert(aegis_scheduler_next(s, &got) == AEGIS_ERR_NOT_FOUND);

    /* Double completion → NOT_FOUND. */
    assert(aegis_scheduler_notify_complete(s, a) == AEGIS_ERR_NOT_FOUND);

    aegis_scheduler_destroy(s);
    aegis_task_graph_destroy(g);
}

/* ── Ordering: Dependency → Priority → FIFO ────────────────────────────────── */

static void test_priority_order(void)
{
    aegis_scheduler_t* s = NULL;
    aegis_task_graph_t* g = NULL;
    assert(aegis_scheduler_create(&s) == AEGIS_OK);
    assert(aegis_task_graph_create(&g) == AEGIS_OK);
    assert(aegis_scheduler_attach(s, g) == AEGIS_OK);

    aegis_task_t* lo  = make_task(g, "lo");
    aegis_task_t* hi  = make_task(g, "hi");
    aegis_task_t* mid = make_task(g, "mid");
    aegis_task_set_priority(lo, 1);
    aegis_task_set_priority(hi, 10);
    aegis_task_set_priority(mid, 5);

    size_t enqueued = 0;
    assert(aegis_scheduler_poll(s, &enqueued) == AEGIS_OK);
    assert(enqueued == 3);

    const char* expected[] = {"hi", "mid", "lo"};
    for (int i = 0; i < 3; i++) {
        aegis_task_t* t = NULL;
        assert(aegis_scheduler_next(s, &t) == AEGIS_OK);
        assert(strcmp(aegis_task_name(t), expected[i]) == 0);
        aegis_task_set_state_for_test(t, AEGIS_TASK_SUCCESS);
        assert(aegis_scheduler_notify_complete(s, t) == AEGIS_OK);
    }
    aegis_task_t* t = NULL;
    assert(aegis_scheduler_next(s, &t) == AEGIS_ERR_NOT_FOUND);

    aegis_scheduler_destroy(s);
    aegis_task_graph_destroy(g);
}

static void test_fifo_tiebreak(void)
{
    aegis_scheduler_t* s = NULL;
    aegis_task_graph_t* g = NULL;
    assert(aegis_scheduler_create(&s) == AEGIS_OK);
    assert(aegis_task_graph_create(&g) == AEGIS_OK);
    assert(aegis_scheduler_attach(s, g) == AEGIS_OK);

    enum { N = 8 };
    aegis_task_t* tasks[N];
    for (int i = 0; i < N; i++) {
        char name[16];
        snprintf(name, sizeof(name), "fifo%d", i);
        tasks[i] = make_task(g, name);
        aegis_task_set_priority(tasks[i], 7); /* identical priorities */
    }

    size_t enqueued = 0;
    assert(aegis_scheduler_poll(s, &enqueued) == AEGIS_OK);
    assert(enqueued == N);

    for (int i = 0; i < N; i++) {
        aegis_task_t* t = NULL;
        assert(aegis_scheduler_next(s, &t) == AEGIS_OK);
        assert(t == tasks[i]); /* strict enqueue order */
        aegis_task_set_state_for_test(t, AEGIS_TASK_SUCCESS);
        assert(aegis_scheduler_notify_complete(s, t) == AEGIS_OK);
    }

    aegis_scheduler_destroy(s);
    aegis_task_graph_destroy(g);
}

/* ── Dependency gating ─────────────────────────────────────────────────────── */

static void test_dependency_gating_chain(void)
{
    aegis_scheduler_t* s = NULL;
    aegis_task_graph_t* g = NULL;
    assert(aegis_scheduler_create(&s) == AEGIS_OK);
    assert(aegis_task_graph_create(&g) == AEGIS_OK);
    assert(aegis_scheduler_attach(s, g) == AEGIS_OK);

    aegis_task_t* a = make_task(g, "first");
    aegis_task_t* b = make_task(g, "second");
    assert(aegis_task_graph_add_dependency(g, a, b) == AEGIS_OK); /* b depends on a */

    size_t enqueued = 0;

    /* Only `a` is schedulable; `b` is gated. */
    assert(aegis_scheduler_poll(s, &enqueued) == AEGIS_OK);
    assert(enqueued == 1);
    assert(aegis_scheduler_pending_count(s) == 1);

    aegis_task_t* got = NULL;
    assert(aegis_scheduler_next(s, &got) == AEGIS_OK);
    assert(got == a);
    assert(aegis_scheduler_next(s, &got) == AEGIS_ERR_NOT_FOUND);

    /* While `a` runs, repeated polls keep `b` gated. */
    aegis_task_set_state_for_test(a, AEGIS_TASK_RUNNING);
    assert(aegis_scheduler_poll(s, &enqueued) == AEGIS_OK);
    assert(enqueued == 0);
    assert(aegis_scheduler_next(s, &got) == AEGIS_ERR_NOT_FOUND);

    /* Complete `a` → `b` becomes schedulable on the next poll. */
    aegis_task_set_state_for_test(a, AEGIS_TASK_SUCCESS);
    assert(aegis_scheduler_notify_complete(s, a) == AEGIS_OK);
    assert(aegis_scheduler_poll(s, &enqueued) == AEGIS_OK);
    assert(enqueued == 1);
    assert(aegis_task_state(b) == AEGIS_TASK_READY); /* promoted */

    assert(aegis_scheduler_next(s, &got) == AEGIS_OK);
    assert(got == b);
    aegis_task_set_state_for_test(b, AEGIS_TASK_SUCCESS);
    assert(aegis_scheduler_notify_complete(s, b) == AEGIS_OK);

    aegis_scheduler_destroy(s);
    aegis_task_graph_destroy(g);
}

static void test_failed_source_blocks_dependent(void)
{
    aegis_scheduler_t* s = NULL;
    aegis_task_graph_t* g = NULL;
    assert(aegis_scheduler_create(&s) == AEGIS_OK);
    assert(aegis_task_graph_create(&g) == AEGIS_OK);
    assert(aegis_scheduler_attach(s, g) == AEGIS_OK);

    aegis_task_t* a = make_task(g, "doomed");
    aegis_task_t* b = make_task(g, "dependent");
    assert(aegis_task_graph_add_dependency(g, a, b) == AEGIS_OK);

    /* `a` fails; failure propagation is the replanner's job, so `b`
     * must stay permanently gated in v1. */
    aegis_task_set_state_for_test(a, AEGIS_TASK_FAILED);

    size_t enqueued = 99;
    assert(aegis_scheduler_poll(s, &enqueued) == AEGIS_OK);
    assert(enqueued == 0);
    assert(aegis_task_state(b) == AEGIS_TASK_PENDING);

    aegis_scheduler_destroy(s);
    aegis_task_graph_destroy(g);
}

static void test_skipped_source_satisfies_dependent(void)
{
    aegis_scheduler_t* s = NULL;
    aegis_task_graph_t* g = NULL;
    assert(aegis_scheduler_create(&s) == AEGIS_OK);
    assert(aegis_task_graph_create(&g) == AEGIS_OK);
    assert(aegis_scheduler_attach(s, g) == AEGIS_OK);

    aegis_task_t* a = make_task(g, "skipped_src");
    aegis_task_t* b = make_task(g, "after_skip");
    assert(aegis_task_graph_add_dependency(g, a, b) == AEGIS_OK);

    aegis_task_set_state_for_test(a, AEGIS_TASK_SKIPPED);

    size_t enqueued = 0;
    assert(aegis_scheduler_poll(s, &enqueued) == AEGIS_OK);
    assert(enqueued == 1);

    aegis_task_t* got = NULL;
    assert(aegis_scheduler_next(s, &got) == AEGIS_OK);
    assert(got == b);

    aegis_scheduler_destroy(s);
    aegis_task_graph_destroy(g);
}

/* ── Stale-entry dropping ──────────────────────────────────────────────────── */

static void test_stale_entry_dropped(void)
{
    aegis_scheduler_t* s = NULL;
    aegis_task_graph_t* g = NULL;
    assert(aegis_scheduler_create(&s) == AEGIS_OK);
    assert(aegis_task_graph_create(&g) == AEGIS_OK);
    assert(aegis_scheduler_attach(s, g) == AEGIS_OK);

    aegis_task_t* a = make_task(g, "cancelled_while_queued");

    size_t enqueued = 0;
    assert(aegis_scheduler_poll(s, &enqueued) == AEGIS_OK);
    assert(enqueued == 1);
    assert(aegis_scheduler_pending_count(s) == 1);

    /* Cancel externally while queued. */
    aegis_task_set_state_for_test(a, AEGIS_TASK_CANCELLED);

    aegis_task_t* got = (aegis_task_t*)0x1;
    assert(aegis_scheduler_next(s, &got) == AEGIS_ERR_NOT_FOUND);
    assert(got == NULL);
    assert(aegis_scheduler_pending_count(s) == 0);

    aegis_scheduler_destroy(s);
    aegis_task_graph_destroy(g);
}

/* ── Custom policy hook ────────────────────────────────────────────────────── */

static int reverse_priority_cmp(const aegis_task_t* lhs, const aegis_task_t* rhs, void* user)
{
    (void)user;
    /* Lower priority value dispatches first (reversed). */
    return aegis_task_priority(rhs) - aegis_task_priority(lhs);
}

static void test_custom_policy(void)
{
    aegis_scheduler_t* s = NULL;
    aegis_task_graph_t* g = NULL;
    assert(aegis_scheduler_create(&s) == AEGIS_OK);
    assert(aegis_task_graph_create(&g) == AEGIS_OK);
    assert(aegis_scheduler_attach(s, g) == AEGIS_OK);

    aegis_task_t* lo  = make_task(g, "p1");
    aegis_task_t* hi  = make_task(g, "p10");
    aegis_task_set_priority(lo, 1);
    aegis_task_set_priority(hi, 10);

    assert(aegis_scheduler_set_policy(s, reverse_priority_cmp, NULL) == AEGIS_OK);
    assert(aegis_scheduler_set_policy(NULL, NULL, NULL) == AEGIS_ERR_INVALID);

    size_t enqueued = 0;
    assert(aegis_scheduler_poll(s, &enqueued) == AEGIS_OK);
    assert(enqueued == 2);

    /* Reversed policy: p1 before p10. */
    aegis_task_t* t = NULL;
    assert(aegis_scheduler_next(s, &t) == AEGIS_OK);
    assert(t == lo);
    aegis_task_set_state_for_test(t, AEGIS_TASK_SUCCESS);
    assert(aegis_scheduler_notify_complete(s, t) == AEGIS_OK);
    assert(aegis_scheduler_next(s, &t) == AEGIS_OK);
    assert(t == hi);

    /* Restore the default policy. */
    assert(aegis_scheduler_set_policy(s, NULL, NULL) == AEGIS_OK);

    aegis_scheduler_destroy(s);
    aegis_task_graph_destroy(g);
}

/* ── Notify without dispatch ───────────────────────────────────────────────── */

static void test_notify_unknown_task(void)
{
    aegis_scheduler_t* s = NULL;
    aegis_task_graph_t* g = NULL;
    assert(aegis_scheduler_create(&s) == AEGIS_OK);
    assert(aegis_task_graph_create(&g) == AEGIS_OK);
    assert(aegis_scheduler_attach(s, g) == AEGIS_OK);

    aegis_task_t* a = make_task(g, "never_dispatched");

    assert(aegis_scheduler_notify_complete(s, a) == AEGIS_ERR_NOT_FOUND);

    aegis_scheduler_destroy(s);
    aegis_task_graph_destroy(g);
}

int main(void)
{
    test_lifecycle();
    test_attach_validation();
    test_next_empty();
    test_single_task_dispatch_cycle();
    test_priority_order();
    test_fifo_tiebreak();
    test_dependency_gating_chain();
    test_failed_source_blocks_dependent();
    test_skipped_source_satisfies_dependent();
    test_stale_entry_dropped();
    test_custom_policy();
    test_notify_unknown_task();
    return 0;
}
