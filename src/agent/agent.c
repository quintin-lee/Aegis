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
#include "aegis/agent/agent.h"
#include "agent_internal.h"
#include "lifecycle.h"
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

/**
 * @brief Allocate an agent in the CREATED state with its own event bus.
 *
 * Copies @p name and initializes the lock, done-flag and event bus.
 *
 * @param[out] out   Receives the new agent on success.
 * @param[in]  name  Agent name; must be non-NULL and non-empty.
 * @return AEGIS_OK on success, AEGIS_ERR_INVALID for a NULL @p out or an
 *         empty @p name, AEGIS_ERR_NOMEM on allocation failure.
 *
 * @note Ownership: caller owns the agent; release with
 *       aegis_agent_destroy(). Thread-safe.
 */
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

/**
 * @brief Destroy an agent, aborting it first when still active.
 *
 * An agent in RUNNING, PAUSED or INITIALIZING is force-moved to ABORTED
 * and its done-flag is set before teardown. NULL is a no-op.
 *
 * @param[in] agent  Agent to destroy.
 */
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

/**
 * @brief Return the agent's current state (lock-protected snapshot).
 *
 * @param[in] agent  Agent to query; NULL yields AEGIS_AGENT_CREATED.
 * @return Current state. Thread-safe.
 */
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

/**
 * @brief Move a CREATED agent to READY (CREATED → INITIALIZING → READY).
 *
 * Emits a state-change event for each hop. Idempotent when already READY.
 *
 * @param[in] agent  Agent to initialize.
 * @return AEGIS_OK on success (or already READY), AEGIS_ERR_INVALID for
 *         a NULL agent or a state other than CREATED/READY. Thread-safe.
 */
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

/**
 * @brief Move a READY agent to RUNNING. Idempotent when already RUNNING.
 *
 * @param[in] agent  Agent to start.
 * @return AEGIS_OK on success (or already RUNNING), AEGIS_ERR_INVALID
 *         for a NULL agent or a state other than READY/RUNNING.
 *         Thread-safe.
 */
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

/**
 * @brief Move a RUNNING agent to PAUSED.
 *
 * @param[in] agent  Agent to pause.
 * @return AEGIS_OK on success, AEGIS_ERR_INVALID for a NULL agent or
 *         any state other than RUNNING. Thread-safe.
 */
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

/**
 * @brief Move a PAUSED agent back to RUNNING.
 *
 * @param[in] agent  Agent to resume.
 * @return AEGIS_OK on success, AEGIS_ERR_INVALID for a NULL agent or
 *         any state other than PAUSED. Thread-safe.
 */
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

/**
 * @brief Request cancellation of a RUNNING or PAUSED agent.
 *
 * Publishes a CANCEL_REQUESTED event on the agent bus, then completes
 * the RUNNING/PAUSED → CANCELLING → CANCELLED transition synchronously
 * and signals the done-flag. (Production use would let the event loop
 * drain in-flight tasks before the final hop.)
 *
 * @param[in] agent  Agent to cancel.
 * @return AEGIS_OK on success, AEGIS_ERR_INVALID for a NULL agent or
 *         any state other than RUNNING/PAUSED. Thread-safe.
 */
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

/**
 * @brief Block until the agent reaches a terminal state.
 *
 * Returns immediately when already terminal. Polls the state every
 * 10 ms, so the effective wait is quantized to that granularity.
 *
 * @param[in] agent       Agent to wait for.
 * @param[in] timeout_ms  Maximum wait in milliseconds; ≤ 0 waits
 *                        indefinitely.
 * @return AEGIS_OK once terminal, AEGIS_ERR_INVALID for a NULL agent,
 *         AEGIS_ERR_TIMEOUT when the deadline expires first.
 *         Thread-safe.
 */
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

/**
 * @brief Set the agent's goal string (copied, truncated to the buffer).
 *
 * Passing NULL clears the goal. NULL agent is a no-op.
 *
 * @param[in] agent  Agent to update.
 * @param[in] goal   New goal text; NULL clears it. Thread-safe.
 */
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

/**
 * @brief Return the agent's goal string.
 *
 * @param[in] agent  Agent to query; NULL yields NULL.
 * @return Borrowed pointer to the internal goal buffer, or NULL when
 *         unset/empty. Thread-safe for the read, but the pointer is only
 *         valid until the next set_goal call; copy it if retained.
 */
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

/**
 * @brief Return the agent's name given at creation.
 *
 * @param[in] agent  Agent to query; NULL yields NULL.
 * @return Borrowed name string owned by the agent; do not free.
 */
const char* aegis_agent_name(const aegis_agent_t* agent)
{
    if (!agent) {
        return NULL;
    }
    return agent->name;
}

/**
 * @brief Return the agent's private event bus.
 *
 * @param[in] agent  Agent to query; NULL yields NULL.
 * @return Borrowed bus pointer owned by the agent; do not destroy.
 */
aegis_event_bus_t* aegis_agent_event_bus(const aegis_agent_t* agent)
{
    if (!agent) {
        return NULL;
    }
    return agent->bus;
}
