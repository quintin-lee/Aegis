/**
 * @file agent.c
 * @brief Agent lifecycle and state machine implementation.
 *
 * State transition table:
 *   CREATED        -> INITIALIZING  (via aegis_agent_init)
 *   INITIALIZING   -> READY         (completed internally by init)
 *   READY          -> RUNNING       (via aegis_agent_start)
 *   RUNNING        -> PAUSED        (via aegis_agent_pause)
 *   PAUSED         -> RUNNING       (via aegis_agent_resume)
 *   RUNNING/PAUSED -> CANCELLING    (via aegis_agent_cancel)
 *   CANCELLING     -> COMPLETED     (via internal transition on success)
 *   CANCELLING     -> FAILED        (via internal transition on error)
 *   CANCELLING     -> CANCELLED     (via internal transition on cancel ack)
 *   ANY terminal   -> (none)        — no transitions out of terminal states
 */
#define _POSIX_C_SOURCE 200809L
#include "aegis/agent.h"
#include "internal/agent_internal.h"
#include "internal/lifecycle.h"
#include "aegis/common/time.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ── Valid transitions ─────────────────────────────────────────────────────── */

static bool is_terminal(aegis_agent_state_t s)
{
    return s == AEGIS_AGENT_COMPLETED || s == AEGIS_AGENT_FAILED || s == AEGIS_AGENT_CANCELLED ||
           s == AEGIS_AGENT_ABORTED;
}

/* ── Helper: emit a state transition event ─────────────────────────────────── */

static void emit_state_change(aegis_agent_t* agent, aegis_agent_state_t from,
                              aegis_agent_state_t to)
{
    if (!agent || !agent->bus) {
        return;
    }
    (void)from;

    aegis_event_payload_t payload = {.data = &to, .size = sizeof(to)};

    aegis_event_t* ev = NULL;
    if (aegis_event_create(&ev, 0x1000 /* STATE_CHANGE */, &payload) == AEGIS_OK) {
        aegis_event_bus_publish(agent->bus, ev);
        aegis_event_destroy(ev);
    }
}

/* ── Lifecycle ─────────────────────────────────────────────────────────────── */

aegis_status_t aegis_agent_create(aegis_agent_t** out, const char* name)
{
    AEGIS_CHECK_OUT(out);

    if (!name || name[0] == '\0') {
        return AEGIS_ERR_INVALID;
    }

    aegis_agent_t* agent = (aegis_agent_t*)calloc(1, sizeof(*agent));
    if (!agent) {
        return AEGIS_ERR_NOMEM;
    }

    agent->name   = strdup(name);
    agent->state  = AEGIS_AGENT_CREATED;
    agent->bus    = NULL;
    agent->joined = NULL;

    if (!agent->name) {
        free(agent);
        return AEGIS_ERR_NOMEM;
    }

    int rc = aegis_mutex_create(&agent->lock, AEGIS_MUTEX_RECURSIVE);
    if (rc != 0) {
        free(agent->name);
        free(agent);
        return AEGIS_ERR_NOMEM;
    }

    rc = aegis_atomic_int_create(&agent->done_flag, 0);
    if (rc != 0) {
        aegis_mutex_destroy(agent->lock);
        free(agent->name);
        free(agent);
        return AEGIS_ERR_NOMEM;
    }

    rc = aegis_event_bus_create(&agent->bus);
    if (rc != 0) {
        aegis_atomic_int_destroy(agent->done_flag);
        aegis_mutex_destroy(agent->lock);
        free(agent->name);
        free(agent);
        return AEGIS_ERR_NOMEM;
    }

    *out = agent;
    return AEGIS_OK;
}

void aegis_agent_destroy(aegis_agent_t* agent)
{
    if (!agent) {
        return;
    }

    aegis_agent_state_t s = aegis_agent_state(agent);
    if (s == AEGIS_AGENT_RUNNING || s == AEGIS_AGENT_PAUSED || s == AEGIS_AGENT_INITIALIZING) {
        aegis_mutex_lock(agent->lock);
        if (!is_terminal(s)) {
            agent->state = AEGIS_AGENT_ABORTED;
            aegis_atomic_int_store(agent->done_flag, 1);
        }
        aegis_mutex_unlock(agent->lock);
    }

    aegis_event_bus_destroy(agent->bus);
    aegis_atomic_int_destroy(agent->done_flag);
    aegis_mutex_destroy(agent->lock);
    free(agent->name);
    free(agent);
}

/* ── State access ──────────────────────────────────────────────────────────── */

aegis_agent_state_t aegis_agent_state(const aegis_agent_t* agent)
{
    if (!agent) {
        return AEGIS_AGENT_CREATED;
    }
    aegis_mutex_lock(agent->lock);
    aegis_agent_state_t s = agent->state;
    aegis_mutex_unlock(agent->lock);
    return s;
}

/* ── State transitions ─────────────────────────────────────────────────────── */

aegis_status_t aegis_agent_init(aegis_agent_t* agent)
{
    if (!agent) {
        return AEGIS_ERR_INVALID;
    }

    aegis_mutex_lock(agent->lock);

    if (agent->state == AEGIS_AGENT_READY) {
        aegis_mutex_unlock(agent->lock);
        return AEGIS_OK; /* idempotent */
    }

    if (agent->state != AEGIS_AGENT_CREATED) {
        aegis_mutex_unlock(agent->lock);
        return AEGIS_ERR_INVALID;
    }

    agent->state = AEGIS_AGENT_INITIALIZING;
    emit_state_change(agent, AEGIS_AGENT_CREATED, AEGIS_AGENT_INITIALIZING);
    aegis_mutex_unlock(agent->lock);

    /* Initialization complete */
    aegis_mutex_lock(agent->lock);
    agent->state = AEGIS_AGENT_READY;
    emit_state_change(agent, AEGIS_AGENT_INITIALIZING, AEGIS_AGENT_READY);
    aegis_mutex_unlock(agent->lock);

    return AEGIS_OK;
}

aegis_status_t aegis_agent_start(aegis_agent_t* agent)
{
    if (!agent) {
        return AEGIS_ERR_INVALID;
    }

    aegis_mutex_lock(agent->lock);

    if (agent->state == AEGIS_AGENT_RUNNING) {
        aegis_mutex_unlock(agent->lock);
        return AEGIS_OK; /* idempotent */
    }

    if (agent->state != AEGIS_AGENT_READY) {
        aegis_mutex_unlock(agent->lock);
        return AEGIS_ERR_INVALID;
    }

    agent->state = AEGIS_AGENT_RUNNING;
    emit_state_change(agent, AEGIS_AGENT_READY, AEGIS_AGENT_RUNNING);
    aegis_mutex_unlock(agent->lock);

    return AEGIS_OK;
}

aegis_status_t aegis_agent_pause(aegis_agent_t* agent)
{
    if (!agent) {
        return AEGIS_ERR_INVALID;
    }

    aegis_mutex_lock(agent->lock);

    if (agent->state != AEGIS_AGENT_RUNNING) {
        aegis_mutex_unlock(agent->lock);
        return AEGIS_ERR_INVALID;
    }

    agent->state = AEGIS_AGENT_PAUSED;
    emit_state_change(agent, AEGIS_AGENT_RUNNING, AEGIS_AGENT_PAUSED);
    aegis_mutex_unlock(agent->lock);

    return AEGIS_OK;
}

aegis_status_t aegis_agent_resume(aegis_agent_t* agent)
{
    if (!agent) {
        return AEGIS_ERR_INVALID;
    }

    aegis_mutex_lock(agent->lock);

    if (agent->state != AEGIS_AGENT_PAUSED) {
        aegis_mutex_unlock(agent->lock);
        return AEGIS_ERR_INVALID;
    }

    agent->state = AEGIS_AGENT_RUNNING;
    emit_state_change(agent, AEGIS_AGENT_PAUSED, AEGIS_AGENT_RUNNING);
    aegis_mutex_unlock(agent->lock);

    return AEGIS_OK;
}

aegis_status_t aegis_agent_cancel(aegis_agent_t* agent)
{
    if (!agent) {
        return AEGIS_ERR_INVALID;
    }

    aegis_mutex_lock(agent->lock);

    if (agent->state != AEGIS_AGENT_RUNNING && agent->state != AEGIS_AGENT_PAUSED) {
        aegis_mutex_unlock(agent->lock);
        return AEGIS_ERR_INVALID;
    }

    agent->state = AEGIS_AGENT_CANCELLING;
    aegis_mutex_unlock(agent->lock);

    /* Emit cancellation event */
    aegis_event_payload_t payload = {.data = &agent->state, .size = sizeof(agent->state)};
    aegis_event_t*        ev      = NULL;
    if (aegis_event_create(&ev, 0x2000 /* CANCEL_REQUESTED */, &payload) == AEGIS_OK) {
        aegis_event_bus_publish(agent->bus, ev);
        aegis_event_destroy(ev);
    }

    /* Complete cancellation synchronously (no background work in test).
     * In production, the event loop would drive CANCELLING → CANCELLED
     * after draining in-flight tasks. */
    aegis_mutex_lock(agent->lock);
    agent->state = AEGIS_AGENT_CANCELLED;
    emit_state_change(agent, AEGIS_AGENT_CANCELLING, AEGIS_AGENT_CANCELLED);
    aegis_atomic_int_store(agent->done_flag, 1);
    aegis_mutex_unlock(agent->lock);

    return AEGIS_OK;
}

aegis_status_t aegis_agent_join(aegis_agent_t* agent, long timeout_ms)
{
    if (!agent) {
        return AEGIS_ERR_INVALID;
    }

    if (is_terminal(aegis_agent_state(agent))) {
        return AEGIS_OK;
    }

    if (timeout_ms <= 0) {
        /* Wait indefinitely */
        while (!is_terminal(aegis_agent_state(agent))) {
            aegis_sleep_ms(10);
        }
    } else {
        long elapsed = 0;
        while (elapsed < timeout_ms) {
            if (is_terminal(aegis_agent_state(agent))) {
                return AEGIS_OK;
            }
            aegis_sleep_ms(10);
            elapsed += 10;
        }
        return AEGIS_ERR_TIMEOUT;
    }

    return AEGIS_OK;
}

/* ── Goal ──────────────────────────────────────────────────────────────────── */

void aegis_agent_set_goal(aegis_agent_t* agent, const char* goal)
{
    if (!agent) {
        return;
    }
    aegis_mutex_lock(agent->lock);
    if (goal) {
        strncpy(agent->goal, goal, sizeof(agent->goal) - 1);
        agent->goal[sizeof(agent->goal) - 1] = '\0';
    } else {
        agent->goal[0] = '\0';
    }
    aegis_mutex_unlock(agent->lock);
}

const char* aegis_agent_get_goal(const aegis_agent_t* agent)
{
    if (!agent) {
        return NULL;
    }
    aegis_mutex_lock(agent->lock);
    const char* g = agent->goal[0] ? agent->goal : NULL;
    aegis_mutex_unlock(agent->lock);
    return g;
}

/* ── Properties ────────────────────────────────────────────────────────────── */

const char* aegis_agent_name(const aegis_agent_t* agent)
{
    if (!agent) {
        return NULL;
    }
    return agent->name;
}

aegis_event_bus_t* aegis_agent_event_bus(const aegis_agent_t* agent)
{
    if (!agent) {
        return NULL;
    }
    return agent->bus;
}
