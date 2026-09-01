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
    if (autonomous_runtime_create(&aa->runtime) != AEGIS_OK) {
        pthread_mutex_destroy(&aa->lock);
        free(aa->llm_name_copy);
        free(aa);
        return AEGIS_ERR_NOMEM;
    }
    aa->runtime->token = NULL;
    aa->iteration      = 0;
    aa->tasks_executed = 0;
    aa->state          = AEGIS_AUTO_CREATED;
    /* Transition CREATED -> INITIALIZING */
    aegis_status_t tr = autonomous_transition(aa, AEGIS_AUTO_INITIALIZING);
    if (tr != AEGIS_OK) {
        autonomous_runtime_destroy(aa->runtime);
        pthread_mutex_destroy(&aa->lock);
        free(aa->llm_name_copy);
        free(aa);
        return tr;
    }

    if (!cfg->cancel_token) {
        aegis_status_t rc = aegis_cancellation_token_create(&aa->owned_token);
        if (rc != AEGIS_OK) {
            autonomous_runtime_destroy(aa->runtime);
            pthread_mutex_destroy(&aa->lock);
            free(aa->llm_name_copy);
            free(aa);
            return rc;
        }
    }
    aa->runtime->token = autonomous_get_token(aa);

    /* Security must be an explicit gate: if tools are present but no policy
     * is supplied, create an allow-all policy instead of bypassing. */
    if (cfg->tool_registry && !cfg->security_policy) {
        aegis_security_policy_t* allow = NULL;
        aegis_status_t           prc   = aegis_security_policy_create(&allow);
        if (prc != AEGIS_OK) {
            if (aa->owned_token) {
                aegis_cancellation_token_destroy(aa->owned_token);
            }
            autonomous_runtime_destroy(aa->runtime);
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
    if (aa->runtime) {
        autonomous_runtime_destroy(aa->runtime);
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
    if (aa->runtime) {
        autonomous_runtime_destroy(aa->runtime);
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
    return autonomous_checkpoint_restore(aa, path);
}

aegis_status_t aegis_autonomous_agent_run(aegis_autonomous_agent_t* aa, const char* goal_text,
                                          aegis_autonomous_result_t* out_result)
{
    if (!aa || !goal_text || goal_text[0] == '\0') {
        return AEGIS_ERR_INVALID;
    }
    if (!aa->runtime) {
        return AEGIS_ERR_INVALID;
    }
    // Sync runtime goal and token before loop
    strncpy(aa->runtime->goal, goal_text, sizeof(aa->runtime->goal) - 1);
    aa->runtime->token = autonomous_get_token(aa);
    // Delegate to loop orchestrator; loop owns iteration/plan/graph ownership
    aegis_status_t rc = autonomous_loop_run(aa, aa->runtime, goal_text, out_result);
    // On return, runtime retains plan/graph for inspection until next run or destroy;
    // reset iteration for fresh goal if not recovered — handled inside loop.
    // Ensure scheduler is alive (loop may have left it attached)
    if (!aa->scheduler) {
        (void)aegis_scheduler_create(&aa->scheduler);
    }
    return rc;
}
