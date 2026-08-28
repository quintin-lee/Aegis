/**
 * @file autonomous_agent_internal.h
 * @brief Internal definitions for autonomous runtime modules.
 *
 * Not part of public API. All autonomous submodules include this header
 * to share the agent struct and internal helpers. Public header
 * include/aegis/autonomous_agent.h exposes only the opaque handle.
 */
#ifndef AEGIS_AUTONOMOUS_INTERNAL_H
#define AEGIS_AUTONOMOUS_INTERNAL_H

#include "aegis/autonomous_agent.h"
#include "aegis/autonomous_state.h"
#include "aegis/planner/planner.h"
#include "aegis/scheduler/scheduler.h"
#include "aegis/executor/executor.h"
#include "aegis/critic/critic.h"
#include "aegis/checkpoint/checkpoint.h"
#include "aegis/task/graph.h"
#include "aegis/common/cancellation/cancellation.h"

#include <pthread.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

struct aegis_autonomous_agent {
    aegis_autonomous_agent_config_t cfg;
    char*                           llm_name_copy;
    aegis_planner_t*                planner;
    aegis_scheduler_t*              scheduler;
    aegis_executor_t*               executor;
    aegis_critic_t*                 critic;
    aegis_cancellation_token_t*     owned_token;
    bool                            recovered;
    aegis_autonomous_state_t        state;
    pthread_mutex_t                 lock;
    uint32_t                        iteration;
    uint32_t                        tasks_executed;
};

/** Return effective cancellation token (borrowed). */
aegis_cancellation_token_t* autonomous_get_token(aegis_autonomous_agent_t* aa);

/** State transition with validation, lock, and post-unlock publish. */
aegis_status_t autonomous_transition(aegis_autonomous_agent_t* aa,
                                     aegis_autonomous_state_t  target);

/** Check if a transition is allowed per table (no lock). */
bool autonomous_transition_allowed(aegis_autonomous_state_t from,
                                   aegis_autonomous_state_t to);

/* Lifecycle helpers */
aegis_status_t autonomous_lifecycle_init(aegis_autonomous_agent_t* aa);
void           autonomous_lifecycle_cleanup(aegis_autonomous_agent_t* aa);

/* Checkpoint helpers */
void autonomous_checkpoint_save(aegis_autonomous_agent_t* aa,
                                const char*               goal,
                                aegis_plan_t*             plan,
                                aegis_task_graph_t*       graph);

/* Planning / Execution / Evaluation / Reflection / Replanning stubs */
aegis_status_t autonomous_planning_run(aegis_autonomous_agent_t* aa,
                                       const char*               goal,
                                       aegis_plan_t**            out_plan);
aegis_status_t autonomous_execution_run(aegis_autonomous_agent_t* aa,
                                        aegis_plan_t*             plan,
                                        aegis_task_graph_t**      out_graph,
                                        const char*               goal);
aegis_status_t autonomous_evaluation_run(aegis_autonomous_agent_t* aa,
                                         const char*               goal,
                                         aegis_plan_t*             plan,
                                         aegis_task_graph_t*       graph,
                                         int*                      out_result);
aegis_status_t autonomous_reflection_run(aegis_autonomous_agent_t* aa,
                                         aegis_task_graph_t*       graph,
                                         const char**              out_feedback);
aegis_status_t autonomous_replanning_run(aegis_autonomous_agent_t* aa,
                                         aegis_plan_t*             old_plan,
                                         const char*               feedback,
                                         aegis_plan_t**            out_new_plan);

#ifdef __cplusplus
}
#endif

#endif /* AEGIS_AUTONOMOUS_INTERNAL_H */
