/**
 * @file autonomous_agent_internal.h
 * @brief Internal definitions for autonomous runtime modules.
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
#include "aegis/reflection/reflection.h"
#include "aegis/replanner/replanner.h"

#include <pthread.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Runtime context — owns Plan/Graph/Critique/Reflection state. */
typedef struct aegis_autonomous_runtime {
    char                     goal[512];
    aegis_plan_t*            plan;
    aegis_task_graph_t*      graph;
    uint64_t                 iteration;
    uint64_t                 checkpoint_sequence;
    aegis_critique_t         last_critique;
    aegis_reflection_t*      last_reflection;
    char*                    replan_feedback;
    uint64_t                 tasks_executed;
    uint64_t                 tasks_failed;
    uint64_t                 tasks_retried;
    uint64_t                 plans_generated;
    uint64_t                 replans;
    bool                     recovering;
    aegis_cancellation_token_t* token;
} aegis_autonomous_runtime_t;

struct aegis_autonomous_agent {
    aegis_autonomous_agent_config_t cfg;
    char*                           llm_name_copy;
    aegis_planner_t*                planner;
    aegis_scheduler_t*              scheduler;
    aegis_executor_t*               executor;
    aegis_critic_t*                 critic;
    aegis_cancellation_token_t*     owned_token;
    aegis_security_policy_t*        owned_security_policy;
    bool                            recovered;
    aegis_autonomous_state_t        state;
    pthread_mutex_t                 lock;
    uint32_t                        iteration;
    uint32_t                        tasks_executed;
    aegis_autonomous_runtime_t*     runtime;
};

/** Return effective cancellation token (borrowed). */
aegis_cancellation_token_t* autonomous_get_token(aegis_autonomous_agent_t* aa);

/** State transition with validation, lock, and post-unlock publish. */
aegis_status_t autonomous_transition(aegis_autonomous_agent_t* aa,
                                     aegis_autonomous_state_t  target);
bool autonomous_transition_allowed(aegis_autonomous_state_t from,
                                   aegis_autonomous_state_t to);

/* Runtime lifecycle */
aegis_status_t autonomous_runtime_create(aegis_autonomous_runtime_t** out);
void           autonomous_runtime_destroy(aegis_autonomous_runtime_t* rt);
void           autonomous_runtime_reset(aegis_autonomous_runtime_t* rt);

/* Lifecycle helpers */
aegis_status_t autonomous_lifecycle_init(aegis_autonomous_agent_t* aa);
void           autonomous_lifecycle_cleanup(aegis_autonomous_agent_t* aa);

/* Checkpoint helpers */
void autonomous_checkpoint_save(aegis_autonomous_agent_t* aa,
                                const char*               goal,
                                aegis_plan_t*             plan,
                                aegis_task_graph_t*       graph);
aegis_status_t autonomous_checkpoint_restore(aegis_autonomous_agent_t* aa,
                                             const char*               path);

/* Core loop */
aegis_status_t autonomous_loop_run(aegis_autonomous_agent_t* agent,
                                   aegis_autonomous_runtime_t* runtime,
                                   const char* goal,
                                   aegis_autonomous_result_t* out_result);

/* Submodules */
aegis_status_t autonomous_plan(aegis_autonomous_agent_t* agent,
                               aegis_autonomous_runtime_t* runtime,
                               const char* goal);
aegis_status_t autonomous_execute(aegis_autonomous_agent_t* agent,
                                  aegis_autonomous_runtime_t* runtime);
aegis_status_t autonomous_evaluate(aegis_autonomous_agent_t* agent,
                                   aegis_autonomous_runtime_t* runtime);
aegis_status_t autonomous_reflect(aegis_autonomous_agent_t* agent,
                                  aegis_autonomous_runtime_t* runtime);
aegis_status_t autonomous_replan(aegis_autonomous_agent_t* agent,
                                 aegis_autonomous_runtime_t* runtime);

#ifdef __cplusplus
}
#endif

#endif /* AEGIS_AUTONOMOUS_INTERNAL_H */
