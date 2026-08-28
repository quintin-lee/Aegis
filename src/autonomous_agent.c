#include "aegis/autonomous_agent.h"
#include "aegis/autonomous_state.h"

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

#include "autonomous/autonomous_agent_internal.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static aegis_cancellation_token_t* get_token(aegis_autonomous_agent_t* aa)
{
    return autonomous_get_token(aa);
}

static void checkpoint_save(aegis_autonomous_agent_t* aa, const char* goal, aegis_plan_t* plan,
                            aegis_task_graph_t* graph)
{
    autonomous_checkpoint_save(aa, goal, plan, graph);
}

static aegis_status_t aa_transition(aegis_autonomous_agent_t* aa,
                                    aegis_autonomous_state_t  new_state)
{
    return autonomous_transition(aa, new_state);
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

    if (pthread_mutex_init(&aa->lock, NULL) != 0) {
        free(aa->llm_name_copy);
        free(aa);
        return AEGIS_ERR_INTERNAL;
    }
    aa->state = AEGIS_AUTO_CREATED;
    /* Transition CREATED -> INITIALIZING */
    aegis_status_t tr = autonomous_transition(aa, AEGIS_AUTO_INITIALIZING);
    if (tr != AEGIS_OK) {
        pthread_mutex_destroy(&aa->lock);
        free(aa->llm_name_copy);
        free(aa);
        return tr;
    }

    if (!cfg->cancel_token) {
        aegis_status_t rc = aegis_cancellation_token_create(&aa->owned_token);
        if (rc != AEGIS_OK) {
            pthread_mutex_destroy(&aa->lock);
            free(aa->llm_name_copy);
            free(aa);
            return rc;
        }
    }
    /* Security must be an explicit gate: if tools are present but no policy
     * is supplied, create an allow-all policy instead of bypassing. */
    if (cfg->tool_registry && !cfg->security_policy) {
        aegis_security_policy_t* allow = NULL;
        aegis_status_t           prc   = aegis_security_policy_create(&allow);
        if (prc != AEGIS_OK) {
            if (aa->owned_token) {
                aegis_cancellation_token_destroy(aa->owned_token);
            }
            pthread_mutex_destroy(&aa->lock);
            free(aa->llm_name_copy);
            free(aa);
            return prc;
        }
        aegis_capability_t all =
            (aegis_capability_t)(AEGIS_CAP_READ_FILE | AEGIS_CAP_WRITE_FILE | AEGIS_CAP_SHELL |
                                 AEGIS_CAP_NETWORK | AEGIS_CAP_RUN_PROCESS | AEGIS_CAP_ACCESS_CRED);
        (void)aegis_security_policy_add_rule(allow, "*", all);
        aa->owned_security_policy = allow;
        aa->cfg.security_policy   = allow;
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

    tr = autonomous_transition(aa, AEGIS_AUTO_READY);
    if (tr != AEGIS_OK) {
        rc = tr;
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
    if (aa->owned_security_policy) {
        aegis_security_policy_destroy(aa->owned_security_policy);
    }
    pthread_mutex_destroy(&aa->lock);
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
    if (aa->owned_security_policy) {
        aegis_security_policy_destroy(aa->owned_security_policy);
    }
    pthread_mutex_destroy(&aa->lock);
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
    /* Best-effort transition to CANCELLING if allowed. */
    (void)autonomous_transition(aa, AEGIS_AUTO_CANCELLING);
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
    aegis_checkpoint_t* ckpt = NULL;
    aegis_status_t      rc   = aegis_checkpoint_create(&ckpt);
    if (rc != AEGIS_OK) {
        return rc;
    }
    aegis_checkpoint_populate(ckpt, NULL, NULL, NULL, NULL, 0);
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
    /* Validate snapshot: check version and task count for consistency. */
    uint32_t version = aegis_checkpoint_version(ckpt);
    size_t   n_tasks = aegis_checkpoint_task_count(ckpt);
    /* Rebuild agent state from snapshot. */
    pthread_mutex_lock(&aa->lock);
    aa->recovered = true;
    /* Restore iteration from checkpoint version (version is iteration+1). */
    if (version > 0) {
        aa->iteration = version;
    }
    pthread_mutex_unlock(&aa->lock);
    /* Validate task snapshots: ensure retry counts plausible. */
    for (size_t i = 0; i < n_tasks; i++) {
        const aegis_checkpoint_task_snapshot_t* snap = aegis_checkpoint_task_snapshot(ckpt, i);
        if (snap && snap->max_retries < 0) {
            aegis_checkpoint_destroy(ckpt);
            return AEGIS_ERR_INVALID;
        }
    }
    aegis_checkpoint_destroy(ckpt);
    (void)autonomous_transition(aa, AEGIS_AUTO_RECOVERING);
    (void)autonomous_transition(aa, AEGIS_AUTO_READY);
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
    uint32_t                    loop_count     = 0;
    aegis_plan_t*               plan           = NULL;
    aegis_task_graph_t*         graph          = NULL;
    aegis_status_t              final          = AEGIS_OK;

    /* Ensure we start from READY. Restore may have left us in READY already. */
    pthread_mutex_lock(&aa->lock);
    aegis_autonomous_state_t cur           = aa->state;
    uint32_t                 start_iter    = aa->iteration;
    bool                     was_recovered = aa->recovered;
    pthread_mutex_unlock(&aa->lock);
    (void)was_recovered;
    if (cur != AEGIS_AUTO_READY) {
        if (cur == AEGIS_AUTO_RECOVERING) {
            (void)autonomous_transition(aa, AEGIS_AUTO_READY);
        } else if (cur == AEGIS_AUTO_CREATED || cur == AEGIS_AUTO_INITIALIZING) {
            (void)autonomous_transition(aa, AEGIS_AUTO_READY);
        }
    }

    aa_transition(aa, AEGIS_AUTO_PLANNING);
    aegis_status_t rc = aegis_planner_plan(aa->planner, goal_text, token, &plan);
    if (rc != AEGIS_OK) {
        (void)autonomous_transition(aa, AEGIS_AUTO_FAILED);
        final = rc;
        goto done;
    }
    rc = autonomous_transition(aa, AEGIS_AUTO_SCHEDULING);
    if (rc != AEGIS_OK) {
        final = rc;
        goto done;
    }

    /* Resume from checkpoint if recovered; otherwise start at 0. */
    loop_count = start_iter;
    for (iterations = start_iter; iterations < aa->cfg.max_iterations; iterations++) {
        loop_count++;
        if (token && aegis_cancellation_token_is_cancelled(token)) {
            final = AEGIS_ERR_CANCELLED;
            (void)autonomous_transition(aa, AEGIS_AUTO_CANCELLING);
            break;
        }

        if (graph) {
            aegis_task_graph_destroy(graph);
            graph = NULL;
        }
        /* Scheduler is a runtime service: keep it alive, just re-attach. */
        pthread_mutex_lock(&aa->lock);
        aa->iteration = loop_count;
        pthread_mutex_unlock(&aa->lock);
        rc = aegis_plan_materialize(plan, &graph);
        if (rc != AEGIS_OK) {
            (void)autonomous_transition(aa, AEGIS_AUTO_FAILED);
            final = rc;
            break;
        }
        rc = aegis_scheduler_attach(aa->scheduler, graph);
        if (rc != AEGIS_OK) {
            (void)autonomous_transition(aa, AEGIS_AUTO_FAILED);
            final = rc;
            break;
        }

        aa_transition(aa, AEGIS_AUTO_EXECUTING);
        while (1) {
            if (token && aegis_cancellation_token_is_cancelled(token)) {
                final = AEGIS_ERR_CANCELLED;
                (void)autonomous_transition(aa, AEGIS_AUTO_CANCELLING);
                break;
            }
            size_t enqueued = 0;
            aegis_scheduler_poll(aa->scheduler, &enqueued);
            aegis_task_t* task = NULL;
            rc                 = aegis_scheduler_next(aa->scheduler, &task);
            if (rc == AEGIS_ERR_NOT_FOUND) {
                break;
            }
            if (rc != AEGIS_OK) {
                (void)autonomous_transition(aa, AEGIS_AUTO_FAILED);
                final = rc;
                break;
            }
            if (aa->cfg.default_task_timeout_ns > 0) {
                long ms = (long)(aa->cfg.default_task_timeout_ns / 1000000ULL);
                aegis_task_set_timeout_ms(task, ms);
            }
            uint32_t tid = aegis_task_id(task);
            if (aegis_task_type(task) == AEGIS_TASK_TYPE_TOOL && aa->cfg.tool_registry != NULL) {
                const char*      tool_name = aegis_task_name(task);
                aegis_tool_def_t def;
                aegis_status_t   find_rc = aegis_tool_registry_find(
                    (aegis_tool_registry_t*)aa->cfg.tool_registry, tool_name, &def);
                if (find_rc != AEGIS_OK) {
                    aegis_scheduler_notify_complete(aa->scheduler, task);
                    final = find_rc;
                    (void)autonomous_transition(aa, AEGIS_AUTO_FAILED);
                    goto done;
                }
                /* Security gate is mandatory: NULL policy is not bypass. */
                aegis_security_policy_t* policy = (aegis_security_policy_t*)aa->cfg.security_policy;
                if (!policy && aa->owned_security_policy) {
                    policy = aa->owned_security_policy;
                }
                if (!policy) {
                    aegis_scheduler_notify_complete(aa->scheduler, task);
                    final = AEGIS_ERR_PERM;
                    (void)autonomous_transition(aa, AEGIS_AUTO_FAILED);
                    goto done;
                }
                rc = aegis_security_gate(policy, NULL, tool_name, def.capabilities, token);
                if (rc != AEGIS_OK) {
                    aegis_scheduler_notify_complete(aa->scheduler, task);
                    final = AEGIS_ERR_PERM;
                    (void)autonomous_transition(aa, AEGIS_AUTO_FAILED);
                    goto done;
                }
                aegis_tool_args_t* args = NULL;
                rc                      = aegis_tool_args_create(&args);
                if (rc == AEGIS_OK) {
                    rc = aegis_tool_submit(aa->executor,
                                           (aegis_tool_registry_t*)aa->cfg.tool_registry, task,
                                           tool_name, args);
                    if (rc != AEGIS_OK) {
                        aegis_tool_args_destroy(args);
                        aegis_scheduler_notify_complete(aa->scheduler, task);
                        final = rc;
                        (void)autonomous_transition(aa, AEGIS_AUTO_FAILED);
                        goto done;
                    }
                }
            } else {
                aegis_scheduler_notify_complete(aa->scheduler, task);
                final = AEGIS_ERR_NOT_FOUND;
                (void)autonomous_transition(aa, AEGIS_AUTO_FAILED);
                goto done;
            }
            if (rc != AEGIS_OK) {
                aegis_scheduler_notify_complete(aa->scheduler, task);
                final = rc;
                (void)autonomous_transition(aa, AEGIS_AUTO_FAILED);
                break;
            }
            aegis_exec_result_t eres;
            rc = aegis_executor_wait(aa->executor, tid, &eres, -1);
            aegis_scheduler_notify_complete(aa->scheduler, task);
            if (rc != AEGIS_OK) {
                (void)autonomous_transition(aa, AEGIS_AUTO_FAILED);
                final = rc;
                break;
            }
            pthread_mutex_lock(&aa->lock);
            aa->tasks_executed++;
            pthread_mutex_unlock(&aa->lock);
            tasks_executed++;

            /* Checkpointing: EXECUTING -> CHECKPOINTING -> EXECUTING */
            (void)autonomous_transition(aa, AEGIS_AUTO_CHECKPOINTING);
            checkpoint_save(aa, goal_text, plan, graph);
            (void)autonomous_transition(aa, AEGIS_AUTO_EXECUTING);

            if (eres.outcome == AEGIS_EXEC_TIMED_OUT) {
                final = AEGIS_ERR_TIMEOUT;
                (void)autonomous_transition(aa, AEGIS_AUTO_FAILED);
                break;
            }
            if (eres.outcome == AEGIS_EXEC_CANCELLED) {
                final = AEGIS_ERR_CANCELLED;
                (void)autonomous_transition(aa, AEGIS_AUTO_CANCELLING);
                break;
            }
        }

        if (final == AEGIS_ERR_CANCELLED || final == AEGIS_ERR_TIMEOUT) {
            checkpoint_save(aa, goal_text, plan, graph);
            break;
        }
        if (final != AEGIS_OK && final != AEGIS_ERR_NOT_FOUND) {
            checkpoint_save(aa, goal_text, plan, graph);
            break;
        }
        checkpoint_save(aa, goal_text, plan, graph);

        (void)autonomous_transition(aa, AEGIS_AUTO_EVALUATING);
        aegis_critique_t critique;
        memset(&critique, 0, sizeof(critique));
        rc = aegis_critic_evaluate(aa->critic, goal_text, plan, graph, token, &critique);
        if (rc == AEGIS_ERR_CANCELLED) {
            final = AEGIS_ERR_CANCELLED;
            (void)autonomous_transition(aa, AEGIS_AUTO_CANCELLING);
            break;
        }
        if (rc != AEGIS_OK) {
            (void)autonomous_transition(aa, AEGIS_AUTO_FAILED);
            final = rc;
            break;
        }
        if (critique.result == AEGIS_CRITIQUE_SUCCESS) {
            (void)autonomous_transition(aa, AEGIS_AUTO_COMPLETED);
            final = AEGIS_OK;
            break;
        }
        if (critique.result == AEGIS_CRITIQUE_REPLAN_REQUIRED ||
            critique.result == AEGIS_CRITIQUE_PARTIAL ||
            critique.result == AEGIS_CRITIQUE_FAILURE) {
            (void)autonomous_transition(aa, AEGIS_AUTO_REFLECTING);
            aegis_reflection_t* refl = NULL;
            rc                       = aegis_reflection_create(&refl, graph);
            if (rc != AEGIS_OK) {
                (void)autonomous_transition(aa, AEGIS_AUTO_FAILED);
                final = rc;
                break;
            }
            const char* feedback = aegis_reflection_feedback(refl);
            if (!feedback || feedback[0] == '\0') {
                feedback = "plan failed, need revision";
            }
            (void)autonomous_transition(aa, AEGIS_AUTO_REPLANNING);
            aegis_plan_t* new_plan = NULL;
            rc                     = aegis_replan(aa->planner, plan, feedback, token, &new_plan);
            aegis_reflection_destroy(refl);
            if (rc != AEGIS_OK) {
                (void)autonomous_transition(aa, AEGIS_AUTO_FAILED);
                final = rc;
                break;
            }
            aegis_plan_destroy(plan);
            plan = new_plan;
            (void)autonomous_transition(aa, AEGIS_AUTO_PLANNING);
            (void)autonomous_transition(aa, AEGIS_AUTO_SCHEDULING);
            final = AEGIS_OK;
            continue;
        }
        (void)autonomous_transition(aa, AEGIS_AUTO_FAILED);
        final = AEGIS_ERR_INTERNAL;
        break;
    }

    if (loop_count >= aa->cfg.max_iterations && final == AEGIS_OK) {
        final = AEGIS_ERR_MAX_ITERATIONS;
        (void)autonomous_transition(aa, AEGIS_AUTO_FAILED);
    } else if (final == AEGIS_ERR_CANCELLED) {
        (void)autonomous_transition(aa, AEGIS_AUTO_CANCELLED);
    } else if (final != AEGIS_OK) {
        /* Ensure FAILED if not already terminal. */
        pthread_mutex_lock(&aa->lock);
        aegis_autonomous_state_t st = aa->state;
        pthread_mutex_unlock(&aa->lock);
        if (st != AEGIS_AUTO_FAILED && st != AEGIS_AUTO_CANCELLED && st != AEGIS_AUTO_COMPLETED) {
            (void)autonomous_transition(aa, AEGIS_AUTO_FAILED);
        }
    }

done:
    if (graph) {
        aegis_task_graph_destroy(graph);
    }
    if (plan) {
        aegis_plan_destroy(plan);
    }
    if (!aa->scheduler) {
        (void)aegis_scheduler_create(&aa->scheduler);
    }

    if (out_result) {
        pthread_mutex_lock(&aa->lock);
        bool rec = aa->recovered;
        pthread_mutex_unlock(&aa->lock);
        out_result->final_status              = final;
        out_result->iterations                = loop_count;
        out_result->tasks_executed            = tasks_executed;
        out_result->recovered_from_checkpoint = rec;
    }
    pthread_mutex_lock(&aa->lock);
    aa->recovered = false;
    /* Reset iteration for next fresh goal; crash recovery already consumed it. */
    aa->iteration = 0;
    pthread_mutex_unlock(&aa->lock);
    return final;
}
