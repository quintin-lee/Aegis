/**
 * @file scheduler_internal.h
 * @brief Internal scheduler struct layout.
 *
 * NOT part of the public API.
 *
 * Concurrency: all fields below are guarded by `lock`. Lock ordering
 * (global, no reverse path exists):
 *
 *     scheduler->lock → task_graph->lock → aegis_task->lock
 */
#ifndef AEGIS_SCHEDULER_INTERNAL_H
#define AEGIS_SCHEDULER_INTERNAL_H

#include "aegis/scheduler/scheduler.h"
#include "aegis/task/graph.h"
#include "aegis/common/mutex.h"
#include "graph_internal.h"
#include <stdint.h>
#include <stddef.h>

/** Maximum queued tasks (bounded by graph capacity). */
#define AEGIS_SCHED_MAX_TASKS AEGIS_GRAPH_MAX_TASKS

/** Ready-queue entry: task plus FIFO sequence number. */
typedef struct aegis_sched_entry {
    aegis_task_t* task; /**< Borrowed task pointer (owned by the graph). */
    uint64_t      seq;  /**< Monotonic enqueue sequence (FIFO tiebreak). */
} aegis_sched_entry_t;

/** Internal scheduler structure. */
struct aegis_scheduler {
    /* Concurrency */
    aegis_mutex_t* lock; /**< Guards every field below. */

    /* Wiring */
    aegis_task_graph_t* graph; /**< Borrowed task graph (source of tasks). */

    /* Ready queue — binary max-heap ordered by policy, then seq */
    aegis_sched_entry_t heap[AEGIS_SCHED_MAX_TASKS];
    size_t              n_heap;

    /* Duplicate-dispatch prevention: ids currently queued / handed out */
    uint32_t queued[AEGIS_SCHED_MAX_TASKS];
    size_t   n_queued;
    uint32_t inflight[AEGIS_SCHED_MAX_TASKS];
    size_t   n_inflight;

    /* FIFO sequence allocator */
    uint64_t next_seq;

    /* Policy hook — NULL selects the default Dependency→Priority→FIFO
     * order. Reserved extension point for deadline/resource policies. */
    aegis_sched_compare_fn cmp;
    void*                  cmp_user;
};

#endif /* AEGIS_SCHEDULER_INTERNAL_H */
