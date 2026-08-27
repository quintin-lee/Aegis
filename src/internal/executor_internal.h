/**
 * @file executor_internal.h
 * @brief Internal layout of the executor and its job records.
 *
 * NOT part of the public API.
 *
 * Ownership model:
 *   - The executor OWNS every aegis_job_t from successful submit()
 *     until it is reaped by a successful wait() (waiter frees) or
 *     discarded by destroy().
 *   - Tasks are BORROWED pointers owned by the task graph; the
 *     executor never frees them and never outlives the caller's
 *     contract (see executor.h ownership docs).
 *   - The embedded cancellation token is written by the owning worker
 *     (deadline, before handing the token to work) and by any thread
 *     via the lock-free cancel flags.
 *
 * Locking:
 *   - exec->lock protects queue, table, counters, intake/drained flags
 *     and all job->state/result fields.
 *   - Work functions run with NO executor lock held.
 *   - Global order: executor -> task (task setters may be called while
 *     holding exec->lock; never the reverse).
 */
#ifndef AEGIS_EXECUTOR_INTERNAL_H
#define AEGIS_EXECUTOR_INTERNAL_H

#include "aegis/executor.h"
#include "aegis/common/thread.h"
#include "cancellation_internal.h"

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>

/* Configuration fallbacks (zeroed config fields use these). */
#define AEGIS_EXEC_WORKERS_DEFAULT 4u
#define AEGIS_EXEC_WORKERS_MAX     64u
#define AEGIS_EXEC_QUEUE_DEFAULT   1024u

/**
 * Interruptible backoff granularity: retry delay sleeps are sliced
 * into chunks of at most this many milliseconds so cancellation and
 * shutdown are observed promptly during WAITING rest periods.
 */
#define AEGIS_EXEC_BACKOFF_SLICE_MS 5u

/** Job lifecycle. */
typedef enum aegis_job_state {
    AEGIS_JOB_QUEUED   = 0, /**< In the FIFO, not yet picked up.        */
    AEGIS_JOB_RUNNING  = 1, /**< Executing (or resting in backoff).     */
    AEGIS_JOB_FINISHED = 2, /**< Terminal; result awaits wait()/reap.   */
} aegis_job_state_t;

/** One submitted unit of work. Executor-owned. */
typedef struct aegis_job aegis_job_t;
struct aegis_job {
    struct aegis_job* next; /**< FIFO link; valid while QUEUED.      */

    uint32_t      task_id; /**< Cached aegis_task_id().             */
    aegis_task_t* task;    /**< Borrowed; graph keeps ownership.    */
    aegis_work_fn fn;      /**< User work function.                 */
    void*         user;    /**< Borrowed user context.              */

    aegis_job_state_t          state; /**< Guarded by executor lock.           */
    aegis_cancellation_token_t token; /**< Flags: lock-free; deadline: set by
                                           owning worker before each attempt. */
    aegis_exec_result_t result;       /**< Filled once, before FINISHED.       */
    int64_t             start_ns;     /**< First attempt start (monotonic ns),
                                           0 until started.                    */
};

/** Executor instance. Public handle is the opaque typedef. */
struct aegis_executor {
    /* Concurrency primitives (raw pthreads; no condvar wrapper exists). */
    pthread_mutex_t lock;
    pthread_cond_t  work_avail; /**< Signaled: enqueue or intake closed.   */
    pthread_cond_t  done_cond;  /**< Signaled: a job reached FINISHED.     */

    /* Intrusive FIFO of QUEUED jobs. */
    aegis_job_t* q_head;
    aegis_job_t* q_tail;
    size_t       q_count;
    size_t       q_capacity;

    /** Registry of ALL live jobs (QUEUED/RUNNING/unreaped FINISHED) for
     *  id-based lookup by cancel()/wait(). Swap-remove on reap. */
    aegis_job_t** table;
    size_t        table_len;
    size_t        table_cap;

    size_t running;     /**< Jobs popped but not yet FINISHED (includes
                             retry-backoff rest).                        */
    bool intake_closed; /**< Set by shutdown(); rejects submit().        */
    bool drained;       /**< Set once a drain completed successfully.    */

    aegis_thread_t** workers; /**< Worker handles; joined then freed.       */
    unsigned         worker_count;
};

#endif /* AEGIS_EXECUTOR_INTERNAL_H */
