/**
 * @file graph_internal.h
 * @brief Internal task graph struct layout.
 *
 * NOT part of the public API.
 */
#ifndef AEGIS_GRAPH_INTERNAL_H
#define AEGIS_GRAPH_INTERNAL_H

#include "aegis/task/graph.h"
#include "aegis/common/mutex.h"
#include "aegis/common/vector.h"
#include <stdint.h>
#include <stdbool.h>

/** Maximum tasks in a graph. */
#define AEGIS_GRAPH_MAX_TASKS 1024
/** Maximum dependencies per task. */
#define AEGIS_GRAPH_MAX_DEPS_PER_TASK 256

/** Internal task graph structure. */
struct aegis_task_graph {
    /* Task storage */
    aegis_task_t* tasks[AEGIS_GRAPH_MAX_TASKS];
    size_t        n_tasks;

    /* Dependency storage — adjacency list: deps[i] = deps of task i */
    aegis_dependency_t* deps[AEGIS_GRAPH_MAX_TASKS][AEGIS_GRAPH_MAX_DEPS_PER_TASK];
    size_t              n_deps[AEGIS_GRAPH_MAX_TASKS];
    size_t              n_dependencies;

    /* Next task ID allocator */
    uint32_t next_id;

    /* Concurrency */
    aegis_mutex_t* lock;
};

#endif /* AEGIS_GRAPH_INTERNAL_H */
