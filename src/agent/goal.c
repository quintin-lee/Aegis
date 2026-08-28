/**
 * @file goal.c
 * @brief Goal abstraction — not yet integrated into the agent.
 *
 * This module provides a lightweight goal descriptor that can be
 * associated with an agent. The actual planning logic is deferred
 * to the planner module (not implemented in this iteration).
 */
#include "aegis/agent/agent.h"
#include "agent_internal.h"
#include "lifecycle.h"

/* Goal is managed inline within the agent struct.
 * This file exists as an extension point for future goal-specific APIs. */
