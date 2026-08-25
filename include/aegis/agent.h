/**
 * @file agent.h
 * @brief Agent lifecycle, state machine, and goal management.
 *
 * The agent is the top-level runtime entity. It owns a state machine
 * that governs its lifecycle, a goal that defines its objective, and
 * an event bus for inter-component communication.
 *
 * State machine:
 *   CREATED → INITIALIZING → READY → RUNNING → PAUSED
 *                                    ↓         ↓
 *                              CANCELLING ←───────┘
 *                                    ↓
 *                              COMPLETED / FAILED / CANCELLED / ABORTED
 *
 * All state transitions are guarded by a mutex. Transitions not
 * listed above are rejected with AEGIS_ERR_INVALID.
 *
 * No circular dependency with event bus: agent.h includes event.h,
 * but event.h does NOT include agent.h.
 */
#ifndef AEGIS_AGENT_H
#define AEGIS_AGENT_H

#include "aegis/types.h"
#include "aegis/status.h"
#include "aegis/event.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Opaque agent handle. */
typedef struct aegis_agent aegis_agent_t;

/**
 * @brief Agent state identifiers.
 *
 * These form the nodes of the agent state machine.
 */
typedef enum aegis_agent_state {
    AEGIS_AGENT_CREATED,      /**< Handle allocated, not yet initialized. */
    AEGIS_AGENT_INITIALIZING, /**< Initialization in progress.            */
    AEGIS_AGENT_READY,        /**< Initialized and waiting for start.   */
    AEGIS_AGENT_RUNNING,      /**< Actively executing.                   */
    AEGIS_AGENT_PAUSED,       /**< Execution suspended.                  */
    AEGIS_AGENT_CANCELLING,   /**< Cancellation requested, shutting down.*/
    AEGIS_AGENT_COMPLETED,    /**< Goal achieved successfully.           */
    AEGIS_AGENT_FAILED,       /**< Execution failed.                     */
    AEGIS_AGENT_CANCELLED,    /**< Cancellation completed.               */
    AEGIS_AGENT_ABORTED,      /**< Forced abort (error recovery).        */
} aegis_agent_state_t;

/* ── Lifecycle ─────────────────────────────────────────────────────────────── */

/**
 * @brief Create a new agent with the given name.
 *
 * The agent is created in the CREATED state. A default event bus
 * is attached but not yet populated with subscribers.
 *
 * @param[out] out   Pointer receiving the created agent handle.
 *                    Ownership: transferred.
 * @param[in]  name  Agent name (must be non-NULL, non-empty). Borrowed.
 * @return AEGIS_OK on success, or a negative error code.
 */
aegis_status_t aegis_agent_create(aegis_agent_t** out, const char* name);

/**
 * @brief Destroy an agent and release all owned resources.
 *
 * Stops the agent if it is in a terminal or active state.
 * Safe to call with NULL (no-op).
 *
 * @param agent Handle to destroy (ownership: consumed).
 */
void aegis_agent_destroy(aegis_agent_t* agent);

/* ── State Machine ─────────────────────────────────────────────────────────── */

/**
 * @brief Get the current agent state.
 *
 * Thread-safe: returns the last known state under mutex protection.
 *
 * @param agent Agent handle (borrowed; may be NULL → returns CREATED).
 * @return Current state.
 */
aegis_agent_state_t aegis_agent_state(const aegis_agent_t* agent);

/**
 * @brief Request initialization.
 *
 * Transitions: CREATED → INITIALIZING → READY
 *
 * Idempotent: if already READY, returns AEGIS_OK.
 *
 * @param agent Agent handle.
 * @return AEGIS_OK on success, or a negative error code.
 */
aegis_status_t aegis_agent_init(aegis_agent_t* agent);

/**
 * @brief Start agent execution.
 *
 * Transitions: READY → RUNNING
 *
 * @param agent Agent handle.
 * @return AEGIS_OK on success, or a negative error code.
 */
aegis_status_t aegis_agent_start(aegis_agent_t* agent);

/**
 * @brief Pause agent execution.
 *
 * Transitions: RUNNING → PAUSED
 *
 * @param agent Agent handle.
 * @return AEGIS_OK on success, or a negative error code.
 */
aegis_status_t aegis_agent_pause(aegis_agent_t* agent);

/**
 * @brief Resume agent execution.
 *
 * Transitions: PAUSED → RUNNING
 *
 * @param agent Agent handle.
 * @return AEGIS_OK on success, or a negative error code.
 */
aegis_status_t aegis_agent_resume(aegis_agent_t* agent);

/**
 * @brief Request cancellation.
 *
 * Transitions: RUNNING → CANCELLING, PAUSED → CANCELLING
 *
 * The agent will transition to CANCELLED once all in-flight work
 * completes. This is asynchronous — use aegis_agent_join() to wait.
 *
 * @param agent Agent handle.
 * @return AEGIS_OK on success, or a negative error code.
 */
aegis_status_t aegis_agent_cancel(aegis_agent_t* agent);

/**
 * @brief Wait for the agent to reach a terminal state.
 *
 * Blocks the caller until the agent is in COMPLETED, FAILED,
 * CANCELLED, or ABORTED state.
 *
 * @param agent  Agent handle.
 * @param timeout_ms  Maximum wait time in milliseconds (0 = wait forever).
 * @return AEGIS_OK if agent reached terminal state,
 *         AEGIS_ERR_TIMEOUT if timeout elapsed,
 *         AEGIS_ERR_INVALID if agent is NULL.
 */
aegis_status_t aegis_agent_join(aegis_agent_t* agent, long timeout_ms);

/* ── Goal ──────────────────────────────────────────────────────────────────── */

/**
 * @brief Set the agent's goal.
 *
 * The goal describes what the agent is trying to achieve.
 *
 * @param agent  Agent handle.
 * @param goal   Goal string (may be NULL to clear). Owned by caller.
 */
void aegis_agent_set_goal(aegis_agent_t* agent, const char* goal);

/**
 * @brief Get the agent's current goal.
 *
 * @param agent Agent handle (borrowed).
 * @return Goal string (NULL if none set). Borrowed, do not free.
 */
const char* aegis_agent_get_goal(const aegis_agent_t* agent);

/* ── Properties ────────────────────────────────────────────────────────────── */

/**
 * @brief Get the agent name.
 *
 * @param agent Agent handle (borrowed).
 * @return Name string (NULL if not set). Borrowed.
 */
const char* aegis_agent_name(const aegis_agent_t* agent);

/**
 * @brief Get the event bus associated with the agent.
 *
 * @param agent Agent handle (borrowed).
 * @return Event bus handle (borrowed; may be NULL).
 */
const aegis_event_bus_t* aegis_agent_event_bus(const aegis_agent_t* agent);

#ifdef __cplusplus
}
#endif

#endif /* AEGIS_AGENT_H */
