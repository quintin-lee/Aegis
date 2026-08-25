#ifndef AEGIS_TYPES_H
#define AEGIS_TYPES_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/**
 * @file aegis_types.h
 * @brief Core type definitions shared across all Aegis modules.
 *
 * This header defines the canonical error codes (aegis_status_t),
 * the capability bitmask (aegis_capability_t), and all opaque
 * agent-level handle declarations. Struct layouts are defined
 * in src/internal/ and must never be exposed to public consumers.
 */

/* ── Status codes ─────────────────────────────────────────────────────────── */

/**
 * @brief Standard Aegis status codes.
 *
 * Positive values are reserved for future use. Negative values
 * indicate errors; 0 (AEGIS_OK) indicates success.
 */
typedef enum aegis_status {
    AEGIS_OK            =  0, /**< Operation succeeded.                              */
    AEGIS_ERR_INTERNAL  = -1, /**< Unexpected internal failure.                      */
    AEGIS_ERR_NOMEM     = -2, /**< Memory allocation failed.                         */
    AEGIS_ERR_INVALID   = -3, /**< Invalid argument supplied.                        */
    AEGIS_ERR_NOT_FOUND = -4, /**< Requested resource was not found.                 */
    AEGIS_ERR_BUSY      = -5, /**< Resource is currently in use; try again later.    */
    AEGIS_ERR_TIMEOUT   = -6, /**< Operation did not complete within the timeout.    */
    AEGIS_ERR_CANCELLED = -7, /**< Operation was cancelled by the caller.            */
    AEGIS_ERR_PERM      = -8, /**< Permission denied by policy.                      */
    AEGIS_ERR_PROVIDER  = -9, /**< Error originating from an external provider.      */
    AEGIS_ERR_TOOL      = -10,/**< Error originating from a tool execution.          */
} aegis_status_t;

/* ── Opaque handles ───────────────────────────────────────────────────────── */

/** Agent — the top-level runtime entity that owns a planner, scheduler, and memory. */
typedef struct aegis_agent    aegis_agent_t;
/** Task — a unit of work scheduled for execution.                           */
typedef struct aegis_task     aegis_task_t;
/** Event — a timestamped occurrence within the runtime.                     */
typedef struct aegis_event    aegis_event_t;
/** Plan — a sequence of tasks produced by the planner.                      */
typedef struct aegis_plan     aegis_plan_t;
/** Goal — the high-level objective the agent is trying to achieve.          */
typedef struct aegis_goal     aegis_goal_t;
/** Memory — working, episodic, semantic, and procedural memory stores.      */
typedef struct aegis_memory   aegis_memory_t;
/** Tool — a callable capability registered with the agent.                  */
typedef struct aegis_tool     aegis_tool_t;
/** Provider — external service (LLM, embedding model, storage) adapter.     */
typedef struct aegis_provider aegis_provider_t;

/* ── Capability flags ─────────────────────────────────────────────────────── */

/**
 * @brief Bitmask of capabilities an agent or tool may request.
 *
 * Each flag represents a class of system operations. A policy engine
 * grants or denies combinations at runtime.
 */
typedef enum aegis_capability {
    AEGIS_CAP_NONE        = 0,     /**< No capabilities.                         */
    AEGIS_CAP_READ_FILE   = (1u << 0),  /**< Read files from the filesystem.       */
    AEGIS_CAP_WRITE_FILE  = (1u << 1),  /**< Write or create files.                */
    AEGIS_CAP_SHELL       = (1u << 2),  /**< Execute shell commands.               */
    AEGIS_CAP_NETWORK     = (1u << 3),  /**< Make outbound network requests.       */
    AEGIS_CAP_RUN_PROCESS = (1u << 4),  /**< Spawn child processes.                */
    AEGIS_CAP_ACCESS_CRED = (1u << 5),  /**< Access credentials / secrets.         */
} aegis_capability_t;

#endif /* AEGIS_TYPES_H */
