#ifndef AEGIS_STRATEGY_AUTONOMOUS_H
#define AEGIS_STRATEGY_AUTONOMOUS_H

#include "aegis/agent/strategy.h"
#include "aegis/autonomous_agent.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file autonomous_strategy.h
 * @brief AutonomousStrategy — wraps Goal→Plan→Graph→Scheduler→Executor as a pluggable strategy.
 */

typedef struct aegis_autonomous_strategy aegis_autonomous_strategy_t;

aegis_status_t aegis_autonomous_strategy_create(const aegis_autonomous_agent_config_t* cfg,
                                                aegis_autonomous_strategy_t**          out);
void           aegis_autonomous_strategy_destroy(aegis_autonomous_strategy_t* s);

// Returns a strategy_def that can be registered with agent loop
const aegis_agent_strategy_def_t* aegis_autonomous_strategy_def(aegis_autonomous_strategy_t* s);

#ifdef __cplusplus
}
#endif

#endif /* AEGIS_STRATEGY_AUTONOMOUS_H */
