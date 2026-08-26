/**
 * @file executor.c
 * @brief Task execution engine: worker pool, retry, timeout, cancellation.
 *
 * Locking discipline (global order: executor -> task):
 *   - exec->lock guards queue/table/counters/flags and job->state.
 *   - Task setters (aegis_task_set_state / _set_error) may be called
 *     while holding exec->lock or not; the task's own mutex serializes
 *     them. The reverse nesting (task lock -> executor lock) never
 *     occurs anywhere in this module.
 *   - User work functions run with NO runtime locks held, so they can
 *     freely call back into scheduler/task APIs.
 *
 * Cancellation is cooperative: nothing force-kills threads. Workers
 * are fully reclaimable iff submitted work honors its token (see the
 * public header contract).
 */
#define _POSIX_C_SOURCE 200809L

#include "internal/executor_internal.h"
#include "internal/cancellation_internal.h"
#include "internal/task_internal.h"
#include "aegis/common/time.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Backoff cap so exponential growth cannot overflow the ms domain. */
#define AEGIS_EXEC_BACKOFF_MAX_MS 60000L

/** Clamp a millisecond budget to a safe nanosecond delta. */
static int64_t deadline_after_ms(long timeout_ms)
{
    if (timeout_ms <= 0) {
        return 0;
    }
    const int64_t ns     = (int64_t)timeout_ms * INT64_C(1000000);
    const int64_t max_ns = INT64_MAX / 2; /* headroom for later compares */
    return ns > max_ns ? max_ns : ns;
}

static bool deadline_expired_ns(int64_t deadline_ns)
{
    return deadline_ns != 0 && aegis_mono_now() > deadline_ns;
}

/** Initialize @p cond with the CLOCK_MONOTONIC clock for timed waits. */
static int cond_init_monotonic(pthread_cond_t* cond)
{
    pthread_condattr_t attr;
    int                rc = pthread_condattr_init(&attr);
    if (rc != 0) {
        return rc;
    }
    rc = pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    if (rc == 0) {
        rc = pthread_cond_init(cond, &attr);
    }
    pthread_condattr_destroy(&attr);
    return rc;
}

static struct timespec timespec_from_deadline(int64_t deadline_ns)
{
    struct timespec ts;
    ts.tv_sec  = (time_t)(deadline_ns / INT64_C(1000000000));
    ts.tv_nsec = (long)(deadline_ns % INT64_C(1000000000));
    return ts;
}

/* ── Job table helpers (exec->lock held) ──────────────────────────────── */

static aegis_job_t* table_find_locked(const aegis_executor_t* exec, uint32_t task_id)
{
    for (size_t i = 0; i < exec->table_len; i++) {
        if (exec->table[i]->task_id == task_id) {
            return exec->table[i];
        }
    }
    return NULL;
}

static aegis_status_t table_add_locked(aegis_executor_t* exec, aegis_job_t* job)
{
    if (exec->table_len == exec->table_cap) {
        const size_t  new_cap = exec->table_cap == 0 ? 16 : exec->table_cap * 2;
        aegis_job_t** grown   = realloc(exec->table, new_cap * sizeof(*grown));
        if (!grown) {
            return AEGIS_ERR_NOMEM;
        }
        exec->table     = grown;
        exec->table_cap = new_cap;
    }
    exec->table[exec->table_len++] = job;
    return AEGIS_OK;
}

static void table_remove_locked(aegis_executor_t* exec, aegis_job_t* job)
{
    for (size_t i = 0; i < exec->table_len; i++) {
        if (exec->table[i] == job) {
            exec->table[i] = exec->table[--exec->table_len];
            return;
        }
    }
}

/* ── FIFO helpers (exec->lock held) ───────────────────────────────────── */

static void queue_push_locked(aegis_executor_t* exec, aegis_job_t* job)
{
    job->next = NULL;
    if (exec->q_tail) {
        exec->q_tail->next = job;
    } else {
        exec->q_head = job;
    }
    exec->q_tail = job;
    exec->q_count++;
}

static aegis_job_t* queue_pop_locked(aegis_executor_t* exec)
{
    aegis_job_t* job = exec->q_head;
    if (!job) {
        return NULL;
    }
    exec->q_head = job->next;
    if (!exec->q_head) {
        exec->q_tail = NULL;
    }
    job->next = NULL;
    exec->q_count--;
    return job;
}

/* ── Job execution ────────────────────────────────────────────────────── */

/**
 * Mark @p job terminal, publish its result, wake waiters/drain.
 * exec->lock is taken here; caller must NOT hold it.
 */
static void finish_job(aegis_executor_t* exec, aegis_job_t* job, aegis_exec_outcome_t outcome,
                       aegis_status_t status, int attempts)
{
    pthread_mutex_lock(&exec->lock);
    job->result.outcome     = outcome;
    job->result.status      = status;
    job->result.attempts    = attempts;
    job->result.duration_ns = aegis_mono_elapsed(job->start_ns, aegis_mono_now());
    job->state              = AEGIS_JOB_FINISHED;
    exec->running--;
    pthread_cond_broadcast(&exec->done_cond);
    pthread_mutex_unlock(&exec->lock);
}

/**
 * Sleep @p delay_ms in small slices, aborting early on cancellation.
 * @return false if the token was tripped during the rest period.
 */
static bool interruptible_backoff(const aegis_cancellation_token_t* token, long delay_ms)
{
    uint64_t remaining = delay_ms <= 0 ? 0 : (uint64_t)delay_ms;
    while (remaining > 0) {
        if (aegis_cancellation_token_is_cancelled(token)) {
            return false;
        }
        const uint64_t slice =
            remaining > AEGIS_EXEC_BACKOFF_SLICE_MS ? AEGIS_EXEC_BACKOFF_SLICE_MS : remaining;
        aegis_sleep_ms(slice);
        remaining -= slice;
    }
    return !aegis_cancellation_token_is_cancelled(token);
}

/** Run all attempts of @p job to a terminal outcome. No locks on entry. */
static void run_job(aegis_executor_t* exec, aegis_job_t* job)
{
    const aegis_task_retry_policy_t policy     = aegis_task_retry_policy(job->task);
    const long                      timeout_ms = aegis_task_timeout_ms(job->task);
    /* max_attempts counts RETRIES; total tries allowed = retries + 1. */
    const long max_tries = policy.max_attempts < 0 ? 0 : policy.max_attempts;
    long       delay_ms  = policy.delay_ms < 0 ? 0 : policy.delay_ms;

    pthread_mutex_lock(&exec->lock);
    if (job->start_ns == 0) {
        job->start_ns = aegis_mono_now();
    }
    pthread_mutex_unlock(&exec->lock);

    int attempts = 0;
    for (;;) {
        attempts++;

        /* Cancelled/shutdown before this attempt began: never run fn. */
        if (aegis_cancel_flags(&job->token) != AEGIS_CANCEL_NONE) {
            aegis_task_set_state(job->task, AEGIS_TASK_CANCELLED);
            finish_job(exec, job, AEGIS_EXEC_CANCELLED, AEGIS_ERR_CANCELLED, attempts - 1);
            return;
        }

        /* Arm the per-attempt deadline BEFORE handing the token out.
         * Plain write: only the owning worker mutates it per attempt. */
        job->token.deadline_ns =
            timeout_ms > 0 ? aegis_mono_now() + deadline_after_ms(timeout_ms) : 0;

        aegis_task_set_state(job->task, AEGIS_TASK_RUNNING);

        const aegis_status_t rc = job->fn(job->task, &job->token, job->user);

        /* Classification precedence: explicit cancel/shutdown flag >
         * expired deadline > work return code. */
        if (aegis_cancel_flags(&job->token) != AEGIS_CANCEL_NONE) {
            aegis_task_set_state(job->task, AEGIS_TASK_CANCELLED);
            finish_job(exec, job, AEGIS_EXEC_CANCELLED, AEGIS_ERR_CANCELLED, attempts);
            return;
        }
        if (deadline_expired_ns(job->token.deadline_ns)) {
            /* Timed-out attempts are never retried: latency budget spent. */
            aegis_task_set_error(job->task, "execution timed out");
            aegis_task_set_state(job->task, AEGIS_TASK_FAILED);
            finish_job(exec, job, AEGIS_EXEC_TIMED_OUT, AEGIS_ERR_TIMEOUT, attempts);
            return;
        }
        if (rc == AEGIS_OK) {
            aegis_task_set_state(job->task, AEGIS_TASK_SUCCESS);
            finish_job(exec, job, AEGIS_EXEC_COMPLETED, AEGIS_OK, attempts);
            return;
        }

        if ((long)attempts > max_tries) {
            /* Preserve a detailed message left by the work function;
             * fall back to a generic one when none was set. */
            const char* prev = aegis_task_error(job->task);
            if (!prev || prev[0] == '\0') {
                char msg[64];
                snprintf(msg, sizeof(msg), "work failed with status %d", (int)rc);
                aegis_task_set_error(job->task, msg);
            }
            aegis_task_set_state(job->task, AEGIS_TASK_FAILED);
            finish_job(exec, job, AEGIS_EXEC_FAILED, rc, attempts);
            return;
        }

        /* Retry rest in WAITING; interruptible by cancel/shutdown. */
        aegis_task_set_state(job->task, AEGIS_TASK_WAITING);
        if (!interruptible_backoff(&job->token, delay_ms)) {
            aegis_task_set_state(job->task, AEGIS_TASK_CANCELLED);
            finish_job(exec, job, AEGIS_EXEC_CANCELLED, AEGIS_ERR_CANCELLED, attempts);
            return;
        }
        if (policy.exponential_backoff && delay_ms < AEGIS_EXEC_BACKOFF_MAX_MS) {
            delay_ms *= 2;
            if (delay_ms > AEGIS_EXEC_BACKOFF_MAX_MS) {
                delay_ms = AEGIS_EXEC_BACKOFF_MAX_MS;
            }
        }
    }
}

static void* worker_main(void* arg)
{
    aegis_executor_t* exec = arg;

    pthread_mutex_lock(&exec->lock);
    for (;;) {
        while (exec->q_head == NULL && !exec->intake_closed) {
            pthread_cond_wait(&exec->work_avail, &exec->lock);
        }
        if (exec->q_head == NULL) {
            break; /* intake closed and queue drained: worker retires. */
        }

        aegis_job_t* job = queue_pop_locked(exec);
        job->state       = AEGIS_JOB_RUNNING;
        exec->running++;
        pthread_mutex_unlock(&exec->lock);

        run_job(exec, job); /* user code runs with no locks held */

        pthread_mutex_lock(&exec->lock);
    }
    pthread_mutex_unlock(&exec->lock);
    return NULL;
}

/* ── Public API ───────────────────────────────────────────────────────── */

aegis_status_t aegis_executor_create(aegis_executor_t** out, const aegis_executor_config_t* cfg)
{
    if (!out) {
        return AEGIS_ERR_INVALID;
    }
    *out = NULL;

    aegis_executor_t* exec = calloc(1, sizeof(*exec));
    if (!exec) {
        return AEGIS_ERR_NOMEM;
    }

    unsigned workers =
        cfg && cfg->worker_count > 0 ? cfg->worker_count : AEGIS_EXEC_WORKERS_DEFAULT;
    size_t q_capacity =
        cfg && cfg->queue_capacity > 0 ? cfg->queue_capacity : AEGIS_EXEC_QUEUE_DEFAULT;
    if (workers > AEGIS_EXEC_WORKERS_MAX) {
        workers = AEGIS_EXEC_WORKERS_MAX;
    }

    if (pthread_mutex_init(&exec->lock, NULL) != 0) {
        free(exec);
        return AEGIS_ERR_INTERNAL;
    }
    if (cond_init_monotonic(&exec->work_avail) != 0 || cond_init_monotonic(&exec->done_cond) != 0) {
        goto fail_sync;
    }

    exec->worker_count = workers;
    exec->q_capacity   = q_capacity;
    exec->workers      = calloc(workers, sizeof(*exec->workers));
    if (!exec->workers) {
        goto fail_sync;
    }

    for (unsigned i = 0; i < workers; i++) {
        if (aegis_thread_create(&exec->workers[i], worker_main, exec, 0) != 0) {
            /* Roll back: close intake so already-started workers retire. */
            pthread_mutex_lock(&exec->lock);
            exec->intake_closed = true;
            pthread_cond_broadcast(&exec->work_avail);
            pthread_mutex_unlock(&exec->lock);
            for (unsigned j = 0; j < i; j++) {
                aegis_thread_join(exec->workers[j]);
                aegis_thread_destroy(exec->workers[j]);
            }
            goto fail_workers;
        }
    }

    *out = exec;
    return AEGIS_OK;

fail_workers:
    free(exec->workers);
fail_sync:
    pthread_cond_destroy(&exec->done_cond);
    pthread_cond_destroy(&exec->work_avail);
    pthread_mutex_destroy(&exec->lock);
    free(exec);
    return AEGIS_ERR_NOMEM;
}

void aegis_executor_destroy(aegis_executor_t* exec)
{
    if (!exec) {
        return;
    }

    /* Full drain first: trips tokens of everything still outstanding. */
    (void)aegis_executor_shutdown(exec, -1);

    for (unsigned i = 0; i < exec->worker_count; i++) {
        aegis_thread_join(exec->workers[i]);
        aegis_thread_destroy(exec->workers[i]);
    }
    free(exec->workers);

    /* Discard results nobody waited for. */
    pthread_mutex_lock(&exec->lock);
    for (size_t i = 0; i < exec->table_len; i++) {
        free(exec->table[i]);
    }
    free(exec->table);
    pthread_mutex_unlock(&exec->lock);

    pthread_cond_destroy(&exec->done_cond);
    pthread_cond_destroy(&exec->work_avail);
    pthread_mutex_destroy(&exec->lock);
    free(exec);
}

aegis_status_t aegis_executor_submit(aegis_executor_t* exec, aegis_task_t* task, aegis_work_fn fn,
                                     void* user)
{
    if (!exec || !task || !fn) {
        return AEGIS_ERR_INVALID;
    }

    /* Allocate before mutating anything: every failure path below is
     * then a plain free() with state untouched. */
    aegis_job_t* job = calloc(1, sizeof(*job));
    if (!job) {
        return AEGIS_ERR_NOMEM;
    }

    pthread_mutex_lock(&exec->lock);

    if (exec->intake_closed) {
        pthread_mutex_unlock(&exec->lock);
        free(job);
        return AEGIS_ERR_CANCELLED;
    }
    if (table_find_locked(exec, aegis_task_id(task)) != NULL) {
        pthread_mutex_unlock(&exec->lock);
        free(job);
        return AEGIS_ERR_BUSY; /* duplicate active submission */
    }
    if (exec->q_count >= exec->q_capacity) {
        pthread_mutex_unlock(&exec->lock);
        free(job);
        return AEGIS_ERR_BUSY; /* queue full */
    }
    if (table_add_locked(exec, job) != AEGIS_OK) {
        pthread_mutex_unlock(&exec->lock);
        free(job);
        return AEGIS_ERR_NOMEM;
    }

    /* Atomic PENDING|READY -> RUNNING under the task mutex closes the
     * window that would let two executors claim the same task. */
    if (!aegis_task_try_begin_execution(task)) {
        table_remove_locked(exec, job);
        pthread_mutex_unlock(&exec->lock);
        free(job);
        return AEGIS_ERR_BUSY; /* wrong state (already running/terminal) */
    }

    job->task_id = aegis_task_id(task);
    job->task    = task;
    job->fn      = fn;
    job->user    = user;
    job->state   = AEGIS_JOB_QUEUED;
    queue_push_locked(exec, job);
    pthread_cond_signal(&exec->work_avail);

    pthread_mutex_unlock(&exec->lock);
    return AEGIS_OK;
}

aegis_status_t aegis_executor_cancel(aegis_executor_t* exec, uint32_t task_id)
{
    if (!exec) {
        return AEGIS_ERR_INVALID;
    }

    pthread_mutex_lock(&exec->lock);
    aegis_job_t* job = table_find_locked(exec, task_id);
    if (!job) {
        pthread_mutex_unlock(&exec->lock);
        return AEGIS_ERR_NOT_FOUND;
    }
    if (job->state == AEGIS_JOB_FINISHED) {
        pthread_mutex_unlock(&exec->lock);
        return AEGIS_ERR_BUSY;
    }
    aegis_cancel_request(&job->token, AEGIS_CANCEL_USER);
    pthread_mutex_unlock(&exec->lock);
    return AEGIS_OK;
}

aegis_status_t aegis_executor_wait(aegis_executor_t* exec, uint32_t task_id,
                                   aegis_exec_result_t* out, long timeout_ms)
{
    if (!exec) {
        return AEGIS_ERR_INVALID;
    }

    const bool    infinite  = timeout_ms < 0;
    const int64_t budget_ns = infinite ? 0 : deadline_after_ms(timeout_ms);
    const int64_t deadline  = infinite ? 0 : aegis_mono_now() + budget_ns;

    aegis_status_t status = AEGIS_ERR_TIMEOUT;

    pthread_mutex_lock(&exec->lock);
    for (;;) {
        aegis_job_t* job = table_find_locked(exec, task_id);
        if (!job) {
            status = AEGIS_ERR_NOT_FOUND;
            break;
        }
        if (job->state == AEGIS_JOB_FINISHED) {
            if (out) {
                *out = job->result;
            }
            table_remove_locked(exec, job);
            free(job); /* single-owner release: first successful wait */
            status = AEGIS_OK;
            break;
        }
        if (infinite) {
            pthread_cond_wait(&exec->done_cond, &exec->lock);
        } else {
            const int64_t remaining = deadline - aegis_mono_now();
            if (remaining <= 0) {
                break; /* status stays TIMEOUT; job remains intact */
            }
            const struct timespec ts = timespec_from_deadline(deadline);
            /* Return value needs no handling by design: signal, spurious
             * wake, and ETIMEDOUT all resolve through the predicate
             * re-check at the top of this loop. */
            pthread_cond_timedwait(&exec->done_cond, &exec->lock, &ts);
        }
    }
    pthread_mutex_unlock(&exec->lock);
    return status;
}

aegis_status_t aegis_executor_shutdown(aegis_executor_t* exec, long wait_ms)
{
    if (!exec) {
        return AEGIS_ERR_INVALID;
    }

    const bool    infinite = wait_ms < 0;
    const int64_t deadline = infinite ? 0 : aegis_mono_now() + deadline_after_ms(wait_ms);

    pthread_mutex_lock(&exec->lock);

    if (!exec->intake_closed) {
        exec->intake_closed = true;
        /* Trip every outstanding token; queued jobs will be reaped as
         * CANCELLED by workers without ever running their function. */
        for (size_t i = 0; i < exec->table_len; i++) {
            aegis_cancel_request(&exec->table[i]->token, AEGIS_CANCEL_SHUTDOWN);
        }
        pthread_cond_broadcast(&exec->work_avail); /* wake idle workers */
    } else if (exec->drained) {
        pthread_mutex_unlock(&exec->lock);
        return AEGIS_OK; /* idempotent no-op after successful drain */
    }
    /* Repeated shutdown after a TIMEOUT: tokens already tripped; fall
     * through and keep waiting within the new budget. */

    aegis_status_t status = AEGIS_OK;
    while (exec->q_count > 0 || exec->running > 0) {
        if (infinite) {
            pthread_cond_wait(&exec->done_cond, &exec->lock);
            continue;
        }
        const int64_t remaining = deadline - aegis_mono_now();
        if (remaining <= 0) {
            status = AEGIS_ERR_TIMEOUT; /* jobs keep running, tokens stay tripped */
            break;
        }
        const struct timespec ts = timespec_from_deadline(deadline);
        /* Return value needs no handling by design: signal, spurious
         * wake, and ETIMEDOUT all resolve through the predicate
         * re-check at the top of this loop. */
        pthread_cond_timedwait(&exec->done_cond, &exec->lock, &ts);
    }
    if (status == AEGIS_OK) {
        exec->drained = true;
    }

    pthread_mutex_unlock(&exec->lock);
    return status;
}

size_t aegis_executor_pending_count(const aegis_executor_t* exec)
{
    if (!exec) {
        return 0;
    }
    /* Logical const: the mutex guards the observable state; locking it
     * from a read-only accessor mutates no observable value. */
    pthread_mutex_t* lock = &((aegis_executor_t*)(void*)exec)->lock;
    pthread_mutex_lock(lock);
    const size_t n = exec->q_count;
    pthread_mutex_unlock(lock);
    return n;
}

size_t aegis_executor_running_count(const aegis_executor_t* exec)
{
    if (!exec) {
        return 0;
    }
    pthread_mutex_t* lock = &((aegis_executor_t*)(void*)exec)->lock;
    pthread_mutex_lock(lock);
    const size_t n = exec->running;
    pthread_mutex_unlock(lock);
    return n;
}
