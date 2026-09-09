#ifndef AEGIS_AGENT_STRATEGY_H
#define AEGIS_AGENT_STRATEGY_H

#include "aegis/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file strategy.h
 * @brief Pluggable Agent Loop Strategy ABI.
 *
 * A loop strategy is a lifecycle plug-point for the reactive agent loop
 * (see agent/loop.h): hooks fire around turn / model / tool boundaries and
 * decide whether the loop runs another turn. The bundled AutonomousStrategy
 * (strategy/autonomous_strategy.h) implements goal-driven
 * plan→execute→evaluate→reflect→replan on top of these hooks.
 *
 * This is NOT the plan-level strategy ABI (strategy/strategy.h), which is
 * a policy plug-point for the planner producing aegis_plan_t objects. The
 * two ABIs are intentionally separate: plan-level never sees the loop, and
 * loop-level never produces plans directly.
 *
 * Ownership: definition strings (name/description) are borrowed and must
 * outlive any use of the def; `user` is owned by whoever created the
 * strategy object. Produced state escapes only through loop accessors.
 *
 * Thread safety: all hooks are invoked WITHOUT loop locks held (same rule
 * as the loop observer/approval callbacks). Hooks must honor the loop's
 * cancellation token cooperatively.
 */

/** Forward declaration; defined in agent/loop.h (never include it here —
 *  loop.c includes both headers). Borrowed in all hooks. */
struct aegis_agent_loop;
typedef struct aegis_agent_loop aegis_agent_loop_t;

/** ABI version of the loop-strategy interface. Bump on breaking changes. */
#define AEGIS_AGENT_STRATEGY_ABI_VERSION 1u

typedef struct aegis_agent_strategy_def {
    const char* name;
    const char* description;
    /** Must equal AEGIS_AGENT_STRATEGY_ABI_VERSION. */
    uint32_t abi_version;
    aegis_status_t (*init)(void* user);
    aegis_status_t (*before_turn)(void* user, aegis_agent_loop_t* loop);
    aegis_status_t (*after_model)(void* user, aegis_agent_loop_t* loop);
    aegis_status_t (*after_tool)(void* user, aegis_agent_loop_t* loop);
    aegis_status_t (*should_continue)(void* user, int* out_continue);
    aegis_status_t (*shutdown)(void* user);
    void* user;
} aegis_agent_strategy_def_t;

#ifdef __cplusplus
}
#endif

#endif /* AEGIS_AGENT_STRATEGY_H */
