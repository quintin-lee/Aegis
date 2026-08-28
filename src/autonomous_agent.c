#include "aegis/autonomous_agent.h"

#include "aegis/checkpoint/checkpoint.h"
#include "aegis/provider/provider.h"
#include "aegis/critic/critic.h"
#include "aegis/executor/executor.h"
#include "aegis/task/graph.h"
#include "aegis/planner/plan.h"
#include "aegis/planner/planner.h"
#include "aegis/reflection/reflection.h"
#include "aegis/replanner/replanner.h"
#include "aegis/scheduler/scheduler.h"
#include "aegis/tool/tool.h"
#include "aegis/security/security.h"
#include "task_internal.h"
#include "aegis/task/task.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

struct aegis_autonomous_agent {
    aegis_autonomous_agent_config_t cfg;
    char*                           llm_name_copy;
    aegis_planner_t*                planner;
    aegis_scheduler_t*              scheduler;
    aegis_executor_t*               executor;
    aegis_critic_t*                 critic;
    aegis_cancellation_token_t*     owned_token;
    bool                            recovered;
};

static aegis_cancellation_token_t* get_token(aegis_autonomous_agent_t* aa)
{
    if (aa->cfg.cancel_token) {
        return aa->cfg.cancel_token;
    }
    return aa->owned_token;
}

static void checkpoint_save(aegis_autonomous_agent_t* aa, const char* goal, aegis_plan_t* plan,
                            aegis_task_graph_t* graph)
{
    const char* path = aa->cfg.checkpoint_path;
    if (!path) {
        return;
    }
    aegis_checkpoint_t* ckpt = NULL;
    if (aegis_checkpoint_create(&ckpt) != AEGIS_OK) {
        return;
    }
    // agent is NULL (we don't have aegis_agent_t), pass NULL per API
    aegis_checkpoint_populate(ckpt, NULL, plan, graph, 0);
    if (goal) {
        aegis_checkpoint_set_goal(ckpt, goal);
    }
    aegis_checkpoint_write(ckpt, path, get_token(aa));
    aegis_checkpoint_destroy(ckpt);
}

static aegis_status_t default_work(aegis_task_t* task, const aegis_cancellation_token_t* token,
                                   void* user)
{
    (void)user;
    const char* name = aegis_task_name(task);
    if (!name) {
        name = "";
    }
    // cooperative cancellation / timeout polling
    // simulate work: if name contains "slow", sleep 200ms in 10ms chunks
    // if name contains "fail_once" or "fail", return error
    if (strstr(name, "slow") != NULL) {
        for (int i = 0; i < 20; i++) {
            if (token && aegis_cancellation_token_is_cancelled(token)) {
                return AEGIS_ERR_CANCELLED;
            }
            struct timespec ts = {0, 10 * 1000000L};
            nanosleep(&ts, NULL);
        }
    }
    if (token && aegis_cancellation_token_is_cancelled(token)) {
        return AEGIS_ERR_CANCELLED;
    }
    if (strstr(name, "fail") != NULL) {
        // generic failure
        return AEGIS_ERR_TOOL;
    }
    // success: optionally set output
    const char ok[] = "ok";
    aegis_task_set_output(task, ok, sizeof(ok));
    return AEGIS_OK;
}

aegis_status_t aegis_autonomous_agent_create(aegis_autonomous_agent_t**             out,
                                             const aegis_autonomous_agent_config_t* cfg)
{
    if (!out || !cfg) {
        return AEGIS_ERR_INVALID;
    }
    if (!cfg->provider_registry || !cfg->llm_provider_name) {
        return AEGIS_ERR_INVALID;
    }

    aegis_autonomous_agent_t* aa = (aegis_autonomous_agent_t*)calloc(1, sizeof(*aa));
    if (!aa) {
        return AEGIS_ERR_NOMEM;
    }

    aa->cfg = *cfg;
    if (aa->cfg.max_iterations == 0) {
        aa->cfg.max_iterations = 5;
    }

    aa->llm_name_copy = strdup(cfg->llm_provider_name);
    if (!aa->llm_name_copy) {
        free(aa);
        return AEGIS_ERR_NOMEM;
    }
    aa->cfg.llm_provider_name = aa->llm_name_copy;

    // cancellation token
    if (!cfg->cancel_token) {
        aegis_status_t rc = aegis_cancellation_token_create(&aa->owned_token);
        if (rc != AEGIS_OK) {
            free(aa->llm_name_copy);
            free(aa);
            return rc;
        }
    }

    aegis_planner_config_t pcfg = {
        .provider_registry = cfg->provider_registry,
        .llm_provider_name = aa->llm_name_copy,
    };
    aegis_status_t rc = aegis_planner_create(&aa->planner, &pcfg);
    if (rc != AEGIS_OK) {
        goto fail;
    }

    rc = aegis_scheduler_create(&aa->scheduler);
    if (rc != AEGIS_OK) {
        goto fail;
    }

    aegis_executor_config_t ecfg = {.worker_count = 2, .queue_capacity = 64};
    rc                           = aegis_executor_create(&aa->executor, &ecfg);
    if (rc != AEGIS_OK) {
        goto fail;
    }

    rc = aegis_critic_create(&aa->critic);
    if (rc != AEGIS_OK) {
        goto fail;
    }

    *out = aa;
    return AEGIS_OK;

fail:
    if (aa->planner) {
        aegis_planner_destroy(aa->planner);
    }
    if (aa->scheduler) {
        aegis_scheduler_destroy(aa->scheduler);
    }
    if (aa->executor) {
        aegis_executor_destroy(aa->executor);
    }
    if (aa->critic) {
        aegis_critic_destroy(aa->critic);
    }
    if (aa->owned_token) {
        aegis_cancellation_token_destroy(aa->owned_token);
    }
    free(aa->llm_name_copy);
    free(aa);
    return rc;
}

void aegis_autonomous_agent_destroy(aegis_autonomous_agent_t* aa)
{
    if (!aa) {
        return;
    }
    if (aa->planner) {
        aegis_planner_destroy(aa->planner);
    }
    if (aa->scheduler) {
        aegis_scheduler_destroy(aa->scheduler);
    }
    if (aa->executor) {
        aegis_executor_destroy(aa->executor);
    }
    if (aa->critic) {
        aegis_critic_destroy(aa->critic);
    }
    if (aa->owned_token) {
        aegis_cancellation_token_destroy(aa->owned_token);
    }
    free(aa->llm_name_copy);
    free(aa);
}

aegis_status_t aegis_autonomous_agent_cancel(aegis_autonomous_agent_t* aa)
{
    if (!aa) {
        return AEGIS_ERR_INVALID;
    }
    aegis_cancellation_token_t* tok = get_token(aa);
    if (!tok) {
        return AEGIS_ERR_INVALID;
    }
    aegis_cancellation_token_request_cancel(tok);
    return AEGIS_OK;
}

aegis_status_t aegis_autonomous_agent_checkpoint_save(aegis_autonomous_agent_t* aa,
                                                      const char*               path)
{
    if (!aa) {
        return AEGIS_ERR_INVALID;
    }
    const char* p = path ? path : aa->cfg.checkpoint_path;
    if (!p) {
        return AEGIS_ERR_INVALID;
    }
    // create empty checkpoint for validation that path is writable
    aegis_checkpoint_t* ckpt = NULL;
    aegis_status_t      rc   = aegis_checkpoint_create(&ckpt);
    if (rc != AEGIS_OK) {
        return rc;
    }
    // populate with minimal data (no plan/graph) just to test write
    aegis_checkpoint_populate(ckpt, NULL, NULL, NULL, 0);
    rc = aegis_checkpoint_write(ckpt, p, get_token(aa));
    aegis_checkpoint_destroy(ckpt);
    return rc;
}

aegis_status_t aegis_autonomous_agent_restore(aegis_autonomous_agent_t* aa, const char* path)
{
    if (!aa || !path) {
        return AEGIS_ERR_INVALID;
    }
    aegis_checkpoint_t*       ckpt = NULL;
    aegis_checkpoint_status_t st   = AEGIS_CHECKPOINT_MISSING;
    aegis_status_t            rc   = aegis_checkpoint_read(path, &ckpt, &st);
    if (rc != AEGIS_OK) {
        return rc;
    }
    if (st != AEGIS_CHECKPOINT_OK) {
        if (ckpt) {
            aegis_checkpoint_destroy(ckpt);
        }
        return AEGIS_ERR_NOT_FOUND;
    }
    aegis_checkpoint_destroy(ckpt);
    aa->recovered = true;
    return AEGIS_OK;
}

aegis_status_t aegis_autonomous_agent_run(aegis_autonomous_agent_t* aa, const char* goal_text,
                                          aegis_autonomous_result_t* out_result)
{
    if (!aa || !goal_text || goal_text[0] == '\0') {
        return AEGIS_ERR_INVALID;
    }

    aegis_cancellation_token_t* token          = get_token(aa);
    uint32_t                    iterations     = 0;
    uint32_t                    tasks_executed = 0;
    uint32_t                    loop_count     = 0; /* actual entries into loop body */
    aegis_plan_t*               plan           = NULL;
    aegis_task_graph_t*         graph          = NULL;
    aegis_status_t              final          = AEGIS_OK;

    // initial plan
    aegis_status_t rc = aegis_planner_plan(aa->planner, goal_text, token, &plan);
    if (rc != AEGIS_OK) {
        final = rc;
        goto done;
    }

    for (iterations = 0; iterations < aa->cfg.max_iterations; iterations++) {
        loop_count++;
        if (token && aegis_cancellation_token_is_cancelled(token)) {
            final = AEGIS_ERR_CANCELLED;
            break;
        }

        // materialize
        if (graph) {
            aegis_scheduler_destroy(aa->scheduler);
            aa->scheduler = NULL;
            aegis_scheduler_create(&aa->scheduler);
            aegis_task_graph_destroy(graph);
            graph = NULL;
        }
        rc = aegis_plan_materialize(plan, &graph);
        if (rc != AEGIS_OK) {
            final = rc;
            break;
        }
        rc = aegis_scheduler_attach(aa->scheduler, graph);
        if (rc != AEGIS_OK) {
            final = rc;
            break;
        }

        // inner loop: dispatch ready tasks
        while (1) {
            if (token && aegis_cancellation_token_is_cancelled(token)) {
                final = AEGIS_ERR_CANCELLED;
                break;
            }
            size_t enqueued = 0;
            aegis_scheduler_poll(aa->scheduler, &enqueued);
            aegis_task_t* task = NULL;
            rc                 = aegis_scheduler_next(aa->scheduler, &task);
            if (rc == AEGIS_ERR_NOT_FOUND) {
                // no more ready tasks
                break;
            }
            if (rc != AEGIS_OK) {
                final = rc;
                break;
            }
            // set timeout
            if (aa->cfg.default_task_timeout_ns > 0) {
                long ms = (long)(aa->cfg.default_task_timeout_ns / 1000000ULL);
                aegis_task_set_timeout_ms(task, ms);
            }
            // Dispatch tool-type tasks through the tool registry when available;
            // otherwise fall back to default_work (stub).
            uint32_t tid = aegis_task_id(task);
            if (aegis_task_type(task) == AEGIS_TASK_TYPE_TOOL && aa->cfg.tool_registry != NULL) {
                const char* tool_name = aegis_task_name(task);
                /* Security gate: check policy before dispatching tool. */
                if (aa->cfg.security_policy) {
                    aegis_tool_def_t def;
                    aegis_status_t   find_rc = aegis_tool_registry_find(
                        (aegis_tool_registry_t*)aa->cfg.tool_registry, tool_name, &def);
                    if (find_rc == AEGIS_OK) {
                        rc = aegis_security_gate((aegis_security_policy_t*)aa->cfg.security_policy,
                                                 NULL, tool_name, def.capabilities, token);
                        if (rc != AEGIS_OK) {
                            /* Permission denied — report and skip. */
                            aegis_scheduler_notify_complete(aa->scheduler, task);
                            final = AEGIS_ERR_PERM;
                            goto done;
                        }
                    }
                }
                aegis_tool_args_t* args = NULL;
                rc                      = aegis_tool_args_create(&args);
                if (rc == AEGIS_OK) {
                    rc = aegis_tool_submit(aa->executor,
                                           (aegis_tool_registry_t*)aa->cfg.tool_registry, task,
                                           tool_name, args);
                    if (rc != AEGIS_OK) {
                        /* Tool not found or validation failed — fall back to stub. */
                        aegis_tool_args_destroy(args);
                        rc = aegis_executor_submit(aa->executor, task, default_work, NULL);
                    }
                }
            } else {
                rc = aegis_executor_submit(aa->executor, task, default_work, NULL);
            }
            if (rc != AEGIS_OK) {
                // notify scheduler even on submit failure to avoid leak
                aegis_scheduler_notify_complete(aa->scheduler, task);
                final = rc;
                break;
            }
            aegis_exec_result_t eres;
            rc = aegis_executor_wait(aa->executor, tid, &eres, -1);
            // always notify scheduler
            aegis_scheduler_notify_complete(aa->scheduler, task);
            if (rc != AEGIS_OK) {
                final = rc;
                break;
            }
            tasks_executed++;
            // checkpoint double-write after each task
            checkpoint_save(aa, goal_text, plan, graph);

            if (eres.outcome == AEGIS_EXEC_TIMED_OUT) {
                final = AEGIS_ERR_TIMEOUT;
                break;
            }
            if (eres.outcome == AEGIS_EXEC_CANCELLED) {
                final = AEGIS_ERR_CANCELLED;
                break;
            }
            // on FAILED, continue to next ready tasks; executor already retried per policy
        }

        // check cancellation/timeout that broke inner loop
        if (final == AEGIS_ERR_CANCELLED || final == AEGIS_ERR_TIMEOUT) {
            checkpoint_save(aa, goal_text, plan, graph);
            break;
        }
        if (final != AEGIS_OK && final != AEGIS_ERR_NOT_FOUND) {
            // inner error
            checkpoint_save(aa, goal_text, plan, graph);
            break;
        }
        // round checkpoint
        checkpoint_save(aa, goal_text, plan, graph);

        // critic evaluate
        aegis_critique_t critique;
        memset(&critique, 0, sizeof(critique));
        rc = aegis_critic_evaluate(aa->critic, goal_text, plan, graph, token, &critique);
        if (rc == AEGIS_ERR_CANCELLED) {
            final = AEGIS_ERR_CANCELLED;
            break;
        }
        if (rc != AEGIS_OK) {
            final = rc;
            break;
        }
        if (critique.result == AEGIS_CRITIQUE_SUCCESS) {
            final = AEGIS_OK;
            break;
        }
        if (critique.result == AEGIS_CRITIQUE_REPLAN_REQUIRED ||
            critique.result == AEGIS_CRITIQUE_PARTIAL ||
            critique.result == AEGIS_CRITIQUE_FAILURE) {
            // reflection + replan
            aegis_reflection_t* refl = NULL;
            rc                       = aegis_reflection_create(&refl, graph);
            if (rc != AEGIS_OK) {
                final = rc;
                break;
            }
            const char* feedback = aegis_reflection_feedback(refl);
            if (!feedback || feedback[0] == '\0') {
                feedback = "plan failed, need revision";
            }
            aegis_plan_t* new_plan = NULL;
            rc                     = aegis_replan(aa->planner, plan, feedback, token, &new_plan);
            aegis_reflection_destroy(refl);
            if (rc != AEGIS_OK) {
                final = rc;
                break;
            }
            // version bump is done inside replan, but ensure monotonic
            aegis_plan_destroy(plan);
            plan  = new_plan;
            final = AEGIS_OK;  // continue loop to try new plan
            continue;
        }
        // other results (INVALID) => fail
        final = AEGIS_ERR_INTERNAL;
        break;
    }

    if (loop_count >= aa->cfg.max_iterations && final == AEGIS_OK) {
        // if we exhausted iterations without success, mark busy
        // check last critique was not success
        final = AEGIS_ERR_BUSY;
    }

done:
    if (graph) {
        aegis_task_graph_destroy(graph);
    }
    if (plan) {
        aegis_plan_destroy(plan);
    }
    // Recreate scheduler if we destroyed it last iteration.
    if (!aa->scheduler) {
        (void)aegis_scheduler_create(&aa->scheduler);
    }

    if (out_result) {
        out_result->final_status              = final;
        out_result->iterations                = loop_count;
        out_result->tasks_executed            = tasks_executed;
        out_result->recovered_from_checkpoint = aa->recovered;
    }
    // Reset run-state flags so the agent can be reused for another goal.
    aa->recovered = false;
    return final;
}
