/**
 * @file test_executor.c
 * @brief Unit tests for the executor: lifecycle, submit/cancel/wait,
 *        retry, timeout, cooperative cancellation, shutdown semantics.
 *
 * Tasks here are standalone (no graph): the executor holds borrowed
 * pointers only, and each test destroys its tasks after the executor
 * is gone — which also exercises destroy-time cleanup under ASan.
 */
#include "aegis/executor.h"
#include "aegis/task.h"
#include "aegis/common/time.h"

#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Helpers ───────────────────────────────────────────────────────────────── */

static aegis_task_t* make_task(const char* name)
{
    aegis_task_t* t = NULL;
    assert(aegis_task_create(&t, name, NULL) == AEGIS_OK);
    return t;
}

static aegis_executor_t* make_executor(unsigned workers)
{
    aegis_executor_t*       e   = NULL;
    aegis_executor_config_t cfg = {.worker_count = workers, .queue_capacity = 0};
    assert(aegis_executor_create(&e, &cfg) == AEGIS_OK);
    return e;
}

/** Set to 1 to release gated work functions. */
static atomic_int g_gate;

/**
 * Work that records it ran into *(atomic_int*)user, then spins on the
 * global gate (polling the token) before succeeding.
 */
static aegis_status_t gated_work(aegis_task_t* task, const aegis_cancellation_token_t* token,
                                 void* user)
{
    (void)task;
    if (user) {
        atomic_store((atomic_int*)user, 1);
    }
    while (!atomic_load(&g_gate)) {
        if (aegis_cancellation_token_is_cancelled(token)) {
            return AEGIS_ERR_CANCELLED;
        }
        aegis_sleep_ms(2);
    }
    return AEGIS_OK;
}

/** Work that always fails with @c *(int*)user as status. */
static aegis_status_t failing_work(aegis_task_t* task, const aegis_cancellation_token_t* token,
                                   void* user)
{
    (void)task;
    (void)token;
    return (aegis_status_t)(intptr_t)user;
}

/** Work that fails the first *(int*)user calls, then succeeds. */
static aegis_status_t flaky_work(aegis_task_t* task, const aegis_cancellation_token_t* token,
                                 void* user)
{
    (void)task;
    (void)token;
    int* remaining = user;
    if (*remaining > 0) {
        (*remaining)--;
        return AEGIS_ERR_PROVIDER;
    }
    return AEGIS_OK;
}

/** Work that sleeps in token-polled slices until released or cancelled. */
static aegis_status_t slow_polled_work(aegis_task_t* task, const aegis_cancellation_token_t* token,
                                       void* user)
{
    (void)task;
    if (user) {
        atomic_store((atomic_int*)user, 1);
    }
    for (;;) {
        if (aegis_cancellation_token_is_cancelled(token)) {
            return AEGIS_ERR_CANCELLED;
        }
        if (atomic_load(&g_gate)) {
            return AEGIS_OK;
        }
        aegis_sleep_ms(5);
    }
}

static void open_gate(void)
{
    atomic_store(&g_gate, 1);
}

/* ── Lifecycle & validation ────────────────────────────────────────────────── */

static void test_lifecycle(void)
{
    assert(aegis_executor_create(NULL, NULL) == AEGIS_ERR_INVALID);

    aegis_executor_t* e = NULL;
    assert(aegis_executor_create(&e, NULL) == AEGIS_OK); /* defaults */
    assert(e != NULL);
    assert(aegis_executor_pending_count(e) == 0);
    assert(aegis_executor_running_count(e) == 0);

    assert(aegis_executor_pending_count(NULL) == 0);
    assert(aegis_executor_running_count(NULL) == 0);

    aegis_executor_destroy(e);
    aegis_executor_destroy(NULL); /* no-op, must not crash */

    /* Explicit small pool. */
    e = make_executor(2);
    aegis_executor_destroy(e);
}

static void test_submit_validation(void)
{
    aegis_executor_t* e = make_executor(2);
    aegis_task_t*     t = make_task("validation");

    assert(aegis_executor_submit(NULL, t, gated_work, NULL) == AEGIS_ERR_INVALID);
    assert(aegis_executor_submit(e, NULL, gated_work, NULL) == AEGIS_ERR_INVALID);
    assert(aegis_executor_submit(e, t, NULL, NULL) == AEGIS_ERR_INVALID);
    assert(aegis_executor_cancel(NULL, 1) == AEGIS_ERR_INVALID);
    assert(aegis_executor_wait(NULL, 1, NULL, -1) == AEGIS_ERR_INVALID);
    assert(aegis_executor_shutdown(NULL, -1) == AEGIS_ERR_INVALID);

    assert(aegis_executor_cancel(e, 999999) == AEGIS_ERR_NOT_FOUND);
    assert(aegis_executor_wait(e, 999999, NULL, 0) == AEGIS_ERR_NOT_FOUND);

    /* Wrong initial state is rejected. */
    aegis_task_set_state_for_test(t, AEGIS_TASK_RUNNING);
    assert(aegis_executor_submit(e, t, gated_work, NULL) == AEGIS_ERR_BUSY);
    aegis_task_set_state_for_test(t, AEGIS_TASK_SUCCESS);
    assert(aegis_executor_submit(e, t, gated_work, NULL) == AEGIS_ERR_BUSY);

    aegis_executor_destroy(e);
    aegis_task_destroy(t);
}

/* ── Success path ──────────────────────────────────────────────────────────── */

static aegis_status_t output_work(aegis_task_t* task, const aegis_cancellation_token_t* token,
                                  void* user)
{
    (void)token;
    return aegis_task_set_output(task, user, strlen(user));
}

static void test_success_path(void)
{
    atomic_store(&g_gate, 1); /* nothing gated here */
    aegis_executor_t*   e = make_executor(2);
    aegis_task_t*       t = make_task("success");
    aegis_exec_result_t r = {0};

    assert(aegis_executor_submit(e, t, output_work, (void*)"done-data") == AEGIS_OK);
    assert(aegis_executor_wait(e, aegis_task_id(t), &r, 5000) == AEGIS_OK);

    assert(r.outcome == AEGIS_EXEC_COMPLETED);
    assert(r.status == AEGIS_OK);
    assert(r.attempts == 1);
    assert(r.duration_ns >= 0);
    assert(aegis_task_state(t) == AEGIS_TASK_SUCCESS);

    size_t      out_size = 0;
    const void* out      = aegis_task_output(t, &out_size);
    assert(out && out_size == strlen("done-data"));
    assert(memcmp(out, "done-data", out_size) == 0);

    /* Result reaped exactly once. */
    assert(aegis_executor_wait(e, aegis_task_id(t), &r, 100) == AEGIS_ERR_NOT_FOUND);
    assert(aegis_executor_cancel(e, aegis_task_id(t)) == AEGIS_ERR_NOT_FOUND);

    aegis_executor_destroy(e);
    aegis_task_destroy(t);
}

/* ── Failure & retry ──────────────────────────────────────────────────────── */

static void test_failure_no_retry(void)
{
    aegis_executor_t*   e  = make_executor(1);
    aegis_task_t*       t  = make_task("fail-fast");
    aegis_exec_result_t r  = {0};
    const int           rc = AEGIS_ERR_TOOL;

    aegis_task_set_retry_policy(t, (aegis_task_retry_policy_t){.max_attempts = 0, .delay_ms = 0});

    assert(aegis_executor_submit(e, t, failing_work, (void*)(intptr_t)rc) == AEGIS_OK);
    assert(aegis_executor_wait(e, aegis_task_id(t), &r, 5000) == AEGIS_OK);

    assert(r.outcome == AEGIS_EXEC_FAILED);
    assert(r.status == rc);
    assert(r.attempts == 1);
    assert(aegis_task_state(t) == AEGIS_TASK_FAILED);
    assert(aegis_task_error(t) != NULL);

    aegis_executor_destroy(e);
    aegis_task_destroy(t);
}

static void test_retry_exhausted(void)
{
    aegis_executor_t*   e = make_executor(1);
    aegis_task_t*       t = make_task("retry-exhausted");
    aegis_exec_result_t r = {0};

    aegis_task_set_retry_policy(t, (aegis_task_retry_policy_t){.max_attempts = 2, .delay_ms = 1});

    assert(aegis_executor_submit(e, t, failing_work, (void*)(intptr_t)AEGIS_ERR_INTERNAL) ==
           AEGIS_OK);
    assert(aegis_executor_wait(e, aegis_task_id(t), &r, 5000) == AEGIS_OK);

    /* retries=2 → three total attempts. */
    assert(r.outcome == AEGIS_EXEC_FAILED);
    assert(r.status == AEGIS_ERR_INTERNAL);
    assert(r.attempts == 3);
    assert(aegis_task_state(t) == AEGIS_TASK_FAILED);

    aegis_executor_destroy(e);
    aegis_task_destroy(t);
}

static void test_retry_then_success(void)
{
    aegis_executor_t*   e         = make_executor(1);
    aegis_task_t*       t         = make_task("retry-recovers");
    aegis_exec_result_t r         = {0};
    int                 remaining = 2;

    aegis_task_set_retry_policy(
        t,
        (aegis_task_retry_policy_t){.max_attempts = 3, .delay_ms = 1, .exponential_backoff = true});

    assert(aegis_executor_submit(e, t, flaky_work, &remaining) == AEGIS_OK);
    assert(aegis_executor_wait(e, aegis_task_id(t), &r, 5000) == AEGIS_OK);

    assert(r.outcome == AEGIS_EXEC_COMPLETED);
    assert(r.status == AEGIS_OK);
    assert(r.attempts == 3); /* two failures, then success */
    assert(remaining == 0);
    assert(aegis_task_state(t) == AEGIS_TASK_SUCCESS);

    aegis_executor_destroy(e);
    aegis_task_destroy(t);
}

/* ── Timeout ──────────────────────────────────────────────────────────────── */

static void test_timeout_not_retried(void)
{
    aegis_executor_t*   e = make_executor(1);
    aegis_task_t*       t = make_task("timeout");
    aegis_exec_result_t r = {0};
    atomic_store(&g_gate, 0);

    /* Retries configured: must NOT apply after a timeout. */
    aegis_task_set_retry_policy(t, (aegis_task_retry_policy_t){.max_attempts = 3, .delay_ms = 1});
    aegis_task_set_timeout_ms(t, 40);

    atomic_int ran;
    atomic_init(&ran, 0);
    assert(aegis_executor_submit(e, t, slow_polled_work, &ran) == AEGIS_OK);
    assert(aegis_executor_wait(e, aegis_task_id(t), &r, 5000) == AEGIS_OK);

    assert(r.outcome == AEGIS_EXEC_TIMED_OUT);
    assert(r.status == AEGIS_ERR_TIMEOUT);
    assert(r.attempts == 1); /* timed-out attempts are never retried */
    assert(aegis_task_state(t) == AEGIS_TASK_FAILED);

    aegis_executor_destroy(e);
    aegis_task_destroy(t);
}

/* ── Cancellation ─────────────────────────────────────────────────────────── */

static void test_cancel_queued_never_runs(void)
{
    aegis_executor_t*   e       = make_executor(1); /* single worker: second job queues */
    aegis_task_t*       blocker = make_task("blocker");
    aegis_task_t*       victim  = make_task("queued-victim");
    aegis_exec_result_t rb = {0}, rv = {0};
    atomic_store(&g_gate, 0);

    atomic_int ran_blocker, ran_victim;
    atomic_init(&ran_blocker, 0);
    atomic_init(&ran_victim, 0);

    assert(aegis_executor_submit(e, blocker, gated_work, &ran_blocker) == AEGIS_OK);
    /* Wait until the blocker is actually running so the victim surely
     * queues behind it. */
    while (!atomic_load(&ran_blocker)) {
        aegis_sleep_ms(2);
    }
    assert(aegis_executor_submit(e, victim, gated_work, &ran_victim) == AEGIS_OK);

    assert(aegis_executor_cancel(e, aegis_task_id(victim)) == AEGIS_OK);
    /* Double cancel while still queued is fine (idempotent request). */
    assert(aegis_executor_cancel(e, aegis_task_id(victim)) == AEGIS_OK);

    open_gate();
    assert(aegis_executor_wait(e, aegis_task_id(victim), &rv, 5000) == AEGIS_OK);
    assert(aegis_executor_wait(e, aegis_task_id(blocker), &rb, 5000) == AEGIS_OK);

    /* Victim finished CANCELLED without its function ever running. */
    assert(rv.outcome == AEGIS_EXEC_CANCELLED);
    assert(rv.status == AEGIS_ERR_CANCELLED);
    assert(rv.attempts == 0);
    assert(atomic_load(&ran_victim) == 0);
    assert(aegis_task_state(victim) == AEGIS_TASK_CANCELLED);

    assert(rb.outcome == AEGIS_EXEC_COMPLETED);
    assert(atomic_load(&ran_blocker) == 1);
    assert(aegis_task_state(blocker) == AEGIS_TASK_SUCCESS);

    aegis_executor_destroy(e);
    aegis_task_destroy(blocker);
    aegis_task_destroy(victim);
}

static void test_cancel_running_cooperative(void)
{
    aegis_executor_t*   e = make_executor(1);
    aegis_task_t*       t = make_task("running-cancel");
    aegis_exec_result_t r = {0};
    atomic_store(&g_gate, 0); /* work runs until cancelled */

    atomic_int ran;
    atomic_init(&ran, 0);
    assert(aegis_executor_submit(e, t, slow_polled_work, &ran) == AEGIS_OK);

    /* Wait until it actually started, then cancel. */
    while (!atomic_load(&ran)) {
        aegis_sleep_ms(2);
    }
    assert(aegis_executor_cancel(e, aegis_task_id(t)) == AEGIS_OK);

    assert(aegis_executor_wait(e, aegis_task_id(t), &r, 5000) == AEGIS_OK);
    assert(r.outcome == AEGIS_EXEC_CANCELLED);
    assert(r.status == AEGIS_ERR_CANCELLED); /* work returned ERR_CANCELLED honoring token */
    assert(aegis_task_state(t) == AEGIS_TASK_CANCELLED);

    aegis_executor_destroy(e);
    aegis_task_destroy(t);
}

/* Ignores the cancellation token entirely: cancel during run returns OK but
 * must not change the outcome. Used to observe FINISHED-before-reap
 * deterministically. */
static int stubborn_slow_work(aegis_task_t* t, const aegis_cancellation_token_t* tok, void* user)
{
    (void)t;
    (void)tok;
    (void)user;
    for (int i = 0; i < 10; i++) {
        aegis_sleep_ms(5);
    }
    return AEGIS_OK;
}

static void test_cancel_after_finish_is_busy(void)
{
    aegis_executor_t* e = make_executor(1);
    aegis_task_t*     t = make_task("finish-then-cancel");

    assert(aegis_executor_submit(e, t, stubborn_slow_work, NULL) == AEGIS_OK);

    /* Deterministic finish detection: no polling cancel here — a mid-run
     * cancel would trip the USER flag and (for token-honoring work) flip the
     * outcome. Wait until all work drained instead. */
    bool finished = false;
    for (int i = 0; i < 5000 && !finished; i++) {
        if (aegis_executor_running_count(e) == 0 && aegis_executor_pending_count(e) == 0) {
            finished = true;
        } else {
            aegis_sleep_ms(1);
        }
    }
    assert(finished);

    /* Finished but unreaped → BUSY. */
    assert(aegis_executor_cancel(e, aegis_task_id(t)) == AEGIS_ERR_BUSY);

    aegis_exec_result_t r = {0};
    assert(aegis_executor_wait(e, aegis_task_id(t), &r, 5000) == AEGIS_OK);
    assert(r.outcome == AEGIS_EXEC_COMPLETED);

    /* After reap the id is unknown again. */
    assert(aegis_executor_cancel(e, aegis_task_id(t)) == AEGIS_ERR_NOT_FOUND);

    aegis_executor_destroy(e);
    aegis_task_destroy(t);
}

/* ── Duplicate submission / queue capacity ────────────────────────────────── */

static void test_duplicate_and_queue_full(void)
{
    static atomic_int       ran_a;
    aegis_executor_t*       e   = NULL;
    aegis_executor_config_t cfg = {.worker_count = 1, .queue_capacity = 1};
    assert(aegis_executor_create(&e, &cfg) == AEGIS_OK);

    aegis_task_t* a = make_task("dup-a");
    aegis_task_t* b = make_task("dup-b");
    aegis_task_t* c = make_task("queue-c");
    atomic_store(&g_gate, 0);
    atomic_store(&ran_a, 0);

    assert(aegis_executor_submit(e, a, gated_work, &ran_a) == AEGIS_OK);
    /* Deterministic setup: wait until `a` occupies the only worker. */
    while (!atomic_load(&ran_a)) {
        aegis_sleep_ms(2);
    }
    assert(aegis_executor_submit(e, b, gated_work, NULL) == AEGIS_OK);       /* queued  */
    assert(aegis_executor_submit(e, c, gated_work, NULL) == AEGIS_ERR_BUSY); /* full */
    assert(aegis_executor_submit(e, a, gated_work, NULL) == AEGIS_ERR_BUSY); /* dup */

    open_gate();
    aegis_exec_result_t r = {0};
    assert(aegis_executor_wait(e, aegis_task_id(a), &r, 5000) == AEGIS_OK);
    assert(aegis_executor_wait(e, aegis_task_id(b), &r, 5000) == AEGIS_OK);

    aegis_executor_destroy(e);
    aegis_task_destroy(a);
    aegis_task_destroy(b);
    aegis_task_destroy(c);
}

/* ── Wait budgeting ───────────────────────────────────────────────────────── */

static void test_wait_timeout_leaves_job_intact(void)
{
    aegis_executor_t*   e = make_executor(1);
    aegis_task_t*       t = make_task("wait-budget");
    aegis_exec_result_t r = {0};
    atomic_store(&g_gate, 0);

    assert(aegis_executor_submit(e, t, gated_work, NULL) == AEGIS_OK);
    assert(aegis_executor_wait(e, aegis_task_id(t), &r, 30) == AEGIS_ERR_TIMEOUT);
    /* Zero-budget wait polls once. */
    assert(aegis_executor_wait(e, aegis_task_id(t), &r, 0) == AEGIS_ERR_TIMEOUT);

    open_gate();
    assert(aegis_executor_wait(e, aegis_task_id(t), &r, -1) == AEGIS_OK);
    assert(r.outcome == AEGIS_EXEC_COMPLETED);

    aegis_executor_destroy(e);
    aegis_task_destroy(t);
}

/* ── Shutdown ─────────────────────────────────────────────────────────────── */

static void test_shutdown_drains_everything(void)
{
    aegis_executor_t* e = make_executor(2);
    aegis_task_t*     tasks[4];
    atomic_store(&g_gate, 0);

    for (int i = 0; i < 4; i++) {
        char name[32];
        snprintf(name, sizeof(name), "drain-%d", i);
        tasks[i] = make_task(name);
        assert(aegis_executor_submit(e, tasks[i], slow_polled_work, NULL) == AEGIS_OK);
    }

    /* Queued jobs exist (2 workers, 4 jobs): wait for the pool to
     * actually pick up its share before asserting exact counts. */
    for (int i = 0; i < 2500; i++) {
        if (aegis_executor_running_count(e) == 2) {
            break;
        }
        aegis_sleep_ms(2);
    }
    assert(aegis_executor_running_count(e) == 2);
    assert(aegis_executor_pending_count(e) == 2);

    assert(aegis_executor_shutdown(e, -1) == AEGIS_OK);
    assert(aegis_executor_pending_count(e) == 0);
    assert(aegis_executor_running_count(e) == 0);
    assert(aegis_executor_shutdown(e, 100) == AEGIS_OK); /* idempotent no-op */

    assert(aegis_executor_submit(e, tasks[0], gated_work, NULL) == AEGIS_ERR_CANCELLED);

    /* Every task reached a terminal state; queued ones were never run. */
    for (int i = 0; i < 4; i++) {
        const aegis_task_state_t st = aegis_task_state(tasks[i]);
        assert(st == AEGIS_TASK_SUCCESS || st == AEGIS_TASK_CANCELLED || st == AEGIS_TASK_FAILED);
    }

    aegis_executor_destroy(e);
    for (int i = 0; i < 4; i++) {
        aegis_task_destroy(tasks[i]);
    }
}

/* Sets the flag once running, then burns ~50ms ignoring the token: long
 * enough that no small shutdown budget can wait it out. */
static int stubborn_flagged_work(aegis_task_t* t, const aegis_cancellation_token_t* tok, void* user)
{
    (void)t;
    (void)tok;
    atomic_store((atomic_int*)user, 1);
    for (int i = 0; i < 10; i++) {
        aegis_sleep_ms(5);
    }
    return AEGIS_OK;
}

static void test_shutdown_budget_timeout_then_recover(void)
{
    static atomic_int ran;
    aegis_executor_t* e = make_executor(1);
    aegis_task_t*     t = make_task("slow-shutdown");
    atomic_store(&ran, 0);

    assert(aegis_executor_submit(e, t, stubborn_flagged_work, &ran) == AEGIS_OK);

    /* Deterministic RUNNING evidence before arming the tiny budget: otherwise
     * shutdown may legally cancel the still-QUEUED job and drain instantly. */
    bool started = false;
    for (int i = 0; i < 5000 && !started; i++) {
        started = atomic_load(&ran) != 0;
        if (!started) {
            aegis_sleep_ms(1);
        }
    }
    assert(started);

    /* Work ignores the token and needs ~50ms; a 20ms budget cannot drain. */
    assert(aegis_executor_shutdown(e, 20) == AEGIS_ERR_TIMEOUT);
    /* A later unbounded shutdown still drains once the work returns. */
    assert(aegis_executor_shutdown(e, -1) == AEGIS_OK);
    assert(aegis_executor_running_count(e) == 0);

    aegis_executor_destroy(e);
    aegis_task_destroy(t);
}

static void test_destroy_discards_unreaped_results(void)
{
    aegis_executor_t* e = make_executor(2);
    aegis_task_t*     tasks[8];
    atomic_store(&g_gate, 1);

    for (int i = 0; i < 8; i++) {
        char name[32];
        snprintf(name, sizeof(name), "unreaped-%d", i);
        tasks[i] = make_task(name);
        assert(aegis_executor_submit(e, tasks[i], gated_work, NULL) == AEGIS_OK);
    }
    /* No waits at all: destroy must drain, join workers and free all
     * job records (validated by ASan). */
    aegis_executor_destroy(e);

    for (int i = 0; i < 8; i++) {
        /* Jobs still queued at destroy time are cancelled without running
         * (documented shutdown semantics); in-flight ones complete. Either
         * way every task must be terminal and every job record freed. */
        const aegis_task_state_t st = aegis_task_state(tasks[i]);
        assert(st == AEGIS_TASK_SUCCESS || st == AEGIS_TASK_CANCELLED);
        aegis_task_destroy(tasks[i]);
    }
}

int main(void)
{
    atomic_init(&g_gate, 1);

    test_lifecycle();
    test_submit_validation();
    test_success_path();
    test_failure_no_retry();
    test_retry_exhausted();
    test_retry_then_success();
    test_timeout_not_retried();
    test_cancel_queued_never_runs();
    test_cancel_running_cooperative();
    test_cancel_after_finish_is_busy();
    test_duplicate_and_queue_full();
    test_wait_timeout_leaves_job_intact();
    test_shutdown_drains_everything();
    test_shutdown_budget_timeout_then_recover();
    test_destroy_discards_unreaped_results();
    return 0;
}
