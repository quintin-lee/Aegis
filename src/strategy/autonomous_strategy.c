/**
 * @file autonomous_strategy.c
 * @brief AutonomousStrategy: goal-driven planning as a loop strategy.
 *
 * Implements the loop-strategy ABI on top of the autonomous phase
 * machine (plan → execute → evaluate → reflect → replan). Owns a
 * private runtime (never touches the agent's run-loop runtime); the
 * goal is read from the loop session's first user message. after_model
 * and after_tool are intentionally no-ops: model turns are reactive,
 * execution stays inside the autonomous executor.
 */
#define _POSIX_C_SOURCE 200809L
#include "aegis/strategy/autonomous_strategy.h"
#include "aegis/agent/strategy.h"
#include "aegis/agent/loop.h"
#include "aegis/autonomous_agent.h"
#include "aegis/session/session.h"
#include "aegis/message/message.h"
#include "aegis/message/role.h"
#include "autonomous_agent_internal.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

struct aegis_autonomous_strategy {
    aegis_autonomous_agent_t*    agent;
    aegis_autonomous_runtime_t*  runtime;
    aegis_agent_strategy_def_t   def;
};

static aegis_status_t strat_init(void* user)
{
    aegis_autonomous_strategy_t* s = (aegis_autonomous_strategy_t*)user;
    if (!s) {
        return AEGIS_ERR_INVALID;
    }
    if (s->runtime) {
        return AEGIS_OK;
    }
    return aegis_autonomous_runtime_create(&s->runtime);
}

static aegis_status_t strat_shutdown(void* user)
{
    aegis_autonomous_strategy_t* s = (aegis_autonomous_strategy_t*)user;
    if (!s) {
        return AEGIS_OK;
    }
    if (s->runtime) {
        aegis_autonomous_runtime_destroy(s->runtime);
        s->runtime = NULL;
    }
    return AEGIS_OK;
}

static const char* find_goal(aegis_agent_loop_t* loop)
{
    aegis_session_t* sess = aegis_agent_loop_session(loop);
    if (!sess) {
        return NULL;
    }
    size_t n = aegis_session_message_count(sess);
    for (size_t i = 0; i < n; i++) {
        const aegis_message_t* m = aegis_session_message_at(sess, i);
        if (m && aegis_message_role(m) == AEGIS_MESSAGE_USER) {
            const char* text = aegis_message_content(m);
            if (text && text[0] != '\0') {
                return text;
            }
        }
    }
    return NULL;
}

static aegis_status_t ensure_ready(aegis_autonomous_agent_t* agent,
                                   aegis_autonomous_runtime_t* runtime)
{
    pthread_mutex_lock(&agent->lock);
    aegis_autonomous_state_t cur = agent->state;
    if (agent->iteration != runtime->iteration) {
        runtime->iteration = agent->iteration;
    }
    pthread_mutex_unlock(&agent->lock);
    if (cur == AEGIS_AUTO_RECOVERING || cur == AEGIS_AUTO_CREATED ||
        cur == AEGIS_AUTO_INITIALIZING) {
        (void)aegis_autonomous_transition(agent, AEGIS_AUTO_READY);
    }
    return AEGIS_OK;
}

static aegis_status_t strat_before_turn(void* user, aegis_agent_loop_t* loop)
{
    aegis_autonomous_strategy_t* s = (aegis_autonomous_strategy_t*)user;
    if (!s || !s->agent || !loop) {
        return AEGIS_ERR_INVALID;
    }
    if (!s->runtime) {
        aegis_status_t rc = aegis_autonomous_runtime_create(&s->runtime);
        if (rc != AEGIS_OK) {
            return rc;
        }
    }
    if (s->runtime->plan) {
        return AEGIS_OK;
    }
    aegis_cancellation_token_t* token = aegis_autonomous_get_token(s->agent);
    if (token && aegis_cancellation_token_is_cancelled(token)) {
        return AEGIS_ERR_CANCELLED;
    }
    const char* goal = find_goal(loop);
    if (!goal) {
        return AEGIS_ERR_INVALID;
    }
    snprintf(s->runtime->goal, sizeof(s->runtime->goal), "%s", goal);
    ensure_ready(s->agent, s->runtime);
    (void)aegis_autonomous_transition(s->agent, AEGIS_AUTO_PLANNING);
    aegis_status_t rc = aegis_autonomous_plan(s->agent, s->runtime, s->runtime->goal);
    if (rc != AEGIS_OK) {
        (void)aegis_autonomous_transition(s->agent, AEGIS_AUTO_FAILED);
        return rc;
    }
    (void)aegis_autonomous_transition(s->agent, AEGIS_AUTO_SCHEDULING);
    return AEGIS_OK;
}

static aegis_status_t strat_after_model(void* user, aegis_agent_loop_t* loop)
{
    (void)user;
    (void)loop;
    return AEGIS_OK;
}

static aegis_status_t strat_after_tool(void* user, aegis_agent_loop_t* loop)
{
    (void)user;
    (void)loop;
    return AEGIS_OK;
}

static aegis_status_t strat_should_continue(void* user, int* out_continue)
{
    aegis_autonomous_strategy_t* s = (aegis_autonomous_strategy_t*)user;
    if (!s || !s->agent || !s->runtime || !out_continue) {
        return AEGIS_ERR_INVALID;
    }
    *out_continue = 0;
    aegis_cancellation_token_t* token = aegis_autonomous_get_token(s->agent);
    if (token && aegis_cancellation_token_is_cancelled(token)) {
        (void)aegis_autonomous_transition(s->agent, AEGIS_AUTO_CANCELLING);
        return AEGIS_ERR_CANCELLED;
    }
    uint32_t max_iter = s->agent->cfg.max_iterations ? s->agent->cfg.max_iterations : 5;
    if (s->runtime->iteration >= max_iter) {
        (void)aegis_autonomous_transition(s->agent, AEGIS_AUTO_FAILED);
        return AEGIS_ERR_MAX_ITERATIONS;
    }
    pthread_mutex_lock(&s->agent->lock);
    s->runtime->iteration++;
    s->agent->iteration = (uint32_t)s->runtime->iteration;
    pthread_mutex_unlock(&s->agent->lock);

    const char* goal = s->runtime->goal;
    aegis_status_t rc = aegis_autonomous_execute(s->agent, s->runtime);
    if (rc != AEGIS_OK) {
        (void)aegis_autonomous_transition(
            s->agent, rc == AEGIS_ERR_CANCELLED ? AEGIS_AUTO_CANCELLING : AEGIS_AUTO_FAILED);
        aegis_autonomous_checkpoint_save(s->agent, goal, s->runtime->plan, s->runtime->graph);
        return rc;
    }
    aegis_autonomous_checkpoint_save(s->agent, goal, s->runtime->plan, s->runtime->graph);

    (void)aegis_autonomous_transition(s->agent, AEGIS_AUTO_EVALUATING);
    rc = aegis_autonomous_evaluate(s->agent, s->runtime);
    if (rc == AEGIS_ERR_CANCELLED) {
        (void)aegis_autonomous_transition(s->agent, AEGIS_AUTO_CANCELLING);
        return rc;
    }
    if (rc != AEGIS_OK) {
        (void)aegis_autonomous_transition(s->agent, AEGIS_AUTO_FAILED);
        return rc;
    }
    if (s->runtime->last_critique.result == AEGIS_CRITIQUE_SUCCESS) {
        (void)aegis_autonomous_transition(s->agent, AEGIS_AUTO_COMPLETED);
        return AEGIS_OK;
    }
    if (s->runtime->last_critique.result == AEGIS_CRITIQUE_REPLAN_REQUIRED ||
        s->runtime->last_critique.result == AEGIS_CRITIQUE_PARTIAL ||
        s->runtime->last_critique.result == AEGIS_CRITIQUE_FAILURE) {
        (void)aegis_autonomous_transition(s->agent, AEGIS_AUTO_REFLECTING);
        rc = aegis_autonomous_reflect(s->agent, s->runtime);
        if (rc != AEGIS_OK) {
            (void)aegis_autonomous_transition(s->agent, AEGIS_AUTO_FAILED);
            return rc;
        }
        (void)aegis_autonomous_transition(s->agent, AEGIS_AUTO_REPLANNING);
        rc = aegis_autonomous_replan(s->agent, s->runtime);
        if (rc != AEGIS_OK) {
            (void)aegis_autonomous_transition(s->agent, AEGIS_AUTO_FAILED);
            return rc;
        }
        (void)aegis_autonomous_transition(s->agent, AEGIS_AUTO_PLANNING);
        (void)aegis_autonomous_transition(s->agent, AEGIS_AUTO_SCHEDULING);
        *out_continue = 1;
        return AEGIS_OK;
    }
    (void)aegis_autonomous_transition(s->agent, AEGIS_AUTO_FAILED);
    return AEGIS_ERR_INTERNAL;
}

aegis_status_t aegis_autonomous_strategy_create(const aegis_autonomous_agent_config_t* cfg,
                                                aegis_autonomous_strategy_t**          out)
{
    if (!cfg || !out) {
        return AEGIS_ERR_INVALID;
    }
    aegis_autonomous_strategy_t* s = (aegis_autonomous_strategy_t*)calloc(1, sizeof(*s));
    if (!s) {
        return AEGIS_ERR_NOMEM;
    }
    aegis_status_t st = aegis_autonomous_agent_create(&s->agent, cfg);
    if (st != AEGIS_OK) {
        free(s);
        return st;
    }
    st = aegis_autonomous_runtime_create(&s->runtime);
    if (st != AEGIS_OK) {
        aegis_autonomous_agent_destroy(s->agent);
        free(s);
        return st;
    }
    s->def.name            = "autonomous";
    s->def.description     = "Goal→Plan→Graph→Scheduler→Executor→Evaluate→Reflect→Replan";
    s->def.abi_version     = AEGIS_AGENT_STRATEGY_ABI_VERSION;
    s->def.init            = strat_init;
    s->def.shutdown        = strat_shutdown;
    s->def.before_turn     = strat_before_turn;
    s->def.after_model     = strat_after_model;
    s->def.after_tool      = strat_after_tool;
    s->def.should_continue = strat_should_continue;
    s->def.user            = s;
    *out                   = s;
    return AEGIS_OK;
}

void aegis_autonomous_strategy_destroy(aegis_autonomous_strategy_t* s)
{
    if (!s) {
        return;
    }
    if (s->runtime) {
        aegis_autonomous_runtime_destroy(s->runtime);
    }
    if (s->agent) {
        aegis_autonomous_agent_destroy(s->agent);
    }
    free(s);
}

const aegis_agent_strategy_def_t* aegis_autonomous_strategy_def(aegis_autonomous_strategy_t* s)
{
    return s ? &s->def : NULL;
}
