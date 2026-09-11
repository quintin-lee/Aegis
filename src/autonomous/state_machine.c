/**
 * @file state_machine.c
 * @brief Agent state machine: spec-defined transition table with
 * validation, mutex-guarded transitions and post-unlock event publish
 * (no callbacks run under the agent lock).
 */
#define _POSIX_C_SOURCE 200809L
#include "autonomous_agent_internal.h"

#include "aegis/autonomous_state.h"

#include <string.h>

/* Transition table per spec. */
typedef struct {
    aegis_autonomous_state_t from;
    aegis_autonomous_state_t to;
} transition_t;

static const transition_t k_allowed[] = {
    {AEGIS_AUTO_CREATED, AEGIS_AUTO_INITIALIZING}, {AEGIS_AUTO_INITIALIZING, AEGIS_AUTO_READY},
    {AEGIS_AUTO_INITIALIZING, AEGIS_AUTO_FAILED},  {AEGIS_AUTO_READY, AEGIS_AUTO_PLANNING},
    {AEGIS_AUTO_READY, AEGIS_AUTO_RECOVERING},     {AEGIS_AUTO_READY, AEGIS_AUTO_CANCELLING},
    {AEGIS_AUTO_PLANNING, AEGIS_AUTO_SCHEDULING},  {AEGIS_AUTO_PLANNING, AEGIS_AUTO_FAILED},
    {AEGIS_AUTO_PLANNING, AEGIS_AUTO_CANCELLING},  {AEGIS_AUTO_SCHEDULING, AEGIS_AUTO_EXECUTING},
    {AEGIS_AUTO_SCHEDULING, AEGIS_AUTO_FAILED},    {AEGIS_AUTO_SCHEDULING, AEGIS_AUTO_CANCELLING},
    {AEGIS_AUTO_EXECUTING, AEGIS_AUTO_EVALUATING}, {AEGIS_AUTO_EXECUTING, AEGIS_AUTO_CHECKPOINTING},
    {AEGIS_AUTO_EXECUTING, AEGIS_AUTO_FAILED},     {AEGIS_AUTO_EXECUTING, AEGIS_AUTO_CANCELLING},
    {AEGIS_AUTO_EVALUATING, AEGIS_AUTO_COMPLETED}, {AEGIS_AUTO_EVALUATING, AEGIS_AUTO_REFLECTING},
    {AEGIS_AUTO_EVALUATING, AEGIS_AUTO_FAILED},    {AEGIS_AUTO_REFLECTING, AEGIS_AUTO_REPLANNING},
    {AEGIS_AUTO_REFLECTING, AEGIS_AUTO_FAILED},    {AEGIS_AUTO_REPLANNING, AEGIS_AUTO_PLANNING},
    {AEGIS_AUTO_REPLANNING, AEGIS_AUTO_FAILED},    {AEGIS_AUTO_CHECKPOINTING, AEGIS_AUTO_EXECUTING},
    {AEGIS_AUTO_CHECKPOINTING, AEGIS_AUTO_FAILED}, {AEGIS_AUTO_RECOVERING, AEGIS_AUTO_READY},
    {AEGIS_AUTO_RECOVERING, AEGIS_AUTO_FAILED},    {AEGIS_AUTO_CANCELLING, AEGIS_AUTO_CANCELLED},
};

/**
 * @brief Test whether a state-machine edge is permitted by the spec table.
 *
 * Linearly scans k_allowed for the (from, to) pair; anything absent — e.g.
 * skipping phases or leaving a terminal state — is rejected.
 *
 * @param[in] from  Current lifecycle state.
 * @param[in] to    Requested target state.
 *
 * @return true when the transition is listed, false otherwise.
 */
bool aegis_autonomous_transition_allowed(aegis_autonomous_state_t from, aegis_autonomous_state_t to)
{
    for (size_t i = 0; i < sizeof(k_allowed) / sizeof(k_allowed[0]); i++) {
        if (k_allowed[i].from == from && k_allowed[i].to == to) {
            return true;
        }
    }
    return false;
}

/**
 * @brief Advance the agent state machine to a new state.
 *
 * Follows lock → validate → update → unlock → publish: the edge is checked
 * against the spec table under the agent mutex, the state is updated, and
 * any event notification happens only after the lock is released so no
 * callback ever runs with aa->lock held.
 *
 * @param[in] aa      Agent whose state advances; must be non-NULL.
 * @param[in] target  Desired state; must be reachable from the current one.
 *
 * @return AEGIS_OK on success; AEGIS_ERR_INVALID for NULL,
 *         AEGIS_ERR_INVALID_STATE for a forbidden edge, AEGIS_ERR_INTERNAL
 *         when the mutex cannot be locked. Failed calls change nothing.
 *
 * Thread-safe: fully serialized by the agent mutex.
 */
aegis_status_t aegis_autonomous_transition(aegis_autonomous_agent_t* aa, aegis_autonomous_state_t target)
{
    if (!aa) {
        return AEGIS_ERR_INVALID;
    }
    /* Phase: lock -> validate -> update -> prepare event -> unlock -> publish */
    int lock_rc = pthread_mutex_lock(&aa->lock);
    if (lock_rc != 0) {
        return AEGIS_ERR_INTERNAL;
    }
    aegis_autonomous_state_t current = aa->state;
    if (!aegis_autonomous_transition_allowed(current, target)) {
        pthread_mutex_unlock(&aa->lock);
        return AEGIS_ERR_INVALID_STATE;
    }
    aa->state = target;

    /* Prepare event data while still holding lock (copy states). */
    aegis_autonomous_state_t prev = current;
    aegis_autonomous_state_t next = target;
    /* Unlock before publishing. */
    pthread_mutex_unlock(&aa->lock);

    /* Publish event after unlock — no lock held during callback.
     * Currently event bus is not wired for autonomous_state; we keep the
     * publish as a no-op but preserve the ordering. If an event bus were
     * attached, it would be invoked here without holding aa->lock. */
    (void)prev;
    (void)next;
    return AEGIS_OK;
}
