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
#ifdef AEGIS_OPENAI_PROVIDER
#include "structured_openai.h"
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stddef.h>

#define CODING_AGENT_SYSTEM_PROMPT "You are a coding agent. Use tools to help the user."

struct aegis_coding_agent {
    aegis_session_t*      session;
    aegis_model_client_t* model;
    char*                 model_name;
    char*                 provider;
    char*                 api_key;
    char*                 base_url;
#ifdef AEGIS_OPENAI_PROVIDER
    aegis_openai_model_ctx_t* openai_model;
#endif
    aegis_tool_registry_t*  tools;
    aegis_mutation_queue_t* mq;
    aegis_agent_loop_t*     loop;
    aegis_skill_registry_t* skills;
    aegis_agent_event_fn    ev_fn;   /**< Borrowed observer; NULL disables. */
    void*                   ev_user; /**< Borrowed, passed to ev_fn.        */
    bool                    owns_tools;
};

static char* dup_or_null(const char* s)
{
    return s ? strdup(s) : NULL;
}

#ifdef AEGIS_OPENAI_PROVIDER
static aegis_status_t build_openai_model(aegis_coding_agent_t*         a,
                                         const char*                   model_name,
                                         aegis_openai_model_ctx_t**    out_ctx,
                                         aegis_model_client_t**        out_client)
{
    aegis_model_backend_t backend = {0};
    aegis_status_t        st =
        aegis_openai_model_create(a->api_key, a->base_url, model_name, out_ctx, &backend);
    if (st != AEGIS_OK) {
        return st;
    }
    st = aegis_model_client_create_with_backend(model_name, &backend, out_client);
    if (st != AEGIS_OK) {
        aegis_openai_model_destroy(*out_ctx);
        *out_ctx = NULL;
    }
    return st;
}
#endif

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
    // Keep provider configuration so set_model() can rebuild the backend later.
    a->model_name = strdup(model_name);
    a->provider   = dup_or_null(cfg->provider);
    a->api_key    = dup_or_null(cfg->api_key);
    a->base_url   = dup_or_null(cfg->base_url);
    if (!a->model_name) {
        aegis_session_destroy(a->session);
        free(a);
        return AEGIS_ERR_NOMEM;
    }
#ifdef AEGIS_OPENAI_PROVIDER
    if (a->provider && strcmp(a->provider, "llm-openai") == 0) {
        st = build_openai_model(a, model_name, &a->openai_model, &a->model);
    } else
#endif
    {
        st = aegis_model_client_create(model_name, &a->model);
    }
    if (st != AEGIS_OK) {
        free(a->model_name);
        free(a->provider);
        free(a->api_key);
        free(a->base_url);
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
            snprintf(proj_path, sizeof(proj_path), "%s/.aegis/skills",
                     cfg->project_root ? cfg->project_root : ".");
            aegis_skill_loader_load_dir(a->skills, proj_path);
        }
    }

    aegis_agent_loop_config_t lcfg;
    memset(&lcfg, 0, sizeof(lcfg));
    lcfg.session       = a->session;
    lcfg.model         = a->model;
    lcfg.tools         = a->tools;
    lcfg.system_prompt = CODING_AGENT_SYSTEM_PROMPT;
    lcfg.on_event      = a->ev_fn;
    lcfg.event_user    = a->ev_user;
    st                 = aegis_agent_loop_create(&lcfg, &a->loop);
    if (st != AEGIS_OK) {
        if (a->owns_tools) {
            if (a->skills) {
                aegis_skill_registry_destroy(a->skills);
            }
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
    if (a->skills) {
        aegis_skill_registry_destroy(a->skills);
    }
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
#ifdef AEGIS_OPENAI_PROVIDER
    if (a->openai_model) {
        aegis_openai_model_destroy(a->openai_model);
    }
#endif
    if (a->session) {
        aegis_session_destroy(a->session);
    }
    free(a->model_name);
    free(a->provider);
    free(a->api_key);
    free(a->base_url);
    free(a);
}

aegis_session_t* aegis_coding_agent_session(aegis_coding_agent_t* a)
{
    return a ? a->session : NULL;
}

aegis_status_t aegis_coding_agent_replace_session(aegis_coding_agent_t* a, aegis_session_t* session)
{
    if (!a || !session) {
        return AEGIS_ERR_INVALID;
    }
    aegis_agent_loop_t*       replacement = NULL;
    aegis_agent_loop_config_t cfg         = {
        .session       = session,
        .model         = a->model,
        .tools         = a->tools,
        .system_prompt = CODING_AGENT_SYSTEM_PROMPT,
        .on_event      = a->ev_fn,
        .event_user    = a->ev_user,
    };
    aegis_status_t st = aegis_agent_loop_create(&cfg, &replacement);
    if (st != AEGIS_OK) {
        return st;
    }
    aegis_agent_loop_t* old_loop    = a->loop;
    aegis_session_t*    old_session = a->session;
    a->loop                         = replacement;
    a->session                      = session;
    aegis_agent_loop_destroy(old_loop);
    aegis_session_destroy(old_session);
    return AEGIS_OK;
}

aegis_status_t aegis_coding_agent_run(aegis_coding_agent_t* a, const char* user_input)
{
    if (!a || !user_input) {
        return AEGIS_ERR_INVALID;
    }
    return aegis_agent_loop_run(a->loop, user_input);
}

const char* aegis_coding_agent_model_name(const aegis_coding_agent_t* a)
{
    return a ? a->model_name : NULL;
}

aegis_status_t aegis_coding_agent_set_event_callback(aegis_coding_agent_t* a,
                                                     aegis_agent_event_fn  fn,
                                                     void*                 user)
{
    if (!a) {
        return AEGIS_ERR_INVALID;
    }
    a->ev_fn   = fn;
    a->ev_user = user;
    return aegis_agent_loop_set_event_callback(a->loop, fn, user);
}

aegis_status_t aegis_coding_agent_set_model(aegis_coding_agent_t* a, const char* model)
{
    if (!a || !model || model[0] == '\0') {
        return AEGIS_ERR_INVALID;
    }
#ifdef AEGIS_OPENAI_PROVIDER
    aegis_openai_model_ctx_t* new_ctx    = NULL;
    aegis_model_client_t*     new_client = NULL;
    aegis_status_t            st;
    if (a->provider && strcmp(a->provider, "llm-openai") == 0) {
        st = build_openai_model(a, model, &new_ctx, &new_client);
    } else
#endif
    {
        st = aegis_model_client_create(model, &new_client);
    }
    if (st != AEGIS_OK) {
        return st;
    }

    // Fresh loop bound to the new client; session/tools unchanged.
    aegis_agent_loop_t*       replacement = NULL;
    aegis_agent_loop_config_t lcfg        = {
        .session       = a->session,
        .model         = new_client,
        .tools         = a->tools,
        .system_prompt = CODING_AGENT_SYSTEM_PROMPT,
        .on_event      = a->ev_fn,
        .event_user    = a->ev_user,
    };
    st = aegis_agent_loop_create(&lcfg, &replacement);
    if (st != AEGIS_OK) {
        aegis_model_client_destroy(new_client);
#ifdef AEGIS_OPENAI_PROVIDER
        if (new_ctx) {
            aegis_openai_model_destroy(new_ctx);
        }
#endif
        return st;
    }

    // Atomic swap: new loop/client in, old ones destroyed.
    aegis_agent_loop_t*   old_loop    = a->loop;
    aegis_model_client_t* old_client  = a->model;
    char*                 old_name    = a->model_name;
#ifdef AEGIS_OPENAI_PROVIDER
    aegis_openai_model_ctx_t* old_ctx  = a->openai_model;
#endif
    char*                 new_name    = strdup(model);
    if (!new_name) {
        // Out of memory: keep everything old, discard the replacement.
        aegis_agent_loop_destroy(replacement);
        aegis_model_client_destroy(new_client);
#ifdef AEGIS_OPENAI_PROVIDER
        if (new_ctx) {
            aegis_openai_model_destroy(new_ctx);
        }
#endif
        return AEGIS_ERR_NOMEM;
    }
    a->loop        = replacement;
    a->model       = new_client;
    a->model_name  = new_name;
#ifdef AEGIS_OPENAI_PROVIDER
    a->openai_model = new_ctx;
#endif
    aegis_agent_loop_destroy(old_loop);
    aegis_model_client_destroy(old_client);
#ifdef AEGIS_OPENAI_PROVIDER
    if (old_ctx) {
        aegis_openai_model_destroy(old_ctx);
    }
#endif
    free(old_name);
    return AEGIS_OK;
}