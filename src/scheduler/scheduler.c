/**
 * @file scheduler.c
 * @brief Task scheduler — dependency-aware, priority-ordered dispatch.
 *
 * Selects ready tasks from the attached task graph and hands them to
 * the executor. The scheduler never executes tasks, never calls tools,
 * and never calls LLMs.
 *
 * Default policy: Dependency → Priority → FIFO.
 *
 * Thread safety: one plain mutex guards all scheduler state. Global
 * lock order is scheduler → graph → task; every path in this file
 * acquires locks in that order only.
 */
#include "aegis/scheduler/scheduler.h"
#include "scheduler_internal.h"
#include "task_internal.h"
#include <stdlib.h>

/* ── Id-set helpers (linear scan — bounded by AEGIS_SCHED_MAX_TASKS) ──────── */

/**
 * @brief Test whether an id is present in a small id set (linear scan).
 *
 * Sets are bounded by AEGIS_SCHED_MAX_TASKS, so a scan stays cheap.
 *
 * @param[in] set  Array of @p n ids.
 * @param[in] n    Number of entries in @p set.
 * @param[in] id   Id to look for.
 * @return True when present, false otherwise.
 */
static bool id_in_set(const uint32_t* set, size_t n, uint32_t id)
{
    for (size_t i = 0; i < n; i++) {
        if (set[i] == id) {
            return true;
        }
    }
    return false;
}

/**
 * @brief Append an id to a set. The caller guarantees spare capacity.
 *
 * @param[in,out] set  Array with room for one more id.
 * @param[in,out] n    Entry count; incremented.
 * @param[in]     id   Id to append (duplicates are the caller's concern).
 */
static void id_set_add(uint32_t* set, size_t* n, uint32_t id)
{
    set[(*n)++] = id;
}

/**
 * @brief Remove an id from a set via swap-with-last.
 *
 * @param[in,out] set  Array of @p *n ids.
 * @param[in,out] n    Entry count; decremented on removal.
 * @param[in]     id   Id to remove.
 * @return True when removed, false when absent (set untouched).
 */
static bool id_set_remove(uint32_t* set, size_t* n, uint32_t id)
{
    for (size_t i = 0; i < *n; i++) {
        if (set[i] == id) {
            set[i] = set[--(*n)];
            return true;
        }
    }
    return false;
}

/* ── Heap ordering ─────────────────────────────────────────────────────────── */

/** Return true if entry @p a must be dispatched before entry @p b. */
static bool entry_before(const aegis_scheduler_t* s, const aegis_sched_entry_t* a,
                         const aegis_sched_entry_t* b)
{
    if (s->cmp) {
        int r = s->cmp(a->task, b->task, s->cmp_user);
        if (r != 0) {
            return r > 0;
        }
        return a->seq < b->seq; /* policy-indifferent → FIFO */
    }
    int pa = aegis_task_priority(a->task);
    int pb = aegis_task_priority(b->task);
    if (pa != pb) {
        return pa > pb; /* higher priority first */
    }
    return a->seq < b->seq; /* FIFO tiebreak */
}

/**
 * @brief Exchange two heap entries in place.
 *
 * @param[in,out] a  First entry.
 * @param[in,out] b  Second entry.
 */
static void heap_swap(aegis_sched_entry_t* a, aegis_sched_entry_t* b)
{
    aegis_sched_entry_t tmp = *a;
    *a                      = *b;
    *b                      = tmp;
}

/**
 * @brief Restore the heap property upward from a newly inserted entry.
 *
 * @param[in] s    Scheduler owning the heap.
 * @param[in] idx  Index of the entry to sift toward the root.
 */
static void heap_sift_up(aegis_scheduler_t* s, size_t idx)
{
    while (idx > 0) {
        size_t parent = (idx - 1) / 2;
        if (!entry_before(s, &s->heap[idx], &s->heap[parent])) {
            break;
        }
        heap_swap(&s->heap[idx], &s->heap[parent]);
        idx = parent;
    }
}

/**
 * @brief Restore the heap property downward from a demoted root entry.
 *
 * @param[in] s    Scheduler owning the heap.
 * @param[in] idx  Index of the entry to sift toward the leaves.
 */
static void heap_sift_down(aegis_scheduler_t* s, size_t idx)
{
    for (;;) {
        size_t left  = 2 * idx + 1;
        size_t right = 2 * idx + 2;
        size_t best  = idx;

        if (left < s->n_heap && entry_before(s, &s->heap[left], &s->heap[best])) {
            best = left;
        }
        if (right < s->n_heap && entry_before(s, &s->heap[right], &s->heap[best])) {
            best = right;
        }
        if (best == idx) {
            break;
        }
        heap_swap(&s->heap[idx], &s->heap[best]);
        idx = best;
    }
}

/* ── Dependency gating ─────────────────────────────────────────────────────── */

/**
 * @brief Find a graph task by id with a linear scan.
 *
 * Called with the graph lock held; the borrowed task pointer must not
 * outlive the lock.
 *
 * @param[in] g   Task graph to search.
 * @param[in] id  Task id to look for.
 * @return Pointer to the task, or NULL when absent.
 */
static aegis_task_t* find_task_by_id(const aegis_task_graph_t* g, uint32_t id)
{
    for (size_t i = 0; i < g->n_tasks; i++) {
        if (g->tasks[i] && g->tasks[i]->id == id) {
            return g->tasks[i];
        }
    }
    return NULL;
}

/**
 * True when every dependency source of tasks[idx] has reached a
 * state that satisfies the edge: SUCCESS or SKIPPED. Sources that are
 * FAILED or CANCELLED block the dependent permanently in v1 — failure
 * propagation belongs to the replanner, not the scheduler.
 *
 * Called with the graph lock held; reads task states through the
 * thread-safe accessor (task locks nest inside the graph lock).
 */
static bool deps_satisfied_locked(const aegis_task_graph_t* g, size_t idx)
{
    for (size_t j = 0; j < g->n_deps[idx]; j++) {
        const aegis_dependency_t* dep = g->deps[idx][j];
        if (!dep) {
            continue;
        }
        const aegis_task_t* src = find_task_by_id(g, aegis_dependency_source(dep));
        if (!src) {
            return false; /* dangling edge: treat as unsatisfied */
        }
        aegis_task_state_t st = aegis_task_state(src);
        if (st != AEGIS_TASK_SUCCESS && st != AEGIS_TASK_SKIPPED) {
            return false;
        }
    }
    return true;
}

/* ── Lifecycle ─────────────────────────────────────────────────────────────── */

/**
 * @brief Create an empty scheduler with its state mutex.
 *
 * No graph is attached yet; attach one before polling.
 *
 * @param[out] out  Receives the new scheduler; set only on success.
 * @return AEGIS_OK on success, AEGIS_ERR_INVALID for NULL @p out,
 *         AEGIS_ERR_NOMEM on allocation/mutex failure.
 *
 * Ownership: the caller owns the returned scheduler and must release it with
 * aegis_scheduler_destroy(). Thread-safe after creation.
 */
aegis_status_t aegis_scheduler_create(aegis_scheduler_t** out)
{
    if (!out) {
        return AEGIS_ERR_INVALID;
    }

    aegis_scheduler_t* s = (aegis_scheduler_t*)calloc(1, sizeof(*s));
    if (!s) {
        return AEGIS_ERR_NOMEM;
    }

    s->lock = NULL;
    int rc  = aegis_mutex_create(&s->lock, AEGIS_MUTEX_PLAIN);
    if (rc != 0) {
        free(s);
        return AEGIS_ERR_NOMEM;
    }

    *out = s;
    return AEGIS_OK;
}

/**
 * @brief Destroy a scheduler and its mutex.
 *
 * Tasks are owned by the attached graph and are left alone; only scheduler
 * state (heap, id sets, mutex) is released. NULL is accepted and ignored.
 *
 * @param[in] sched  Scheduler to destroy, or NULL.
 */
void aegis_scheduler_destroy(aegis_scheduler_t* sched)
{
    if (!sched) {
        return;
    }
    /* Tasks belong to the graph — nothing owned here but the mutex. */
    aegis_mutex_destroy(sched->lock);
    free(sched);
}

/* ── Wiring & policy ───────────────────────────────────────────────────────── */

/**
 * @brief Attach the task graph the scheduler dispatches from (borrowed).
 *
 * The graph must outlive the scheduler and stay attached; only one graph
 * may be attached at a time.
 *
 * @param[in] sched  Scheduler to wire.
 * @param[in] graph  Task graph to dispatch from (borrowed, must outlive use).
 * @return AEGIS_OK on success, AEGIS_ERR_INVALID for NULL arguments,
 *         AEGIS_ERR_BUSY when a graph is already attached.
 *
 * Thread-safe.
 */
aegis_status_t aegis_scheduler_attach(aegis_scheduler_t* sched, aegis_task_graph_t* graph)
{
    if (!sched || !graph) {
        return AEGIS_ERR_INVALID;
    }

    aegis_mutex_lock(sched->lock);

    if (sched->graph) {
        aegis_mutex_unlock(sched->lock);
        return AEGIS_ERR_BUSY;
    }
    sched->graph = graph;

    aegis_mutex_unlock(sched->lock);
    return AEGIS_OK;
}

/**
 * @brief Install a custom dispatch-order comparator (plus opaque user data).
 *
 * A NULL @p cmp restores the default policy (Dependency → Priority → FIFO).
 * The comparator must return positive when its first task dispatches first;
 * ties and a NULL policy fall back to FIFO sequence order.
 *
 * @param[in] sched  Scheduler to configure.
 * @param[in] cmp    Comparator, or NULL for the default policy.
 * @param[in] user   Opaque pointer forwarded to @p cmp.
 * @return AEGIS_OK on success, AEGIS_ERR_INVALID for NULL scheduler.
 *
 * Thread-safe.
 */
aegis_status_t aegis_scheduler_set_policy(aegis_scheduler_t* sched, aegis_sched_compare_fn cmp,
                                          void* user)
{
    if (!sched) {
        return AEGIS_ERR_INVALID;
    }

    aegis_mutex_lock(sched->lock);
    sched->cmp      = cmp;
    sched->cmp_user = user;
    aegis_mutex_unlock(sched->lock);
    return AEGIS_OK;
}

/* ── Selection & dispatch ──────────────────────────────────────────────────── */

/**
 * @brief Harvest newly dispatchable tasks from the graph into the heap.
 *
 * Scans every graph task: PENDING tasks whose dependencies are all
 * SUCCESS/SKIPPED advance to READY, and READY tasks not already queued or
 * in flight are pushed onto the dispatch heap (capped at
 * AEGIS_SCHED_MAX_TASKS; the remainder waits for a later poll). Observes
 * the global lock order scheduler → graph → task.
 *
 * @param[in]  sched         Scheduler with an attached graph.
 * @param[out] out_enqueued  Optional receiver for the newly queued count.
 * @return AEGIS_OK on success, AEGIS_ERR_INVALID for NULL scheduler or a
 *         missing graph attachment.
 *
 * Thread-safe.
 */
aegis_status_t aegis_scheduler_poll(aegis_scheduler_t* sched, size_t* out_enqueued)
{
    if (!sched) {
        return AEGIS_ERR_INVALID;
    }
    if (out_enqueued) {
        *out_enqueued = 0;
    }

    aegis_mutex_lock(sched->lock);

    aegis_task_graph_t* g = sched->graph;
    if (!g) {
        aegis_mutex_unlock(sched->lock);
        return AEGIS_ERR_INVALID;
    }

    size_t enqueued = 0;

    aegis_mutex_lock(g->lock);

    for (size_t i = 0; i < g->n_tasks; i++) {
        aegis_task_t* t = g->tasks[i];
        if (!t) {
            continue;
        }

        aegis_task_state_t st = aegis_task_state(t);
        if (st == AEGIS_TASK_PENDING && deps_satisfied_locked(g, i)) {
            /* Dependencies met — drive the documented PENDING → READY
             * transition so downstream components see a consistent state. */
            aegis_task_set_state(t, AEGIS_TASK_READY);
            st = AEGIS_TASK_READY;
        }
        if (st != AEGIS_TASK_READY) {
            continue;
        }

        uint32_t id = t->id;
        if (id_in_set(sched->queued, sched->n_queued, id) ||
            id_in_set(sched->inflight, sched->n_inflight, id)) {
            continue; /* already scheduled or running: no duplicates */
        }
        if (sched->n_heap >= AEGIS_SCHED_MAX_TASKS) {
            break; /* queue full: remainder harvested on a later poll */
        }

        aegis_sched_entry_t e      = {.task = t, .seq = sched->next_seq++};
        sched->heap[sched->n_heap] = e;
        heap_sift_up(sched, sched->n_heap++);
        id_set_add(sched->queued, &sched->n_queued, id);
        enqueued++;
    }

    aegis_mutex_unlock(g->lock);
    aegis_mutex_unlock(sched->lock);

    if (out_enqueued) {
        *out_enqueued = enqueued;
    }
    return AEGIS_OK;
}

/**
 * @brief Pop the highest-priority dispatchable task from the heap.
 *
 * Pops the heap root, marks its id in flight (so it is never handed out
 * twice), and returns the borrowed task pointer. Stale entries whose state
 * changed while queued (cancelled/skipped externally) are silently dropped.
 *
 * @param[in]  sched  Scheduler to pop from.
 * @param[out] out    Receives the borrowed task; set only on success.
 * @return AEGIS_OK on success, AEGIS_ERR_INVALID for NULL arguments,
 *         AEGIS_ERR_NOT_FOUND when the heap holds nothing dispatchable.
 *
 * Ownership: the graph retains the task; the caller must report completion
 * via aegis_scheduler_notify_complete(). Thread-safe.
 */
aegis_status_t aegis_scheduler_next(aegis_scheduler_t* sched, aegis_task_t** out)
{
    if (!sched || !out) {
        return AEGIS_ERR_INVALID;
    }
    *out = NULL;

    aegis_mutex_lock(sched->lock);

    while (sched->n_heap > 0) {
        /* Pop the root. */
        aegis_sched_entry_t top = sched->heap[0];
        sched->heap[0]          = sched->heap[--sched->n_heap];
        heap_sift_down(sched, 0);

        (void)id_set_remove(sched->queued, &sched->n_queued, top.task->id);

        /* Drop stale entries: state changed while queued (e.g. the task
         * was cancelled or skipped externally). */
        if (aegis_task_state(top.task) != AEGIS_TASK_READY) {
            continue;
        }
        if (id_in_set(sched->inflight, sched->n_inflight, top.task->id)) {
            continue; /* defensive: never hand out twice */
        }

        id_set_add(sched->inflight, &sched->n_inflight, top.task->id);
        *out = top.task;

        aegis_mutex_unlock(sched->lock);
        return AEGIS_OK;
    }

    aegis_mutex_unlock(sched->lock);
    return AEGIS_ERR_NOT_FOUND;
}

/**
 * @brief Release a task from the in-flight set after it finishes.
 *
 * Pairs with aegis_scheduler_next(): every handed-out task must be
 * reported exactly once so dependents can become dispatchable and the id
 * may be scheduled again.
 *
 * @param[in] sched  Scheduler tracking the task.
 * @param[in] task   Finished task (only its id is read).
 * @return AEGIS_OK on success, AEGIS_ERR_INVALID for NULL arguments,
 *         AEGIS_ERR_NOT_FOUND when the id is not in flight.
 *
 * Thread-safe.
 */
aegis_status_t aegis_scheduler_notify_complete(aegis_scheduler_t* sched, const aegis_task_t* task)
{
    if (!sched || !task) {
        return AEGIS_ERR_INVALID;
    }

    aegis_mutex_lock(sched->lock);

    if (!id_set_remove(sched->inflight, &sched->n_inflight, task->id)) {
        aegis_mutex_unlock(sched->lock);
        return AEGIS_ERR_NOT_FOUND;
    }

    aegis_mutex_unlock(sched->lock);
    return AEGIS_OK;
}

/* ── Introspection ─────────────────────────────────────────────────────────── */

/**
 * @brief Return the number of tasks waiting in the dispatch heap.
 *
 * @param[in] sched  Scheduler to inspect, or NULL.
 * @return Heap entry count, or 0 for NULL.
 *
 * Thread-safe (logical const: locking mutates no observable value).
 */
size_t aegis_scheduler_pending_count(const aegis_scheduler_t* sched)
{
    if (!sched) {
        return 0;
    }
    /* Cast mirrors graph.c's accessor pattern: the mutex API is
     * non-const, but this operation does not mutate logical state. */
    aegis_scheduler_t* s = (aegis_scheduler_t*)sched;
    aegis_mutex_lock(s->lock);
    size_t n = s->n_heap;
    aegis_mutex_unlock(s->lock);
    return n;
}

/**
 * @brief Return the number of tasks handed out but not yet reported complete.
 *
 * @param[in] sched  Scheduler to inspect, or NULL.
 * @return In-flight count, or 0 for NULL.
 *
 * Thread-safe (logical const: locking mutates no observable value).
 */
size_t aegis_scheduler_inflight_count(const aegis_scheduler_t* sched)
{
    if (!sched) {
        return 0;
    }
    aegis_scheduler_t* s = (aegis_scheduler_t*)sched;
    aegis_mutex_lock(s->lock);
    size_t n = s->n_inflight;
    aegis_mutex_unlock(s->lock);
    return n;
}
