#define _POSIX_C_SOURCE 200809L
#include "autonomous_agent_internal.h"
#include "aegis/tool/tool.h"
#include "aegis/security/security.h"
#include <string.h>

aegis_status_t autonomous_execute(aegis_autonomous_agent_t*   agent,
                                  aegis_autonomous_runtime_t* runtime)
{
    if (!agent || !runtime) {
        return AEGIS_ERR_INVALID;
    }
    if (!runtime->plan) {
        return AEGIS_ERR_INVALID;
    }
    if (!agent->scheduler || !agent->executor) {
        return AEGIS_ERR_INVALID;
    }

    aegis_cancellation_token_t* token = autonomous_get_token(agent);
    if (token && aegis_cancellation_token_is_cancelled(token)) {
        return AEGIS_ERR_CANCELLED;
    }

    // Materialize graph if needed
    if (runtime->graph) {
        aegis_task_graph_destroy(runtime->graph);
        runtime->graph = NULL;
    }
    aegis_task_graph_t* graph = NULL;
    aegis_status_t      rc    = aegis_plan_materialize(runtime->plan, &graph);
    if (rc != AEGIS_OK) {
        return rc;
    }
    runtime->graph = graph;

    rc = aegis_scheduler_attach(agent->scheduler, graph);
    if (rc != AEGIS_OK) {
        return rc;
    }

    // Execute tasks one by one via scheduler -> executor -> tool
    while (1) {
        if (token && aegis_cancellation_token_is_cancelled(token)) {
            return AEGIS_ERR_CANCELLED;
        }
        size_t enqueued = 0;
        aegis_scheduler_poll(agent->scheduler, &enqueued);
        aegis_task_t* task = NULL;
        rc                 = aegis_scheduler_next(agent->scheduler, &task);
        if (rc == AEGIS_ERR_NOT_FOUND) {
            break;
        }
        if (rc != AEGIS_OK) {
            return rc;
        }

        if (agent->cfg.default_task_timeout_ns > 0) {
            long ms = (long)(agent->cfg.default_task_timeout_ns / 1000000ULL);
            aegis_task_set_timeout_ms(task, ms);
        }
        uint32_t tid = aegis_task_id(task);
        if (aegis_task_type(task) == AEGIS_TASK_TYPE_TOOL && agent->cfg.tool_registry != NULL) {
            const char*      tool_name = aegis_task_name(task);
            aegis_tool_def_t def;
            aegis_status_t   find_rc = aegis_tool_registry_find(
                (aegis_tool_registry_t*)agent->cfg.tool_registry, tool_name, &def);
            if (find_rc != AEGIS_OK) {
                aegis_scheduler_notify_complete(agent->scheduler, task);
                return find_rc;
            }
            aegis_security_policy_t* policy = (aegis_security_policy_t*)agent->cfg.security_policy;
            if (!policy && agent->owned_security_policy) {
                policy = agent->owned_security_policy;
            }
            if (!policy) {
                aegis_scheduler_notify_complete(agent->scheduler, task);
                return AEGIS_ERR_PERM;
            }
            rc = aegis_security_gate(policy, NULL, tool_name, def.capabilities, token);
            if (rc != AEGIS_OK) {
                aegis_scheduler_notify_complete(agent->scheduler, task);
                return AEGIS_ERR_PERM;
            }
            aegis_tool_args_t* args = NULL;
            rc                      = aegis_tool_args_create(&args);
            if (rc != AEGIS_OK) {
                aegis_scheduler_notify_complete(agent->scheduler, task);
                return rc;
            }
            rc =
                aegis_tool_submit(agent->executor, (aegis_tool_registry_t*)agent->cfg.tool_registry,
                                  task, tool_name, args);
            if (rc != AEGIS_OK) {
                aegis_tool_args_destroy(args);
                aegis_scheduler_notify_complete(agent->scheduler, task);
                return rc;
            }
        } else {
            aegis_scheduler_notify_complete(agent->scheduler, task);
            return AEGIS_ERR_NOT_FOUND;
        }

        aegis_exec_result_t eres;
        rc = aegis_executor_wait(agent->executor, tid, &eres, -1);
        aegis_scheduler_notify_complete(agent->scheduler, task);
        if (rc != AEGIS_OK) {
            return rc;
        }

        pthread_mutex_lock(&agent->lock);
        agent->tasks_executed++;
        pthread_mutex_unlock(&agent->lock);
        runtime->tasks_executed++;
        agent->tasks_executed = (uint32_t)runtime->tasks_executed;

        // checkpoint per task: EXECUTING -> CHECKPOINTING -> EXECUTING
        (void)autonomous_transition(agent, AEGIS_AUTO_CHECKPOINTING);
        autonomous_checkpoint_save(agent, runtime->goal, runtime->plan, runtime->graph);
        (void)autonomous_transition(agent, AEGIS_AUTO_EXECUTING);

        if (eres.outcome == AEGIS_EXEC_TIMED_OUT) {
            return AEGIS_ERR_TIMEOUT;
        }
        if (eres.outcome == AEGIS_EXEC_CANCELLED) {
            return AEGIS_ERR_CANCELLED;
        }
        if (eres.outcome == AEGIS_EXEC_FAILED) {
            runtime->tasks_failed++;
            // Task failure is not immediately loop failure — let evaluator decide.
            // But if task graph expects success, continue to evaluation.
        }
    }
    return AEGIS_OK;
}
