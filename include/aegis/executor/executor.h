#ifndef AEGIS_EXECUTOR_H
#define AEGIS_EXECUTOR_H

#include "aegis/types.h"
#include "aegis/task/task.h"
#include "aegis/executor/cancellation.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file executor.h
 * @brief Task execution engine with a worker pool.
 *
 * The scheduler decides WHICH task runs next; the executor actually
 * runs it. Division of responsibility (AGENTS.md §7):
 *   - Scheduler: selection + dispatch bookkeeping only.
 *   - Executor: execution, retry, timeout, cancellation, results.
 * The executor never plans, never schedules, and never calls tools
 * or LLMs itself — it invokes the caller-supplied work function.
 *
 * Execution model:
 *   - A fixed pool of worker threads pulls queued jobs FIFO.
 *   - Each job wraps one task + one work function. The work function
 *     performs the actual task body and may write outputs via
 *     aegis_task_set_output().
 *   - Cancellation is COOPERATIVE: the work function polls the token
 *     it receives; nothing is ever force-killed. Workers are fully
 *     reclaimable iff submitted work honors the token.
 *   - Timeout arms the job's token with a per-attempt deadline. Work
 *     that observes the flag aborts promptly; work that returns after
 *     its deadline (or ignores the token) is classified TIMED_OUT.
 *     Timed-out attempts are NOT retried — the caller asked for a
 *     bounded latency budget.
 *   - Retry reads the task's aegis_task_retry_policy_t:
 *     max_attempts counts RETRIES (0 = single attempt). Between
 *     attempts the task rests in WAITING for delay_ms (doubled each
 *     retry when exponential_backoff is set); the backoff sleep is
 *     interruptible by cancellation/shutdown.
 *
 * Task state machine driven by the executor:
 *   READY|PENDING → RUNNING → SUCCESS
 *                           → FAILED   (retries exhausted / timed out)
 *                           → WAITING  → RUNNING … (retry backoff)
 *                           → CANCELLED
 * The task must be PENDING or READY at submit time; anything else is
 * rejected (AEGIS_ERR_BUSY).
 *
 * Result delivery: blocking aegis_executor_wait() by task id. There
 * is intentionally NO completion callback — callbacks would run user
 * code while internal locks are in play and complicate lifetime
 * reasoning; wait() gives every caller the result exactly once.
 *
 * Ownership contract (Executor ↔ Graph):
 *   - The task GRAPH owns every task. submit()/cancel()/wait() store
 *     borrowed pointers only; the executor frees no tasks.
 *   - The caller MUST NOT destroy a task between submit() and the
 *     matching successful wait() (or full shutdown), otherwise
 *     workers may touch freed memory.
 *   - Results are copied out by wait(); the job record is released
 *     by the FIRST successful wait(). A later wait() for the same id
 *     reports AEGIS_ERR_NOT_FOUND.
 *   - destroy() discards results of jobs nobody waited for.
 *
 * Thread safety: all functions are thread-safe. Lock order (global):
 *   executor → task   (no reverse path; workers invoke user work
 *   functions with NO executor or task lock held).
 * No public call blocks unboundedly except wait()/shutdown() with a
 * negative (infinite) budget, and destroy() joining workers whose
 * work honors the token. Dedicated worker threads absorb blocking
 * work — an external event loop can drive this API without stalling.
 */

/** Opaque executor handle. */
typedef struct aegis_executor aegis_executor_t;

/**
 * @brief Work function executed by a worker on behalf of a task.
 *
 * Runs with no runtime locks held; may block, but should poll @p
 * token periodically so timeout/cancel/shutdown stay responsive.
 * Produces task output via aegis_task_set_output().
 *
 * @param task   Executed task (borrowed).
 * @param token  Cooperative cancellation channel (borrowed).
 * @param user   Caller context passed to submit (borrowed).
 * @return AEGIS_OK on success; any other status marks the attempt as
 *         failed (subject to retry policy).
 */
typedef aegis_status_t (*aegis_work_fn)(aegis_task_t* task, const aegis_cancellation_token_t* token,
                                        void* user);

/** Terminal outcome of a job. */
typedef enum aegis_exec_outcome {
    AEGIS_EXEC_COMPLETED = 0, /**< Work returned AEGIS_OK within budget.  */
    AEGIS_EXEC_FAILED    = 1, /**< Work failed; retries exhausted.         */
    AEGIS_EXEC_TIMED_OUT = 2, /**< Attempt exceeded the task timeout.      */
    AEGIS_EXEC_CANCELLED = 3, /**< Cancelled before/during execution.      */
} aegis_exec_outcome_t;

/** Immutable snapshot of a finished job. Copied out by wait(). */
typedef struct aegis_exec_result {
    aegis_exec_outcome_t outcome;
    aegis_status_t       status; /**< AEGIS_OK, last work error,
                                      ERR_TIMEOUT, or ERR_CANCELLED. */
    int     attempts;            /**< Attempts actually performed. */
    int64_t duration_ns;         /**< Wall time across all attempts. */
} aegis_exec_result_t;

/** Executor configuration. Zero fields fall back to defaults. */
typedef struct aegis_executor_config {
    unsigned worker_count;   /**< Worker threads. Default and cap: 4 / 64.  */
    size_t   queue_capacity; /**< Max queued (not yet running) jobs.
                                  Default: 1024.                            */
} aegis_executor_config_t;

/* ── Lifecycle ─────────────────────────────────────────────────────────── */

/**
 * @brief Create an executor with a worker pool.
 *
 * @param[out] out  Receives the handle. Ownership: transferred.
 * @param[in]  cfg  Configuration (borrowed; NULL = all defaults).
 * @return AEGIS_OK, AEGIS_ERR_INVALID (NULL out), or AEGIS_ERR_NOMEM.
 */
aegis_status_t aegis_executor_create(aegis_executor_t** out, const aegis_executor_config_t* cfg);

/**
 * @brief Shut down and free the executor.
 *
 * Performs a full drain shutdown first (see aegis_executor_shutdown):
 * queued-but-unstarted jobs are cancelled, running work sees its
 * token tripped, and ALL worker threads are joined before resources
 * are released. Blocks until every worker exits — guaranteed to
 * terminate when submitted work honors its token. Unreaped results
 * are discarded. Safe to call with NULL (no-op).
 *
 * @param exec Handle to destroy. Invalid after return. Ownership: consumed.
 */
void aegis_executor_destroy(aegis_executor_t* exec);

/* ── Job submission ────────────────────────────────────────────────────── */

/**
 * @brief Queue a task for execution.
 *
 * @param exec  Executor handle (borrowed).
 * @param task  Task to execute (borrowed; graph keeps ownership).
 *              Must be PENDING or READY; must not already be active.
 * @param fn    Work function (must not be NULL).
 * @param user  Context handed to @p fn (borrowed; may be NULL).
 * @return AEGIS_OK;
 *         AEGIS_ERR_INVALID  — NULL arguments;
 *         AEGIS_ERR_BUSY     — task state not submittable, task already
 *                              active on this executor, or queue full;
 *         AEGIS_ERR_CANCELLED— executor is shutting down;
 *         AEGIS_ERR_NOMEM    — job allocation failed.
 */
aegis_status_t aegis_executor_submit(aegis_executor_t* exec, aegis_task_t* task, aegis_work_fn fn,
                                     void* user);

/**
 * @brief Request cooperative cancellation of a task's job.
 *
 * Queued-not-started jobs complete as CANCELLED without ever running.
 * Running jobs have their token tripped (AEGIS_CANCEL_USER); outcome
 * depends on the work function honoring it.
 *
 * @param exec    Executor handle (borrowed).
 * @param task_id Id of a previously submitted task.
 * @return AEGIS_OK if cancellation was requested;
 *         AEGIS_ERR_NOT_FOUND — unknown or already-reaped task;
 *         AEGIS_ERR_BUSY      — job already finished;
 *         AEGIS_ERR_INVALID   — NULL executor.
 */
aegis_status_t aegis_executor_cancel(aegis_executor_t* exec, uint32_t task_id);

/**
 * @brief Block until the job finishes; copy out its result exactly once.
 *
 * @param exec       Executor handle (borrowed).
 * @param task_id    Submitted task id.
 * @param[out] out   Receives the result (may be NULL to discard).
 * @param timeout_ms Max wait; negative waits forever, 0 polls once.
 * @return AEGIS_OK (result copied, job reaped);
 *         AEGIS_ERR_TIMEOUT  — budget elapsed, job still unfinished;
 *         AEGIS_ERR_NOT_FOUND— unknown id or result already taken;
 *         AEGIS_ERR_INVALID  — NULL executor.
 */
aegis_status_t aegis_executor_wait(aegis_executor_t* exec, uint32_t task_id,
                                   aegis_exec_result_t* out, long timeout_ms);

/* ── Shutdown ──────────────────────────────────────────────────────────── */

/**
 * @brief Stop intake and drain outstanding work.
 *
 * Submits are rejected from now on (AEGIS_ERR_CANCELLED). Queued jobs
 * are cancelled immediately; running jobs' tokens are tripped
 * (AEGIS_CANCEL_SHUTDOWN). Waits until no job remains queued or
 * running.
 *
 * Idempotent: calling again after a successful drain is a no-op
 * returning AEGIS_OK.
 *
 * @param exec     Executor handle (borrowed).
 * @param wait_ms  Max drain wait; negative waits forever.
 * @return AEGIS_OK when fully drained; AEGIS_ERR_TIMEOUT if jobs were
 *         still active when the budget elapsed (they keep running;
 *         tokens stay tripped); AEGIS_ERR_INVALID.
 */
aegis_status_t aegis_executor_shutdown(aegis_executor_t* exec, long wait_ms);

/* ── Introspection ─────────────────────────────────────────────────────── */

/**
 * @brief Jobs queued but not yet started (includes cancelled-queued
 *        until a worker reaps them).
 *
 * @param exec Executor handle (borrowed; NULL → 0).
 */
size_t aegis_executor_pending_count(const aegis_executor_t* exec);

/**
 * @brief Jobs currently executing (including retry backoff rest).
 *
 * @param exec Executor handle (borrowed; NULL → 0).
 */
size_t aegis_executor_running_count(const aegis_executor_t* exec);

#ifdef __cplusplus
}
#endif

#endif /* AEGIS_EXECUTOR_H */
