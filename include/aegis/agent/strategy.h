#ifndef AEGIS_AGENT_STRATEGY_H
#define AEGIS_AGENT_STRATEGY_H

#include "aegis/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file strategy.h
 * @brief Pluggable Agent Strategy ABI.
 */

typedef struct aegis_agent_strategy_def {
    const char* name;
    const char* description;
    aegis_status_t (*init)(void* user);
    aegis_status_t (*before_turn)(void* user, void* loop);
    aegis_status_t (*after_model)(void* user, void* loop);
    aegis_status_t (*after_tool)(void* user, void* loop);
    aegis_status_t (*should_continue)(void* user, int* out_continue);
    aegis_status_t (*shutdown)(void* user);
    void* user;
} aegis_agent_strategy_def_t;

#ifdef __cplusplus
}
#endif

#endif /* AEGIS_AGENT_STRATEGY_H */
