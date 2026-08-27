/**
 * @file planner_internal.h
 * @brief Internal layout of the planner module's data structures.
 *
 * Shared between plan.c, planner.c, replanner.c and reflection.c.
 * Not part of any public ABI.
 */
#ifndef AEGIS_PLANNER_INTERNAL_H
#define AEGIS_PLANNER_INTERNAL_H

#include "aegis/plan.h"
#include "aegis/planner.h"
#include "aegis/provider.h"
#include "aegis/task.h"

#include <stddef.h>
#include <stdint.h>

/** One planned step with owned storage. */
typedef struct aegis_plan_step {
    int64_t           id;
    char*             name;      /**< Owned. */
    char*             desc;      /**< Owned, may be NULL. */
    aegis_task_type_t type;
    int               priority;
    char*             tool_name; /**< Owned, may be NULL. */
    void*             input;     /**< Owned copy, may be NULL. */
    size_t            input_len;
    long              timeout_ms;
    aegis_task_retry_policy_t retry;
    int64_t           deps[AEGIS_PLAN_MAX_DEPS];
    size_t            dep_count;
} aegis_plan_step_t;

struct aegis_plan {
    char*            goal;   /**< Owned. */
    uint32_t         version;
    aegis_plan_step_t* steps;   /**< Owned array. */
    size_t           step_count;
    size_t           step_cap;
};

/** Planner instance state (shared by planner.c / replanner.c). */
struct aegis_planner {
    const aegis_provider_registry_t* registry;      /**< Borrowed. */
    char*                            provider_name; /**< Owned copy. */
    /* Optional replaceable-strategy binding (both NULL = built-in DSL path). */
    const aegis_strategy_registry_t* strategies;    /**< Borrowed, may be NULL. */
    char*                            strategy_name; /**< Owned copy, may be NULL. */
};

/** Look up a step by id (linear scan; plan scale is small). Returns NULL when absent. */
const aegis_plan_step_t* aegis_plan_find_step(const aegis_plan_t* plan, int64_t id);

/** Lowest unused step id (0-based). Used to resolve AEGIS_PLAN_STEP_ID_AUTO. */
int64_t aegis_plan_next_free_id(const aegis_plan_t* plan);

/* Internal variant used by the DSL parser: append an already-validated step
 * taking ownership of the given heap strings (they may be NULL for desc). */
aegis_status_t aegis_planner_add_step_owned(aegis_plan_t* plan, int64_t id, char* name, char* desc,
                                            aegis_task_type_t type, int priority, long timeout_ms,
                                            aegis_task_retry_policy_t retry,
                                            const char* tool_name,
                                            const void* input, size_t input_len,
                                            const int64_t* deps, size_t dep_count);

/* Shared LLM round-trip used by planner.c, replanner.c and bundled
 * strategies: send @p prompt through @p registry/@p provider_name, parse
 * the strict line-DSL response into a NEW plan for @p goal and validate
 * it. On any failure nothing is returned and *out is untouched.
 * Dispatch errors propagate verbatim. */
aegis_status_t aegis_planner_generate(const aegis_provider_registry_t* registry,
                                      const char* provider_name, const char* prompt,
                                      const char* goal,
                                      const aegis_cancellation_token_t* token,
                                      aegis_plan_t** out);

/* Compose the shared DSL instruction block plus a labeled body section:
 * "<instructions><body><goal>\n". Output is malloc'd, caller frees. */
aegis_status_t aegis_planner_compose_prompt(const char* body, const char* goal, char** out);

#endif /* AEGIS_PLANNER_INTERNAL_H */
