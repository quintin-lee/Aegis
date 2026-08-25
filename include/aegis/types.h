#ifndef AEGIS_TYPES_H
#define AEGIS_TYPES_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/**
 * @file aegis_types.h
 * @brief Core type definitions and opaque handles.
 *
 * All public structs are opaque; layouts defined in src/internal/.
 */

/* ── Status codes ─────────────────────────────────────────────────────────── */

typedef enum aegis_status {
    AEGIS_OK             =  0,
    AEGIS_ERR_INTERNAL   = -1,
    AEGIS_ERR_NOMEM      = -2,
    AEGIS_ERR_INVALID    = -3,
    AEGIS_ERR_NOT_FOUND  = -4,
    AEGIS_ERR_BUSY       = -5,
    AEGIS_ERR_TIMEOUT    = -6,
    AEGIS_ERR_CANCELLED  = -7,
    AEGIS_ERR_PERM       = -8,
    AEGIS_ERR_PROVIDER   = -9,
    AEGIS_ERR_TOOL       = -10,
} aegis_status_t;

/* ── Opaque handles ───────────────────────────────────────────────────────── */

typedef struct aegis_agent  aegis_agent_t;
typedef struct aegis_task   aegis_task_t;
typedef struct aegis_event  aegis_event_t;
typedef struct aegis_plan   aegis_plan_t;
typedef struct aegis_goal   aegis_goal_t;
typedef struct aegis_memory aegis_memory_t;
typedef struct aegis_tool   aegis_tool_t;
typedef struct aegis_provider aegis_provider_t;

/* ── Capability flags ─────────────────────────────────────────────────────── */

typedef enum aegis_capability {
    AEGIS_CAP_NONE          = 0,
    AEGIS_CAP_READ_FILE     = (1u << 0),
    AEGIS_CAP_WRITE_FILE    = (1u << 1),
    AEGIS_CAP_SHELL         = (1u << 2),
    AEGIS_CAP_NETWORK       = (1u << 3),
    AEGIS_CAP_RUN_PROCESS   = (1u << 4),
    AEGIS_CAP_ACCESS_CRED   = (1u << 5),
} aegis_capability_t;

#endif /* AEGIS_TYPES_H */
