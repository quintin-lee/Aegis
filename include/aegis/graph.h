/**
 * @file graph.h
 * @brief Task Graph — DAG with add/remove task, add/remove dependency,
 *         cycle detection, topology validation, and ready task queries.
 *
 * The task graph is the central data structure for organizing tasks
 * into a Directed Acyclic Graph (DAG). It enforces:
 *   - No cycles (validated on each dependency addition)
 *   - No self-loops
 *   - No dependencies on non-existent tasks
 *   - Proper cleanup on task removal (transitive dependency update)
 *
 * Thread safety: the graph uses a recursive mutex to protect all
 * operations. Concurrent reads and writes are serialized.
 */
#ifndef AEGIS_GRAPH_H
#define AEGIS_GRAPH_H

#include "aegis/task.h"
#include "aegis/dependency.h"
#include "aegis/status.h"
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Opaque task graph handle. */
typedef struct aegis_task_graph aegis_task_graph_t;

/**
 * @brief Create an empty task graph.
 *
 * @param[out] out  Receives the graph handle. Ownership: transferred.
 * @return AEGIS_OK on success, or a negative error code.
 */
aegis_status_t aegis_task_graph_create(aegis_task_graph_t** out);

/**
 * @brief Destroy a task graph.
 *
 * Destroys all tasks owned by the graph. Safe to call with NULL.
 *
 * @param graph Handle to destroy (ownership: consumed).
 */
void aegis_task_graph_destroy(aegis_task_graph_t* graph);

/* ── Task Management ───────────────────────────────────────────────────────── */

/**
 * @brief Add a task to the graph.
 *
 * The graph takes ownership of the task. The task must not already
 * belong to another graph.
 *
 * @param graph  Task graph (borrowed).
 * @param task   Task to add (ownership: transferred).
 * @return AEGIS_OK on success, or a negative error code.
 */
aegis_status_t aegis_task_graph_add_task(aegis_task_graph_t* graph,
                                          aegis_task_t* task);

/**
 * @brief Remove a task from the graph.
 *
 * Removes the task and all its dependencies. The task is NOT destroyed —
 * ownership returns to the caller.
 *
 * @param graph  Task graph (borrowed).
 * @param task   Task to remove (ownership: NOT transferred — caller retains).
 * @return AEGIS_OK on success, or AEGIS_ERR_NOT_FOUND.
 */
aegis_status_t aegis_task_graph_remove_task(aegis_task_graph_t* graph,
                                             aegis_task_t* task);

/**
 * @brief Look up a task by ID.
 *
 * @param graph  Task graph (borrowed).
 * @param id     Task ID.
 * @return Task handle (borrowed) or NULL if not found.
 */
aegis_task_t* aegis_task_graph_get_task(const aegis_task_graph_t* graph,
                                         uint32_t id);

/* ── Dependency Management ─────────────────────────────────────────────────── */

/**
 * @brief Add a dependency: target depends on source.
 *
 * The dependency is validated for cycles before being added.
 * If a cycle would be created, the dependency is rejected.
 *
 * @param graph    Task graph (borrowed).
 * @param source   Task that must complete first (must exist in graph).
 * @param target   Task that depends on source (must exist in graph).
 * @return AEGIS_OK on success, or a negative error code.
 */
aegis_status_t aegis_task_graph_add_dependency(aegis_task_graph_t* graph,
                                                aegis_task_t* source,
                                                aegis_task_t* target);

/**
 * @brief Remove a dependency.
 *
 * @param graph    Task graph (borrowed).
 * @param source   Source task of the dependency.
 * @param target   Target task of the dependency.
 * @return AEGIS_OK on success, or AEGIS_ERR_NOT_FOUND.
 */
aegis_status_t aegis_task_graph_remove_dependency(aegis_task_graph_t* graph,
                                                   aegis_task_t* source,
                                                   aegis_task_t* target);

/* ── Query ─────────────────────────────────────────────────────────────────── */

/**
 * @brief Get all tasks currently in the READY state.
 *
 * Ready tasks have all dependencies satisfied and are eligible
 * for scheduling.
 *
 * @param graph    Task graph (borrowed).
 * @param[out] out_vector  Vector of ready task pointers (caller owns).
 * @return AEGIS_OK on success.
 */
aegis_status_t aegis_task_graph_ready_tasks(const aegis_task_graph_t* graph,
                                             aegis_task_t*** out_vector,
                                             size_t* out_count);

/**
 * @brief Get the number of tasks in the graph.
 *
 * @param graph Task graph (borrowed; may be NULL → returns 0).
 * @return Task count.
 */
size_t aegis_task_graph_task_count(const aegis_task_graph_t* graph);

/**
 * @brief Get the number of dependencies in the graph.
 *
 * @param graph Task graph (borrowed; may be NULL → returns 0).
 * @return Dependency count.
 */
size_t aegis_task_graph_dependency_count(const aegis_task_graph_t* graph);

/* ── Validation ────────────────────────────────────────────────────────────── */

/**
 * @brief Check if the graph is a valid DAG (no cycles).
 *
 * @param graph Task graph (borrowed).
 * @return true if acyclic, false otherwise.
 */
bool aegis_task_graph_is_dag(const aegis_task_graph_t* graph);

/**
 * @brief Validate the graph topology.
 *
 * Checks:
 *   - No cycles exist
 *   - All dependencies reference existing tasks
 *   - No self-loops
 *
 * @param graph Task graph (borrowed).
 * @return AEGIS_OK if valid, AEGIS_ERR_INVALID if topology is invalid.
 */
aegis_status_t aegis_task_graph_validate(const aegis_task_graph_t* graph);

#ifdef __cplusplus
}
#endif

#endif /* AEGIS_GRAPH_H */
