/**
 * @file graph.c
 * @brief Task Graph — DAG with cycle detection and topological queries.
 *
 * Thread safety: all operations are protected by a recursive mutex.
 * publish() snapshots subscribers under lock, then dispatches without
 * lock to avoid deadlock.
 */
#define _POSIX_C_SOURCE 200809L
#include "aegis/graph.h"
#include "internal/graph_internal.h"
#include "internal/task_internal.h"
#include "internal/dependency_internal.h"
#include "internal/lifecycle.h"
#include "aegis/common/mutex.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>

/* ── Helpers ───────────────────────────────────────────────────────────────── */

static aegis_task_t* find_task(const aegis_task_graph_t* g, uint32_t id)
{
    for (size_t i = 0; i < g->n_tasks; i++) {
        if (g->tasks[i] && g->tasks[i]->id == id) {
            return g->tasks[i];
        }
    }
    return NULL;
}

static bool has_dependency(const aegis_task_graph_t* g, uint32_t source, uint32_t target)
{
    for (size_t i = 0; i < g->n_tasks; i++) {
        if (!g->tasks[i] || g->tasks[i]->id != target) {
            continue;
        }
        for (size_t j = 0; j < g->n_deps[i]; j++) {
            if (g->deps[i][j] && g->deps[i][j]->source == source) {
                return true;
            }
        }
    }
    return false;
}

/** Check if adding edge source→target would create a cycle.
 * Uses DFS from target to see if we can reach source. */
static bool would_create_cycle(aegis_task_graph_t* g, uint32_t source, uint32_t target)
{
    if (source == target) {
        return true; /* self-loop */
    }

    /* Build an adjacency list using task indices for safe array access.
     * adj[i] contains the indices of tasks that depend on task i. */
    size_t adj[AEGIS_GRAPH_MAX_TASKS][AEGIS_GRAPH_MAX_TASKS];
    size_t adj_count[AEGIS_GRAPH_MAX_TASKS] = {0};

    for (size_t i = 0; i < g->n_tasks; i++) {
        if (!g->tasks[i]) {
            continue;
        }
        for (size_t j = 0; j < g->n_deps[i]; j++) {
            aegis_dependency_t* dep = g->deps[i][j];
            if (!dep) {
                continue;
            }
            /* dep->source is the task that must complete first
             * dep->target is the task that depends on it.
             * Edge: source -> target (source must complete before target) */
            /* Find source index */
            for (size_t si = 0; si < g->n_tasks; si++) {
                if (g->tasks[si] && g->tasks[si]->id == dep->source) {
                    if (adj_count[si] < AEGIS_GRAPH_MAX_TASKS) {
                        adj[si][adj_count[si]++] = i;
                    }
                    break;
                }
            }
        }
    }

    /* BFS from target's index to see if we can reach source's index */
    size_t target_idx = 0, source_idx = 0;
    for (size_t i = 0; i < g->n_tasks; i++) {
        if (g->tasks[i]) {
            if (g->tasks[i]->id == target) {
                target_idx = i;
            }
            if (g->tasks[i]->id == source) {
                source_idx = i;
            }
        }
    }

    bool   visited[AEGIS_GRAPH_MAX_TASKS] = {false};
    size_t q[AEGIS_GRAPH_MAX_TASKS];
    size_t q_head = 0, q_tail = 0;

    q[q_tail++]         = target_idx;
    visited[target_idx] = true;

    while (q_head < q_tail) {
        size_t curr = q[q_head++];
        if (curr == source_idx) {
            return true;
        }

        for (size_t k = 0; k < adj_count[curr]; k++) {
            size_t next = adj[curr][k];
            if (!visited[next]) {
                visited[next] = true;
                if (q_tail < sizeof(q) / sizeof(q[0])) {
                    q[q_tail++] = next;
                }
            }
        }
    }
    return false;
}

/* ── Lifecycle ─────────────────────────────────────────────────────────────── */

aegis_status_t aegis_task_graph_create(aegis_task_graph_t** out)
{
    AEGIS_CHECK_OUT(out);

    aegis_task_graph_t* g = (aegis_task_graph_t*)calloc(1, sizeof(*g));
    if (!g) {
        return AEGIS_ERR_NOMEM;
    }

    int rc = aegis_mutex_create(&g->lock, AEGIS_MUTEX_RECURSIVE);
    if (rc != 0) {
        free(g);
        return AEGIS_ERR_NOMEM;
    }

    g->next_id = 1;
    *out       = g;
    return AEGIS_OK;
}

void aegis_task_graph_destroy(aegis_task_graph_t* graph)
{
    if (!graph) {
        return;
    }

    /* Free all tasks and dependencies */
    for (size_t i = 0; i < graph->n_tasks; i++) {
        if (graph->tasks[i]) {
            aegis_task_destroy(graph->tasks[i]);
            graph->tasks[i] = NULL;
        }
        for (size_t j = 0; j < graph->n_deps[i]; j++) {
            free(graph->deps[i][j]);
            graph->deps[i][j] = NULL;
        }
    }
    graph->n_tasks        = 0;
    graph->n_dependencies = 0;

    aegis_mutex_destroy(graph->lock);
    free(graph);
}

/* ── Task Management ───────────────────────────────────────────────────────── */

aegis_status_t aegis_task_graph_add_task(aegis_task_graph_t* graph, aegis_task_t* task)
{
    if (!graph || !task) {
        return AEGIS_ERR_INVALID;
    }

    aegis_mutex_lock(graph->lock);

    if (graph->n_tasks >= AEGIS_GRAPH_MAX_TASKS) {
        aegis_mutex_unlock(graph->lock);
        return AEGIS_ERR_BUSY;
    }

    /* Assign ID if not set */
    if (task->id == 0) {
        task->id = graph->next_id++;
    }

    size_t idx         = graph->n_tasks++;
    graph->tasks[idx]  = task;
    graph->n_deps[idx] = 0;

    aegis_mutex_unlock(graph->lock);
    return AEGIS_OK;
}

aegis_status_t aegis_task_graph_remove_task(aegis_task_graph_t* graph, aegis_task_t* task)
{
    if (!graph || !task) {
        return AEGIS_ERR_INVALID;
    }

    aegis_mutex_lock(graph->lock);

    /* Find and remove the task */
    size_t idx = 0;
    for (; idx < graph->n_tasks; idx++) {
        if (graph->tasks[idx] == task) {
            break;
        }
    }
    if (idx >= graph->n_tasks) {
        aegis_mutex_unlock(graph->lock);
        return AEGIS_ERR_NOT_FOUND;
    }

    /* Remove all dependencies involving this task */
    for (size_t i = 0; i < graph->n_tasks; i++) {
        for (size_t j = 0; j < graph->n_deps[i];) {
            if (graph->deps[i][j] &&
                (graph->deps[i][j]->source == task->id || graph->deps[i][j]->target == task->id)) {
                free(graph->deps[i][j]);
                graph->deps[i][j] = NULL;
                /* Shift remaining deps */
                for (size_t k = j; k < graph->n_deps[i] - 1; k++) {
                    graph->deps[i][k] = graph->deps[i][k + 1];
                }
                graph->n_deps[i]--;
                graph->n_dependencies--;
            } else {
                j++;
            }
        }
    }

    /* Compact task array */
    for (size_t i = idx; i < graph->n_tasks - 1; i++) {
        graph->tasks[i]  = graph->tasks[i + 1];
        graph->n_deps[i] = graph->n_deps[i + 1];
        for (size_t j = 0; j < graph->n_deps[i]; j++) {
            graph->deps[i][j] = graph->deps[i + 1][j];
        }
    }
    graph->tasks[graph->n_tasks - 1] = NULL;
    graph->n_tasks--;

    aegis_mutex_unlock(graph->lock);
    return AEGIS_OK;
}

aegis_task_t* aegis_task_graph_get_task(const aegis_task_graph_t* graph, uint32_t id)
{
    if (!graph) {
        return NULL;
    }
    aegis_mutex_lock(graph->lock);
    aegis_task_t* t = find_task((aegis_task_graph_t*)graph, id);
    aegis_mutex_unlock(graph->lock);
    return t;
}

/* ── Dependency Management ─────────────────────────────────────────────────── */

aegis_status_t aegis_task_graph_add_dependency(aegis_task_graph_t* graph, aegis_task_t* source,
                                               aegis_task_t* target)
{
    if (!graph || !source || !target) {
        return AEGIS_ERR_INVALID;
    }

    aegis_mutex_lock(graph->lock);

    /* Both tasks must exist in the graph */
    bool source_found = false, target_found = false;
    for (size_t i = 0; i < graph->n_tasks; i++) {
        if (graph->tasks[i] == source) {
            source_found = true;
        }
        if (graph->tasks[i] == target) {
            target_found = true;
        }
    }
    if (!source_found || !target_found) {
        aegis_mutex_unlock(graph->lock);
        return AEGIS_ERR_NOT_FOUND;
    }

    /* Check for duplicate */
    if (has_dependency(graph, source->id, target->id)) {
        aegis_mutex_unlock(graph->lock);
        return AEGIS_OK;
    }

    /* Check for cycle */
    if (would_create_cycle(graph, source->id, target->id)) {
        aegis_mutex_unlock(graph->lock);
        return AEGIS_ERR_INVALID;
    }

    /* Add dependency */
    aegis_dependency_t* dep = (aegis_dependency_t*)calloc(1, sizeof(*dep));
    if (!dep) {
        aegis_mutex_unlock(graph->lock);
        return AEGIS_ERR_NOMEM;
    }
    dep->source = source->id;
    dep->target = target->id;

    /* Find target's index */
    size_t tidx = 0;
    for (; tidx < graph->n_tasks; tidx++) {
        if (graph->tasks[tidx] == target) {
            break;
        }
    }

    if (graph->n_deps[tidx] >= AEGIS_GRAPH_MAX_DEPS_PER_TASK) {
        free(dep);
        aegis_mutex_unlock(graph->lock);
        return AEGIS_ERR_BUSY;
    }

    graph->deps[tidx][graph->n_deps[tidx]++] = dep;
    graph->n_dependencies++;

    aegis_mutex_unlock(graph->lock);
    return AEGIS_OK;
}

aegis_status_t aegis_task_graph_remove_dependency(aegis_task_graph_t* graph, aegis_task_t* source,
                                                  aegis_task_t* target)
{
    if (!graph || !source || !target) {
        return AEGIS_ERR_INVALID;
    }

    aegis_mutex_lock(graph->lock);

    /* Find target's index */
    size_t tidx = 0;
    for (; tidx < graph->n_tasks; tidx++) {
        if (graph->tasks[tidx] == target) {
            break;
        }
    }
    if (tidx >= graph->n_tasks) {
        aegis_mutex_unlock(graph->lock);
        return AEGIS_ERR_NOT_FOUND;
    }

    /* Find and remove the dependency */
    for (size_t j = 0; j < graph->n_deps[tidx]; j++) {
        if (graph->deps[tidx][j] && graph->deps[tidx][j]->source == source->id) {
            free(graph->deps[tidx][j]);
            graph->deps[tidx][j] = NULL;
            /* Shift remaining */
            for (size_t k = j; k < graph->n_deps[tidx] - 1; k++) {
                graph->deps[tidx][k] = graph->deps[tidx][k + 1];
            }
            graph->n_deps[tidx]--;
            graph->n_dependencies--;
            aegis_mutex_unlock(graph->lock);
            return AEGIS_OK;
        }
    }

    aegis_mutex_unlock(graph->lock);
    return AEGIS_ERR_NOT_FOUND;
}

/* ── Query ─────────────────────────────────────────────────────────────────── */

aegis_status_t aegis_task_graph_ready_tasks(const aegis_task_graph_t* graph,
                                            aegis_task_t*** out_vector, size_t* out_count)
{
    if (!graph || !out_vector || !out_count) {
        return AEGIS_ERR_INVALID;
    }

    aegis_mutex_lock(graph->lock);

    /* Count ready tasks */
    size_t count = 0;
    for (size_t i = 0; i < graph->n_tasks; i++) {
        if (graph->tasks[i] && graph->tasks[i]->state == AEGIS_TASK_READY) {
            count++;
        }
    }

    /* Allocate output vector */
    aegis_task_t** vec = (aegis_task_t**)calloc(count, sizeof(aegis_task_t*));
    if (count > 0 && !vec) {
        aegis_mutex_unlock(graph->lock);
        return AEGIS_ERR_NOMEM;
    }

    size_t idx = 0;
    for (size_t i = 0; i < graph->n_tasks; i++) {
        if (graph->tasks[i] && graph->tasks[i]->state == AEGIS_TASK_READY) {
            vec[idx++] = graph->tasks[i];
        }
    }

    aegis_mutex_unlock(graph->lock);

    *out_vector = vec;
    *out_count  = count;
    return AEGIS_OK;
}

size_t aegis_task_graph_task_count(const aegis_task_graph_t* graph)
{
    if (!graph) {
        return 0;
    }
    aegis_mutex_lock(graph->lock);
    size_t n = graph->n_tasks;
    aegis_mutex_unlock(graph->lock);
    return n;
}

size_t aegis_task_graph_dependency_count(const aegis_task_graph_t* graph)
{
    if (!graph) {
        return 0;
    }
    aegis_mutex_lock(graph->lock);
    size_t n = graph->n_dependencies;
    aegis_mutex_unlock(graph->lock);
    return n;
}

/* ── Validation ────────────────────────────────────────────────────────────── */

bool aegis_task_graph_is_dag(const aegis_task_graph_t* graph)
{
    if (!graph) {
        return true;
    }

    aegis_mutex_lock(graph->lock);

    /* Kahn's algorithm for cycle detection */
    size_t n = graph->n_tasks;
    if (n == 0) {
        aegis_mutex_unlock(graph->lock);
        return true;
    }

    /* Compute in-degrees */
    size_t in_degree[AEGIS_GRAPH_MAX_TASKS] = {0};
    for (size_t i = 0; i < n; i++) {
        for (size_t j = 0; j < graph->n_deps[i]; j++) {
            if (!graph->deps[i][j]) {
                continue;
            }
            /* Find source task index */
            for (size_t k = 0; k < n; k++) {
                if (graph->tasks[k] && graph->tasks[k]->id == graph->deps[i][j]->source) {
                    in_degree[i]++;
                    break;
                }
            }
        }
    }

    /* Find all tasks with in-degree 0 */
    size_t queue[AEGIS_GRAPH_MAX_TASKS];
    size_t q_head = 0, q_tail = 0;
    for (size_t i = 0; i < n; i++) {
        if (in_degree[i] == 0) {
            queue[q_tail++] = i;
        }
    }

    size_t processed = 0;
    while (q_head < q_tail) {
        size_t curr = queue[q_head++];
        processed++;

        /* For each task that depends on curr, decrement in-degree */
        for (size_t i = 0; i < n; i++) {
            for (size_t j = 0; j < graph->n_deps[i]; j++) {
                if (!graph->deps[i][j]) {
                    continue;
                }
                if (graph->deps[i][j]->source == graph->tasks[curr]->id) {
                    in_degree[i]--;
                    if (in_degree[i] == 0) {
                        queue[q_tail++] = i;
                    }
                }
            }
        }
    }

    bool acyclic = (processed == n);
    aegis_mutex_unlock(graph->lock);
    return acyclic;
}

aegis_status_t aegis_task_graph_validate(const aegis_task_graph_t* graph)
{
    if (!graph) {
        return AEGIS_ERR_INVALID;
    }

    aegis_mutex_lock(graph->lock);

    /* Check no self-loops */
    for (size_t i = 0; i < graph->n_tasks; i++) {
        for (size_t j = 0; j < graph->n_deps[i]; j++) {
            if (!graph->deps[i][j]) {
                continue;
            }
            if (graph->deps[i][j]->source == graph->deps[i][j]->target) {
                aegis_mutex_unlock(graph->lock);
                return AEGIS_ERR_INVALID;
            }
        }
    }

    /* Check all dependencies reference existing tasks */
    for (size_t i = 0; i < graph->n_tasks; i++) {
        if (!graph->tasks[i]) {
            continue;
        }
        for (size_t j = 0; j < graph->n_deps[i]; j++) {
            if (!graph->deps[i][j]) {
                continue;
            }
            bool source_found = false, target_found = false;
            for (size_t k = 0; k < graph->n_tasks; k++) {
                if (graph->tasks[k]) {
                    if (graph->tasks[k]->id == graph->deps[i][j]->source) {
                        source_found = true;
                    }
                    if (graph->tasks[k]->id == graph->deps[i][j]->target) {
                        target_found = true;
                    }
                }
            }
            if (!source_found || !target_found) {
                aegis_mutex_unlock(graph->lock);
                return AEGIS_ERR_INVALID;
            }
        }
    }

    /* Check no cycles */
    bool   is_acyclic = true;
    size_t n          = graph->n_tasks;
    if (n > 0) {
        size_t in_degree[AEGIS_GRAPH_MAX_TASKS] = {0};
        for (size_t i = 0; i < n; i++) {
            for (size_t j = 0; j < graph->n_deps[i]; j++) {
                if (!graph->deps[i][j]) {
                    continue;
                }
                for (size_t k = 0; k < n; k++) {
                    if (graph->tasks[k] && graph->tasks[k]->id == graph->deps[i][j]->source) {
                        in_degree[i]++;
                        break;
                    }
                }
            }
        }

        size_t queue[AEGIS_GRAPH_MAX_TASKS];
        size_t q_head = 0, q_tail = 0;
        for (size_t i = 0; i < n; i++) {
            if (in_degree[i] == 0) {
                queue[q_tail++] = i;
            }
        }

        size_t processed = 0;
        while (q_head < q_tail) {
            size_t curr = queue[q_head++];
            processed++;
            for (size_t i = 0; i < n; i++) {
                for (size_t j = 0; j < graph->n_deps[i]; j++) {
                    if (!graph->deps[i][j]) {
                        continue;
                    }
                    if (graph->tasks[curr] && graph->deps[i][j]->source == graph->tasks[curr]->id) {
                        in_degree[i]--;
                        if (in_degree[i] == 0) {
                            queue[q_tail++] = i;
                        }
                    }
                }
            }
        }
        is_acyclic = (processed == n);
    }

    aegis_mutex_unlock(graph->lock);
    return is_acyclic ? AEGIS_OK : AEGIS_ERR_INVALID;
}
