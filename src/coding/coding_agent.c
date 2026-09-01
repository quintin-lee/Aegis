#define _POSIX_C_SOURCE 200809L
#include "aegis/coding/coding_agent.h"
#include "aegis/coding/coding_tools.h"
#include "aegis/coding/mutations.h"
#include "aegis/skill/registry.h"
#include "aegis/skill/loader.h"
#include "aegis/session/session.h"
#include "aegis/agent/loop.h"
#include "aegis/model/model.h"
#include "aegis/tool/tool.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct aegis_coding_agent {
    aegis_session_t*        session;
    aegis_model_client_t*   model;
    aegis_tool_registry_t*  tools;
    aegis_mutation_queue_t* mq;
    aegis_agent_loop_t*     loop;
    aegis_skill_registry_t* skills;
    bool                    owns_tools;
};

aegis_status_t aegis_coding_agent_create(const aegis_coding_agent_config_t* cfg,
                                         aegis_coding_agent_t**             out)
{
    if (!cfg || !out) {
        return AEGIS_ERR_INVALID;
    }
    aegis_coding_agent_t* a = (aegis_coding_agent_t*)calloc(1, sizeof(*a));
    if (!a) {
        return AEGIS_ERR_NOMEM;
    }

    aegis_status_t st = aegis_session_create(cfg->project_root, &a->session);
    if (st != AEGIS_OK) {
        free(a);
        return st;
    }

    const char* model_name = cfg->model ? cfg->model : "mock";
    st                     = aegis_model_client_create(model_name, &a->model);
    if (st != AEGIS_OK) {
        aegis_session_destroy(a->session);
        free(a);
        return st;
    }

    if (cfg->tools) {
        a->tools      = cfg->tools;
        a->owns_tools = false;
    } else {
        st = aegis_tool_registry_create(&a->tools);
        if (st != AEGIS_OK) {
            aegis_model_client_destroy(a->model);
            aegis_session_destroy(a->session);
            free(a);
            return st;
        }
        a->owns_tools = true;
        st            = aegis_mutation_queue_create(&a->mq);
        if (st != AEGIS_OK) {
            aegis_tool_registry_destroy(a->tools);
            aegis_model_client_destroy(a->model);
            aegis_session_destroy(a->session);
            free(a);
            return st;
        }
        st = aegis_coding_tools_register_all(a->tools, a->mq);
        if (st != AEGIS_OK) {
            aegis_mutation_queue_destroy(a->mq);
            aegis_tool_registry_destroy(a->tools);
            aegis_model_client_destroy(a->model);
            aegis_session_destroy(a->session);
            free(a);
            return st;
        }
        // Load skills (best effort, not fatal)
        aegis_skill_registry_create(&a->skills);
        if (a->skills) {
            const char* home = getenv("HOME");
            if (home) {
                char path[1024];
                snprintf(path, sizeof(path), "%s/.aegis/skills", home);
                aegis_skill_loader_load_dir(a->skills, path);
            }
            char proj_path[1024];
            snprintf(proj_path, sizeof(proj_path), "%s/.aegis/skills", cfg->project_root ? cfg->project_root : ".");
            aegis_skill_loader_load_dir(a->skills, proj_path);
        }
    }

    aegis_agent_loop_config_t lcfg = {
        .model         = a->model,
        .tools         = a->tools,
        .system_prompt = "You are a coding agent. Use tools to help the user.",
    };
    st = aegis_agent_loop_create(&lcfg, &a->loop);
    if (st != AEGIS_OK) {
        if (a->owns_tools) {
            if (a->skills) aegis_skill_registry_destroy(a->skills);
            aegis_mutation_queue_destroy(a->mq);
            aegis_tool_registry_destroy(a->tools);
        }
        aegis_model_client_destroy(a->model);
        aegis_session_destroy(a->session);
        free(a);
        return st;
    }

    *out = a;
    return AEGIS_OK;
}

void aegis_coding_agent_destroy(aegis_coding_agent_t* a)
{
    if (!a) {
        return;
    }
    if (a->loop) {
        aegis_agent_loop_destroy(a->loop);
    }
    if (a->skills) aegis_skill_registry_destroy(a->skills);
    if (a->owns_tools) {
        if (a->mq) {
            aegis_mutation_queue_destroy(a->mq);
        }
        if (a->tools) {
            aegis_tool_registry_destroy(a->tools);
        }
    }
    if (a->model) {
        aegis_model_client_destroy(a->model);
    }
    if (a->session) {
        aegis_session_destroy(a->session);
    }
    free(a);
}

aegis_session_t* aegis_coding_agent_session(aegis_coding_agent_t* a)
{
    return a ? a->session : NULL;
}

aegis_status_t aegis_coding_agent_run(aegis_coding_agent_t* a, const char* user_input)
{
    if (!a || !user_input) {
        return AEGIS_ERR_INVALID;
    }
    return aegis_agent_loop_run(a->loop, user_input);
}
