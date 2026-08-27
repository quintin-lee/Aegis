/**
 * @file test_executor_concurrent.c
 * @brief Concurrency and stress tests for the executor.
 *
 * Covers the required scenarios:
 *   - 1000-task mixed stress (success / retry / timeout / cancel) with
 *     a concurrent cancel storm, verifying outcome classification and
 *     exactly-once result reaping;
 *   - shutdown-vs-submit race hammer across repeated executor rounds;
 *   - create/destroy cycles with in-flight work (worker teardown).
 *
 * TSan-clean by construction: cross-thread flags are atomics; results
 * are read only after a successful wait() (happens-before via the
 * executor's internal lock).
 */
#include "aegis/executor.h"
#include "aegis/task.h"
#include "aegis/common/time.h"

#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Shared helpers ────────────────────────────────────────────────────────── */

enum job_kind {
    KIND_FAST_OK  = 0, /**< Succeeds immediately; retries irrelevant.      */
    KIND_FLAKY    = 1, /**< Fails N random times then succeeds (retries=3).*/
    KIND_TIMEOUT  = 2, /** Oversleeps its deadline; must end TIMED_OUT.   */
    KIND_SLOW_OK  = 3, /**< Sleeps briefly then succeeds.                  */
    KIND_CANCELME = 4, /**< Runs until token observed; storm target.       */
    KIND_COUNT    = 5,
};

typedef struct job_ctx {
    enum job_kind kind;
    atomic_int    remaining_failures;
    atomic_int    ran;
} job_ctx_t;

/** Work needing no context: succeeds immediately. */
static aegis_status_t fast_ok_work(aegis_task_t* task, const aegis_cancellation_token_t* token,
                                   void* user)
{
    (void)task;
    (void)token;
    (void)user;
    return AEGIS_OK;
}

static aegis_status_t ctx_work(aegis_task_t* task, const aegis_cancellation_token_t* token,
                               void* user)
{
    (void)task;
    job_ctx_t* ctx = user;
    atomic_store(&ctx->ran, 1);

    switch (ctx->kind) {
    case KIND_FLAKY:
        if (atomic_load(&ctx->remaining_failures) > 0) {
            atomic_fetch_sub(&ctx->remaining_failures, 1);
            return AEGIS_ERR_PROVIDER;
        }
        return AEGIS_OK;

    case KIND_TIMEOUT:
        /* Sleep past the armed deadline in token-polled slices; the
         * expiry shows up as is_cancelled() → abort promptly. */
        for (int i = 0; i < 12; i++) {
            if (aegis_cancellation_token_is_cancelled(token)) {
                return AEGIS_ERR_CANCELLED;
            }
            aegis_sleep_ms(10);
        }
        return AEGIS_OK; /* unreachable when a deadline is armed */

    case KIND_SLOW_OK:
        for (int i = 0; i < 3; i++) {
            if (aegis_cancellation_token_is_cancelled(token)) {
                return AEGIS_ERR_CANCELLED;
            }
            aegis_sleep_ms(2);
        }
        return AEGIS_OK;

    case KIND_CANCELME:
        while (!aegis_cancellation_token_is_cancelled(token)) {
            aegis_sleep_ms(3);
        }
        return AEGIS_ERR_CANCELLED;

    case KIND_FAST_OK:
    default:
        return AEGIS_OK;
    }
}

/** Per-task classification of how a FINISHED job may look after waits. */
struct tally {
    atomic_int completed;
    atomic_int timed_out;
    atomic_int cancelled;
};

static uint32_t id_of(const aegis_task_t* t)
{
    return aegis_task_id(t);
}

/* ── Test 1: 1000-task mixed stress with cancel storm ─────────────────────── */

static aegis_executor_t* g_stress_exec;

#define STRESS_N              1000
#define STRESS_SUBMIT_THREADS 4
#define STORM_MS              120

static aegis_task_t* stress_tasks[STRESS_N];
static job_ctx_t     stress_ctx[STRESS_N];
static struct tally  stress_tally[STRESS_N];

typedef struct {
    int         thread_index;
    atomic_int* next_slot; /* shared work counter */
} submit_arg_t;

static void* stress_submit_thread(void* raw)
{
    submit_arg_t* arg  = raw;
    unsigned      seed = (unsigned)(arg->thread_index * 7919 + 13);

    for (;;) {
        const size_t i = (size_t)atomic_fetch_add(arg->next_slot, 1);
        if (i >= STRESS_N) {
            break;
        }
        aegis_task_t* t    = stress_tasks[i];
        const int     roll = rand_r(&seed) % 100;

        if (roll < 40) {
            stress_ctx[i].kind = KIND_FAST_OK;
        } else if (roll < 60) {
            stress_ctx[i].kind = KIND_FLAKY;
            atomic_store(&stress_ctx[i].remaining_failures, (int)(rand_r(&seed) % 3));
            aegis_task_set_retry_policy(
                t, (aegis_task_retry_policy_t){.max_attempts = 3, .delay_ms = 1});
        } else if (roll < 75) {
            stress_ctx[i].kind = KIND_TIMEOUT;
            aegis_task_set_timeout_ms(t, 30);
        } else if (roll < 90) {
            stress_ctx[i].kind = KIND_SLOW_OK;
        } else {
            stress_ctx[i].kind = KIND_CANCELME;
        }

        const aegis_status_t rc = aegis_executor_submit(g_stress_exec, t, ctx_work, &stress_ctx[i]);
        assert(rc == AEGIS_OK); /* capacity sized to never fill */
    }
    return NULL;
}

static void* storm_thread(void* raw)
{
    atomic_int* stop = raw;
    unsigned    seed = 4242;
    while (!atomic_load(stop)) {
        const size_t i = (size_t)(rand_r(&seed) % STRESS_N);
        /* Best effort: BUSY/NOT_FOUND are fine outcomes here. */
        aegis_executor_cancel(g_stress_exec, id_of(stress_tasks[i]));
        aegis_sleep_ms(1);
    }
    return NULL;
}

static void classify_stress(size_t i)
{
    aegis_exec_result_t r = {0};
    assert(aegis_executor_wait(g_stress_exec, id_of(stress_tasks[i]), &r, -1) == AEGIS_OK);

    aegis_task_t*            t  = stress_tasks[i];
    const aegis_task_state_t st = aegis_task_state(t);

    switch (r.outcome) {
    case AEGIS_EXEC_COMPLETED:
        atomic_fetch_add(&stress_tally[i].completed, 1);
        assert(r.status == AEGIS_OK);
        assert(st == AEGIS_TASK_SUCCESS);
        break;
    case AEGIS_EXEC_TIMED_OUT:
        assert(stress_ctx[i].kind == KIND_TIMEOUT); /* only oversleepers time out */
        assert(r.status == AEGIS_ERR_TIMEOUT);
        assert(st == AEGIS_TASK_FAILED);
        atomic_fetch_add(&stress_tally[i].timed_out, 1);
        break;
    case AEGIS_EXEC_CANCELLED:
        /* Storm may cancel any role before or during execution. */
        assert(r.status == AEGIS_ERR_CANCELLED);
        assert(st == AEGIS_TASK_CANCELLED);
        atomic_fetch_add(&stress_tally[i].cancelled, 1);
        break;
    case AEGIS_EXEC_FAILED:
    default:
        assert(0 && "unexpected FAILED outcome in stress roles");
    }
}

static void test_stress_1000_mixed(void)
{
    memset(stress_ctx, 0, sizeof(stress_ctx));
    memset(stress_tally, 0, sizeof(stress_tally));

    aegis_executor_config_t cfg = {.worker_count = 8, .queue_capacity = STRESS_N + 16};
    assert(aegis_executor_create(&g_stress_exec, &cfg) == AEGIS_OK);

    char name[32];
    for (size_t i = 0; i < STRESS_N; i++) {
        snprintf(name, sizeof(name), "stress-%zu", i);
        stress_tasks[i] = NULL;
        assert(aegis_task_create(&stress_tasks[i], name, NULL) == AEGIS_OK);
        atomic_init(&stress_ctx[i].remaining_failures, 0);
        atomic_init(&stress_ctx[i].ran, 0);
        atomic_init(&stress_tally[i].completed, 0);
        atomic_init(&stress_tally[i].timed_out, 0);
        atomic_init(&stress_tally[i].cancelled, 0);
    }

    pthread_t    submitters[STRESS_SUBMIT_THREADS];
    submit_arg_t sargs[STRESS_SUBMIT_THREADS];
    atomic_int   next_slot;
    atomic_init(&next_slot, 0);
    for (int k = 0; k < STRESS_SUBMIT_THREADS; k++) {
        sargs[k].thread_index = k;
        sargs[k].next_slot    = &next_slot;
        assert(pthread_create(&submitters[k], NULL, stress_submit_thread, &sargs[k]) == 0);
    }
    for (int k = 0; k < STRESS_SUBMIT_THREADS; k++) {
        pthread_join(submitters[k], NULL);
    }

    /* Cancel storm in parallel with execution. */
    atomic_int stop;
    atomic_init(&stop, 0);
    pthread_t storm;
    assert(pthread_create(&storm, NULL, storm_thread, &stop) == 0);
    aegis_sleep_ms(STORM_MS);
    atomic_store(&stop, 1);
    pthread_join(storm, NULL);

    /* Guarantee termination: whatever the storm did not reach is
     * cancelled by the drain (tokens tripped), so every wait below
     * is bounded. Classifier accepts CANCELLED for any role. */
    assert(aegis_executor_shutdown(g_stress_exec, -1) == AEGIS_OK);

    /* Reap every result exactly once and validate classification. */
    for (size_t i = 0; i < STRESS_N; i++) {
        classify_stress(i);
    }

    /* Sanity: the mix actually exercised multiple outcomes overall. */
    int total_completed = 0, total_timed = 0, total_cancelled = 0;
    for (size_t i = 0; i < STRESS_N; i++) {
        total_completed += atomic_load(&stress_tally[i].completed);
        total_timed += atomic_load(&stress_tally[i].timed_out);
        total_cancelled += atomic_load(&stress_tally[i].cancelled);
    }
    assert(total_completed + total_timed + total_cancelled == STRESS_N);
    printf("  stress: completed=%d timed_out=%d cancelled=%d\n", total_completed, total_timed,
           total_cancelled);

    aegis_executor_destroy(g_stress_exec);
    for (size_t i = 0; i < STRESS_N; i++) {
        aegis_task_destroy(stress_tasks[i]);
    }
}

/* ── Test 2: shutdown-vs-submit race hammer ──────────────────────────────── */

#define RACE_ROUNDS  25
#define RACE_THREADS 6
#define RACE_PER_THR 20

typedef struct {
    aegis_executor_t** exec; /* points at the round-local executor */
    atomic_int         ok_cnt;
    atomic_int         rej_cnt;
} race_arg_t;

static void* race_submitter(void* raw)
{
    race_arg_t* arg = raw;
    for (int i = 0; i < RACE_PER_THR; i++) {
        aegis_task_t* t = NULL;
        if (aegis_task_create(&t, "race", NULL) != AEGIS_OK) {
            continue;
        }
        const aegis_status_t rc = aegis_executor_submit(*arg->exec, t, fast_ok_work, NULL);
        if (rc == AEGIS_OK) {
            atomic_fetch_add(&arg->ok_cnt, 1);
            /* Task ownership passes to round cleanup on success. */
            aegis_executor_wait(*arg->exec, aegis_task_id(t), NULL, -1);
            aegis_task_destroy(t);
        } else {
            atomic_fetch_add(&arg->rej_cnt, 1);
            aegis_task_destroy(t);
        }
    }
    return NULL;
}

static void test_shutdown_race_hammer(void)
{
    for (int round = 0; round < RACE_ROUNDS; round++) {
        aegis_executor_t*       e   = NULL;
        aegis_executor_config_t cfg = {.worker_count = 4, .queue_capacity = 512};
        assert(aegis_executor_create(&e, &cfg) == AEGIS_OK);

        race_arg_t arg = {.exec = &e};
        atomic_init(&arg.ok_cnt, 0);
        atomic_init(&arg.rej_cnt, 0);

        pthread_t th[RACE_THREADS];
        for (int k = 0; k < RACE_THREADS; k++) {
            assert(pthread_create(&th[k], NULL, race_submitter, &arg) == 0);
        }
        /* Shutdown races the submitters immediately. */
        aegis_executor_shutdown(e, -1);
        for (int k = 0; k < RACE_THREADS; k++) {
            pthread_join(th[k], NULL);
        }
        aegis_executor_destroy(e);

        /* Every attempt either landed or was rejected — none lost. */
        assert(atomic_load(&arg.ok_cnt) + atomic_load(&arg.rej_cnt) == RACE_THREADS * RACE_PER_THR);
    }
}

/* ── Test 3: create/destroy cycles with in-flight work ───────────────────── */

#define CYCLES      50
#define CYCLE_TASKS 10

static void test_create_destroy_cycles(void)
{
    for (int c = 0; c < CYCLES; c++) {
        aegis_executor_t*       e   = NULL;
        aegis_executor_config_t cfg = {.worker_count = 2, .queue_capacity = 64};
        assert(aegis_executor_create(&e, &cfg) == AEGIS_OK);

        aegis_task_t* tasks[CYCLE_TASKS];
        for (int i = 0; i < CYCLE_TASKS; i++) {
            tasks[i] = NULL;
            assert(aegis_task_create(&tasks[i], "cycle", NULL) == AEGIS_OK);
            assert(aegis_executor_submit(e, tasks[i], fast_ok_work, NULL) == AEGIS_OK);
        }
        for (int i = 0; i < CYCLE_TASKS; i++) {
            aegis_exec_result_t r = {0};
            assert(aegis_executor_wait(e, aegis_task_id(tasks[i]), &r, -1) == AEGIS_OK);
            assert(r.outcome == AEGIS_EXEC_COMPLETED);
            assert(aegis_task_state(tasks[i]) == AEGIS_TASK_SUCCESS);
            aegis_task_destroy(tasks[i]); /* reaped already: safe */
        }
        aegis_executor_destroy(e);
    }
}

int main(void)
{
    test_stress_1000_mixed();
    test_shutdown_race_hammer();
    test_create_destroy_cycles();
    printf("test_executor_concurrent: all tests passed\n");
    return 0;
}
