#define _POSIX_C_SOURCE 200809L
#include "autonomous_agent_internal.h"
#include <string.h>

aegis_status_t autonomous_loop_run(aegis_autonomous_agent_t*   agent,
                                   aegis_autonomous_runtime_t* runtime, const char* goal,
                                   aegis_autonomous_result_t* out_result)
{
    if (!agent || !runtime || !goal) {
        return AEGIS_ERR_INVALID;
    }

    aegis_cancellation_token_t* token      = autonomous_get_token(agent);
    aegis_status_t              final      = AEGIS_OK;
    uint64_t                    loop_count = 0;

    // Ensure start from READY
    pthread_mutex_lock(&agent->lock);
    aegis_autonomous_state_t cur        = agent->state;
    uint64_t                 start_iter = runtime->iteration;
    if (agent->iteration != runtime->iteration) {
        runtime->iteration = agent->iteration;  // sync legacy
    }
    pthread_mutex_unlock(&agent->lock);
    if (cur != AEGIS_AUTO_READY) {
        if (cur == AEGIS_AUTO_RECOVERING) {
            (void)autonomous_transition(agent, AEGIS_AUTO_READY);
        } else if (cur == AEGIS_AUTO_CREATED || cur == AEGIS_AUTO_INITIALIZING) {
            (void)autonomous_transition(agent, AEGIS_AUTO_READY);
        }
    }

    // Initial planning if needed
    if (!runtime->plan) {
        (void)autonomous_transition(agent, AEGIS_AUTO_PLANNING);
        aegis_status_t rc = autonomous_plan(agent, runtime, goal);
        if (rc != AEGIS_OK) {
            (void)autonomous_transition(agent, AEGIS_AUTO_FAILED);
            final = rc;
            goto done;
        }
        (void)autonomous_transition(agent, AEGIS_AUTO_SCHEDULING);
    }

    for (uint64_t iter = start_iter; iter < agent->cfg.max_iterations; iter++) {
        if (token && aegis_cancellation_token_is_cancelled(token)) {
            final = AEGIS_ERR_CANCELLED;
            (void)autonomous_transition(agent, AEGIS_AUTO_CANCELLING);
            break;
        }

        pthread_mutex_lock(&agent->lock);
        runtime->iteration = iter + 1;
        agent->iteration   = (uint32_t)runtime->iteration;
        pthread_mutex_unlock(&agent->lock);

        if (iter != start_iter) {
            // For subsequent iterations, plan already replanned at end of prior loop
        }

        aegis_status_t rc = autonomous_execute(agent, runtime);
        if (rc == AEGIS_ERR_CANCELLED) {
            final = rc;
            (void)autonomous_transition(agent, AEGIS_AUTO_CANCELLING);
            autonomous_checkpoint_save(agent, goal, runtime->plan, runtime->graph);
            break;
        }
        if (rc == AEGIS_ERR_TIMEOUT) {
            final = rc;
            (void)autonomous_transition(agent, AEGIS_AUTO_FAILED);
            autonomous_checkpoint_save(agent, goal, runtime->plan, runtime->graph);
            break;
        }
        if (rc != AEGIS_OK) {
            final = rc;
            (void)autonomous_transition(agent, AEGIS_AUTO_FAILED);
            autonomous_checkpoint_save(agent, goal, runtime->plan, runtime->graph);
            break;
        }

        autonomous_checkpoint_save(agent, goal, runtime->plan, runtime->graph);

        (void)autonomous_transition(agent, AEGIS_AUTO_EVALUATING);
        rc = autonomous_evaluate(agent, runtime);
        if (rc == AEGIS_ERR_CANCELLED) {
            final = AEGIS_ERR_CANCELLED;
            (void)autonomous_transition(agent, AEGIS_AUTO_CANCELLING);
            break;
        }
        if (rc != AEGIS_OK) {
            (void)autonomous_transition(agent, AEGIS_AUTO_FAILED);
            final = rc;
            break;
        }

        if (runtime->last_critique.result == AEGIS_CRITIQUE_SUCCESS) {
            (void)autonomous_transition(agent, AEGIS_AUTO_COMPLETED);
            final = AEGIS_OK;
            break;
        }
        if (runtime->last_critique.result == AEGIS_CRITIQUE_REPLAN_REQUIRED ||
            runtime->last_critique.result == AEGIS_CRITIQUE_PARTIAL ||
            runtime->last_critique.result == AEGIS_CRITIQUE_FAILURE) {
            (void)autonomous_transition(agent, AEGIS_AUTO_REFLECTING);
            rc = autonomous_reflect(agent, runtime);
            if (rc != AEGIS_OK) {
                (void)autonomous_transition(agent, AEGIS_AUTO_FAILED);
                final = rc;
                break;
            }
            (void)autonomous_transition(agent, AEGIS_AUTO_REPLANNING);
            rc = autonomous_replan(agent, runtime);
            if (rc != AEGIS_OK) {
                (void)autonomous_transition(agent, AEGIS_AUTO_FAILED);
                final = rc;
                break;
            }
            (void)autonomous_transition(agent, AEGIS_AUTO_PLANNING);
            (void)autonomous_transition(agent, AEGIS_AUTO_SCHEDULING);
            final = AEGIS_OK;
            continue;
        }
        (void)autonomous_transition(agent, AEGIS_AUTO_FAILED);
        final = AEGIS_ERR_INTERNAL;
        break;
    }

    pthread_mutex_lock(&agent->lock);
    loop_count = runtime->iteration;
    pthread_mutex_unlock(&agent->lock);
    if (loop_count >= agent->cfg.max_iterations && final == AEGIS_OK) {
        final = AEGIS_ERR_MAX_ITERATIONS;
        (void)autonomous_transition(agent, AEGIS_AUTO_FAILED);
    } else if (final == AEGIS_ERR_CANCELLED) {
        (void)autonomous_transition(agent, AEGIS_AUTO_CANCELLED);
    } else if (final != AEGIS_OK) {
        pthread_mutex_lock(&agent->lock);
        aegis_autonomous_state_t st = agent->state;
        pthread_mutex_unlock(&agent->lock);
        if (st != AEGIS_AUTO_FAILED && st != AEGIS_AUTO_CANCELLED && st != AEGIS_AUTO_COMPLETED) {
            (void)autonomous_transition(agent, AEGIS_AUTO_FAILED);
        }
    }

done:
    if (out_result) {
        pthread_mutex_lock(&agent->lock);
        bool rec = agent->recovered;
        pthread_mutex_unlock(&agent->lock);
        out_result->final_status              = final;
        out_result->iterations                = loop_count;
        out_result->tasks_executed            = runtime->tasks_executed;
        out_result->recovered_from_checkpoint = rec;
    }
    pthread_mutex_lock(&agent->lock);
    agent->recovered    = false;
    runtime->recovering = false;
    pthread_mutex_unlock(&agent->lock);
    return final;
}
