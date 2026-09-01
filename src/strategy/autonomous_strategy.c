#define _POSIX_C_SOURCE 200809L
#include "aegis/strategy/autonomous_strategy.h"
#include "aegis/agent/strategy.h"
#include "aegis/autonomous_agent.h"
#include <stdlib.h>

struct aegis_autonomous_strategy {
    aegis_autonomous_agent_t*  agent;
    aegis_agent_strategy_def_t def;
};

static aegis_status_t strat_init(void* user)
{
    (void)user;
    return AEGIS_OK;
}

static aegis_status_t strat_shutdown(void* user)
{
    (void)user;
    return AEGIS_OK;
}

aegis_status_t aegis_autonomous_strategy_create(const aegis_autonomous_agent_config_t* cfg,
                                                aegis_autonomous_strategy_t**          out)
{
    if (!cfg || !out) {
        return AEGIS_ERR_INVALID;
    }
    aegis_autonomous_strategy_t* s = (aegis_autonomous_strategy_t*)calloc(1, sizeof(*s));
    if (!s) {
        return AEGIS_ERR_NOMEM;
    }
    aegis_status_t st = aegis_autonomous_agent_create(&s->agent, cfg);
    if (st != AEGIS_OK) {
        free(s);
        return st;
    }
    s->def.name            = "autonomous";
    s->def.description     = "Goal→Plan→Graph→Scheduler→Executor→Evaluate→Reflect→Replan";
    s->def.init            = strat_init;
    s->def.shutdown        = strat_shutdown;
    s->def.before_turn     = NULL;
    s->def.after_model     = NULL;
    s->def.after_tool      = NULL;
    s->def.should_continue = NULL;
    s->def.user            = s;
    *out                   = s;
    return AEGIS_OK;
}

void aegis_autonomous_strategy_destroy(aegis_autonomous_strategy_t* s)
{
    if (!s) {
        return;
    }
    if (s->agent) {
        aegis_autonomous_agent_destroy(s->agent);
    }
    free(s);
}

const aegis_agent_strategy_def_t* aegis_autonomous_strategy_def(aegis_autonomous_strategy_t* s)
{
    return s ? &s->def : NULL;
}
