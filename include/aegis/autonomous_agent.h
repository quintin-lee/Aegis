/**
 * @file autonomous_agent.h
 * @brief Autonomous closed-loop orchestrator
 * (Goal→Context→Planner→Plan→TaskGraph→Scheduler→Executor→Tool→Observation→Critic→Reflection→Replanner→NewPlan)
 *
 * Orchestrator lives in Plugin/Application layer and composes existing
 * public APIs only (include/aegis headers). It never accesses src/internal.
 * Supports: failure-driven replan, task-level + round-level cancellation,
 * per-task timeout, checkpoint double-write and restore.
 */
#ifndef AEGIS_AUTONOMOUS_AGENT_H
#define AEGIS_AUTONOMOUS_AGENT_H

#include "aegis/types.h"
#include "aegis/status.h"
#include "aegis/executor/cancellation.h"
#include "aegis/provider/provider.h"
#include "aegis/tool/tool.h"
#include "aegis/security/security.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Opaque autonomous agent handle. */
typedef struct aegis_autonomous_agent aegis_autonomous_agent_t;

/**
 * @brief Configuration for autonomous agent (all pointers borrowed).
 *
 * Provider registry + llm name are required to create the internal planner.
 * Storage + checkpoint_path are optional (checkpoint disabled when NULL).
 */
typedef struct aegis_autonomous_agent_config {
    const aegis_provider_registry_t* provider_registry; /**< Borrowed, required. */
    const char*                      llm_provider_name; /**< Borrowed, required. */
    const char*                      checkpoint_path;   /**< Borrowed, optional. */
    aegis_cancellation_token_t*    cancel_token; /**< Borrowed, optional. Internal token if NULL. */
    const aegis_tool_registry_t*   tool_registry;           /**< Borrowed, optional. */
    const aegis_security_policy_t* security_policy;         /**< Borrowed, optional. */
    uint32_t                       max_iterations;          /**< 0 => 5. */
    uint64_t                       default_task_timeout_ns; /**< 0 => no timeout. */
} aegis_autonomous_agent_config_t;

/** Result of a run. */
typedef struct aegis_autonomous_result {
    aegis_status_t final_status;
    uint32_t       iterations;
    uint32_t       tasks_executed;
    bool           recovered_from_checkpoint;
} aegis_autonomous_result_t;

/**
 * @brief Create autonomous agent.
 * @param[out] out  Receives handle. Ownership: transferred.
 * @param[in]  cfg  Borrowed config (required, fields validated).
 * @return AEGIS_OK or error.
 */
aegis_status_t aegis_autonomous_agent_create(aegis_autonomous_agent_t**             out,
                                             const aegis_autonomous_agent_config_t* cfg);

/** Destroy. Safe with NULL. */
void aegis_autonomous_agent_destroy(aegis_autonomous_agent_t* aa);

/**
 * @brief Run autonomous loop for a goal.
 * @param aa        Agent (borrowed).
 * @param goal_text Goal text (borrowed, required non-empty).
 * @param out_result Optional result (borrowed).
 * @return AEGIS_OK on success (critic SUCCESS), or error/cancel/timeout/max_iterations.
 */
aegis_status_t aegis_autonomous_agent_run(aegis_autonomous_agent_t* aa, const char* goal_text,
                                          aegis_autonomous_result_t* out_result);

/**
 * @brief Request cooperative cancellation (thread-safe, idempotent).
 */
aegis_status_t aegis_autonomous_agent_cancel(aegis_autonomous_agent_t* aa);

/**
 * @brief Persist current state to path (or cfg path). Requires checkpoint configured.
 */
aegis_status_t aegis_autonomous_agent_checkpoint_save(aegis_autonomous_agent_t* aa,
                                                      const char*               path);

/**
 * @brief Restore from checkpoint file.
 */
aegis_status_t aegis_autonomous_agent_restore(aegis_autonomous_agent_t* aa, const char* path);

#ifdef __cplusplus
}
#endif

#endif /* AEGIS_AUTONOMOUS_AGENT_H */
