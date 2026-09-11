/**
 * @file autonomous_agent.c
 * @brief Public autonomous agent lifecycle: create/destroy/run/cancel plus
 * checkpoint save/restore entry points. Composes the phase submodules
 * (planning/execution/evaluation/reflection/replanning); owns planner,
 * scheduler, executor, critic and the cancellation token tree.
 */
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

/**
 * @brief Resolve the effective cancellation token for this agent.
 *
 * Thin wrapper over aegis_autonomous_get_token() kept local so the public
 * entry points below share one call site.
 *
 * @param[in] aa  Agent instance; must be non-NULL.
 *
 * @return Borrowed token pointer, or NULL when the agent carries none.
 */
static aegis_cancellation_token_t* get_token(aegis_autonomous_agent_t* aa)
{
    return aegis_autonomous_get_token(aa);
}

/**
 * @brief Create an autonomous agent and its planner/scheduler/executor/critic.
 *
 * Copies the caller config (strdup'ing the LLM provider name and defaulting
 * max_iterations to 5), creates the runtime, an owned cancellation token when
 * the config carries none, an allow-all security policy when tools exist but
 * no policy was supplied, then the four submodules. Leaves the agent in READY
 * via CREATED -> INITIALIZING -> READY; any failure unwinds everything built.
 *
 * @param[out] out  Receives the new agent on success; untouched on failure.
 * @param[in]  cfg  Agent config; requires provider_registry + llm_provider_name.
 *
 * @return AEGIS_OK on success; AEGIS_ERR_INVALID/NOMEM/INTERNAL otherwise.
 *
 * Ownership: caller owns *out and must call
 * aegis_autonomous_agent_destroy(). Thread-safe: create-only, no sharing yet.
 */
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
    if (aegis_autonomous_runtime_create(&aa->runtime) != AEGIS_OK) {
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
    aegis_status_t tr = aegis_autonomous_transition(aa, AEGIS_AUTO_INITIALIZING);
    if (tr != AEGIS_OK) {
        aegis_autonomous_runtime_destroy(aa->runtime);
        pthread_mutex_destroy(&aa->lock);
        free(aa->llm_name_copy);
        free(aa);
        return tr;
    }

    if (!cfg->cancel_token) {
        aegis_status_t rc = aegis_cancellation_token_create(&aa->owned_token);
        if (rc != AEGIS_OK) {
            aegis_autonomous_runtime_destroy(aa->runtime);
            pthread_mutex_destroy(&aa->lock);
            free(aa->llm_name_copy);
            free(aa);
            return rc;
        }
    }
    aa->runtime->token = aegis_autonomous_get_token(aa);

    /* Security must be an explicit gate: if tools are present but no policy
     * is supplied, create an allow-all policy instead of bypassing. */
    if (cfg->tool_registry && !cfg->security_policy) {
        aegis_security_policy_t* allow = NULL;
        aegis_status_t           prc   = aegis_security_policy_create(&allow);
        if (prc != AEGIS_OK) {
            if (aa->owned_token) {
                aegis_cancellation_token_destroy(aa->owned_token);
            }
            aegis_autonomous_runtime_destroy(aa->runtime);
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

    tr = aegis_autonomous_transition(aa, AEGIS_AUTO_READY);
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
        aegis_autonomous_runtime_destroy(aa->runtime);
    }
    pthread_mutex_destroy(&aa->lock);
    free(aa->llm_name_copy);
    free(aa);
    return rc;
}

/**
 * @brief Destroy an autonomous agent and everything it owns.
 *
 * Tears down runtime, planner, scheduler, executor, critic, the owned
 * cancellation token and owned security policy, then the lock and the agent.
 * NULL is a no-op. The caller must guarantee no run is in flight.
 *
 * @param[in] aa  Agent to destroy; NULL is accepted.
 */
void aegis_autonomous_agent_destroy(aegis_autonomous_agent_t* aa)
{
    if (!aa) {
        return;
    }
    if (aa->runtime) {
        aegis_autonomous_runtime_destroy(aa->runtime);
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
/**
 * @brief Request cancellation of an in-flight autonomous run.
 *
 * Signals the effective cancellation token and best-effort transitions the
 * state machine to CANCELLING when that edge is allowed (ignored otherwise).
 * Non-blocking: the running loop observes the token and winds down.
 *
 * @param[in] aa  Agent whose run should stop; must be non-NULL with a token.
 *
 * @return AEGIS_OK on success; AEGIS_ERR_INVALID when aa/token is missing.
 *
 * Thread-safe: token request + transition are internally synchronized.
 */
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
    (void)aegis_autonomous_transition(aa, AEGIS_AUTO_CANCELLING);
    return AEGIS_OK;
}

/**
 * @brief Persist a checkpoint snapshot for this agent to a file.
 *
 * Resolves the destination as the explicit path, else the config's
 * checkpoint_path (INVALID when neither exists), then writes a checkpoint
 * populated from the current runtime state, honouring the agent token.
 *
 * @param[in] aa    Agent to snapshot; must be non-NULL.
 * @param[in] path  Destination file, or NULL to use cfg.checkpoint_path.
 *
 * @return AEGIS_OK on success; AEGIS_ERR_INVALID or the write error otherwise.
 */
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

/**
 * @brief Restore agent state from a checkpoint file.
 *
 * Thin alias over aegis_autonomous_checkpoint_restore(); reloads the saved
 * plan/graph/iteration state so a later run resumes where it left off.
 *
 * @param[in] aa    Agent to restore into; must be non-NULL.
 * @param[in] path  Checkpoint file previously written by checkpoint_save.
 *
 * @return AEGIS_OK on success; AEGIS_ERR_INVALID/NOT_FOUND/IO on failure.
 */
aegis_status_t aegis_autonomous_agent_restore(aegis_autonomous_agent_t* aa, const char* path)
{
    return aegis_autonomous_checkpoint_restore(aa, path);
}

/**
 * @brief Run the autonomous plan→execute→evaluate→reflect→replan loop.
 *
 * Syncs the runtime goal/token, delegates to aegis_autonomous_loop_run()
 * (which owns iteration counting and plan/graph retention for post-run
 * inspection), and recreates the scheduler if the loop left it detached.
 * Blocks until the goal completes, fails, or cancellation lands.
 *
 * @param[in]  aa          Agent in READY (or post-run reusable) state.
 * @param[in]  goal_text   Non-empty goal description; copied into runtime.
 * @param[out] out_result  Optional summary; NULL skips the result fill.
 *
 * @return AEGIS_OK when the loop reports success; INVALID when inputs or the
 *         runtime are missing, else the loop's terminal status.
 *
 * Thread-safe: serialized against cancel via the agent lock/token.
 */
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
    aa->runtime->token = aegis_autonomous_get_token(aa);
    // Delegate to loop orchestrator; loop owns iteration/plan/graph ownership
    aegis_status_t rc = aegis_autonomous_loop_run(aa, aa->runtime, goal_text, out_result);
    // On return, runtime retains plan/graph for inspection until next run or destroy;
    // reset iteration for fresh goal if not recovered — handled inside loop.
    // Ensure scheduler is alive (loop may have left it attached)
    if (!aa->scheduler) {
        (void)aegis_scheduler_create(&aa->scheduler);
    }
    return rc;
}
