#ifndef AEGIS_SCHEDULER_H
#define AEGIS_SCHEDULER_H

#include "aegis/types.h"
#include "aegis/task/task.h"
#include "aegis/task/graph.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file scheduler.h
 * @brief Task scheduler — selects and dispatches ready tasks.
 *
 * The scheduler decides WHICH ready task runs NEXT and in WHAT order.
 * It never executes tasks, never calls tools, and never calls LLMs —
 * selection and dispatch only (execution belongs to the Executor).
 *
 * Scheduling policy (v1): Dependency → Priority → FIFO.
 *   1. A task is schedulable only when its dependencies are satisfied
 *      (all dependency sources reached SUCCESS or SKIPPED). Tasks whose
 *      sources FAILED or CANCELLED are never scheduled — failure
 *      propagation is the replanner's concern.
 *   2. Among schedulable tasks, higher aegis_task_priority() first.
 *   3. Equal priority: earlier enqueue first (FIFO).
 *
 * Deadline / resource aware policies are reserved through the
 * aegis_sched_compare_fn hook (see aegis_scheduler_set_policy); no cost
 * model is implemented in v1.
 *
 * Ownership contract (Scheduler ↔ Executor ↔ Graph):
 *   - The task GRAPH owns every task. The scheduler stores borrowed
 *     pointers only and frees nothing. The scheduler must be destroyed
 *     before (or outlive) the graph it is attached to.
 *   - aegis_scheduler_next() hands the executor a BORROWED task pointer.
 *     The executor must not free it. For every dispatch the executor
 *     MUST call aegis_scheduler_notify_complete() exactly once — on
 *     success, failure, or cancellation alike — before the task can be
 *     considered released from scheduling.
 *
 * Duplicate-dispatch guarantee: between next() handing out a task and
 * the matching notify_complete(), no other next() returns the same task.
 *
 * Thread safety: all functions are thread-safe. Internally guarded by
 * one scheduler mutex; global lock order is
 * scheduler → task graph → task (no reverse acquisition exists).
 */

/** Opaque scheduler handle. */
typedef struct aegis_scheduler aegis_scheduler_t;

/**
 * @brief Custom ordering policy hook (reserved for deadline/resource).
 *
 * Return > 0 if @p lhs should be dispatched before @p rhs,
 * < 0 if after, 0 if indifferent. When tasks compare equal, FIFO
 * enqueue order breaks the tie. Called with the scheduler lock held —
 * the hook must not call back into scheduler APIs nor block.
 *
 * @param lhs Candidate task (borrowed).
 * @param rhs Candidate task (borrowed).
 * @param user User context registered alongside the hook (borrowed).
 * @return Ordering decision as described above.
 */
typedef int (*aegis_sched_compare_fn)(const aegis_task_t* lhs, const aegis_task_t* rhs, void* user);

/**
 * @brief Create a new scheduler.
 *
 * @param[out] out  Receives the scheduler handle. Ownership: transferred.
 * @return AEGIS_OK on success, or an error code.
 */
aegis_status_t aegis_scheduler_create(aegis_scheduler_t** out);

/**
 * @brief Destroy a scheduler and release all resources.
 *
 * Does NOT touch tasks (the graph owns them). Safe to call with NULL
 * (no-op).
 *
 * @param sched Handle to destroy. After return, pointer is invalid. Ownership: consumed.
 */
void aegis_scheduler_destroy(aegis_scheduler_t* sched);

/**
 * @brief Attach the scheduler to a task graph.
 *
 * The graph is borrowed; its lifetime must exceed the scheduler's.
 * Re-attaching while already bound is rejected (AEGIS_ERR_BUSY).
 *
 * @param sched Scheduler handle (borrowed).
 * @param graph Task graph whose tasks will be polled (borrowed).
 * @return AEGIS_OK, AEGIS_ERR_INVALID (NULL args), or AEGIS_ERR_BUSY.
 */
aegis_status_t aegis_scheduler_attach(aegis_scheduler_t* sched, aegis_task_graph_t* graph);

/**
 * @brief Register a custom dispatch-order policy.
 *
 * Reserved extension point for deadline/resource-aware policies.
 * Passing NULL restores the default Dependency→Priority→FIFO order.
 * May be called at any time; affects subsequent heap operations.
 *
 * @param sched Scheduler handle (borrowed).
 * @param cmp   Comparator (borrowed; NULL restores default).
 * @param user  Opaque context passed to @p cmp (borrowed; may be NULL).
 * @return AEGIS_OK or AEGIS_ERR_INVALID.
 */
aegis_status_t aegis_scheduler_set_policy(aegis_scheduler_t* sched, aegis_sched_compare_fn cmp,
                                          void* user);

/**
 * @brief Harvest newly-schedulable tasks into the ready queue.
 *
 * For every graph task that is READY, or PENDING with all dependency
 * sources SUCCESS/SKIPPED (which is promoted to READY), an entry is
 * enqueued unless the task id is already queued or in flight.
 * If the queue is full, harvesting stops early; a later poll picks up
 * the remainder after the queue drains.
 *
 * @param sched        Scheduler handle (borrowed).
 * @param[out] out_enqueued Receives the number of newly enqueued tasks
 *                          (may be NULL).
 * @return AEGIS_OK or AEGIS_ERR_INVALID.
 */
aegis_status_t aegis_scheduler_poll(aegis_scheduler_t* sched, size_t* out_enqueued);

/**
 * @brief Dequeue the next task to execute.
 *
 * Selects by the active policy (default: priority, then FIFO) among
 * queued tasks still in the READY state. Entries whose state changed
 * while queued (e.g. cancelled externally) are dropped silently.
 * The returned pointer is borrowed; see the ownership contract above.
 *
 * @param sched Scheduler handle (borrowed).
 * @param[out] out Receives the task pointer (borrowed, do not free).
 * @return AEGIS_OK, AEGIS_ERR_INVALID, or AEGIS_ERR_NOT_FOUND when the
 *         queue holds no dispatchable task.
 */
aegis_status_t aegis_scheduler_next(aegis_scheduler_t* sched, aegis_task_t** out);

/**
 * @brief Report completion of a previously dispatched task.
 *
 * Releases the task from the in-flight set so scheduling bookkeeping
 * stays exact. Must be called exactly once per successful next(),
 * regardless of execution outcome. Does NOT modify the task state —
 * the executor drives RUNNING → terminal transitions itself.
 *
 * @param sched Scheduler handle (borrowed).
 * @param task  Task previously returned by next() (borrowed).
 * @return AEGIS_OK, AEGIS_ERR_INVALID, or AEGIS_ERR_NOT_FOUND when the
 *         task is not currently in flight.
 */
aegis_status_t aegis_scheduler_notify_complete(aegis_scheduler_t* sched, const aegis_task_t* task);

/**
 * @brief Number of tasks currently waiting in the ready queue.
 *
 * @param sched Scheduler handle (borrowed; NULL → 0).
 * @return Queued task count.
 */
size_t aegis_scheduler_pending_count(const aegis_scheduler_t* sched);

/**
 * @brief Number of tasks currently dispatched and not yet completed.
 *
 * @param sched Scheduler handle (borrowed; NULL → 0).
 * @return In-flight task count.
 */
size_t aegis_scheduler_inflight_count(const aegis_scheduler_t* sched);

#ifdef __cplusplus
}
#endif

#endif /* AEGIS_SCHEDULER_H */
