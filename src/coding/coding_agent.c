/**
 * @file coding_agent.c
 * @brief Pi-like coding agent implementation: owns session, agent loop,
 * model client, tool registry and skill set. Entry points for create/
 * destroy/run/interrupt plus model hot-switch and session replacement;
 * mock model by default, OpenAI or Anthropic backend when configured.
 */
#define _POSIX_C_SOURCE 200809L
#include "aegis/coding/coding_agent.h"
#include "aegis/coding/coding_tools.h"
#include "aegis/coding/mutations.h"
#include "aegis/skill/registry.h"
#include "aegis/skill/loader.h"
#include "aegis/session/session.h"
#include "aegis/agent/loop.h"
#include "aegis/common/cancellation/cancellation.h"
#include "aegis/model/model.h"
#include "aegis/tool/tool.h"
#ifdef AEGIS_OPENAI_PROVIDER
#include "structured_openai.h"
#endif
#ifdef AEGIS_ANTHROPIC_PROVIDER
#include "structured_anthropic.h"
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
    void*                  provider_ctx;    /**< Opaque backend ctx (OpenAI/Anthropic). */
    void (*provider_destroy)(void*);        /**< Matching destroy fn; NULL when unset. */
    aegis_tool_registry_t*      tools;
    aegis_mutation_queue_t*     mq;
    aegis_agent_loop_t*         loop;
    aegis_cancellation_token_t* token; /**< Per-turn; recreated each run. */
    aegis_skill_registry_t*     skills;
    aegis_agent_event_fn        ev_fn;   /**< Borrowed observer; NULL disables. */
    void*                       ev_user; /**< Borrowed, passed to ev_fn.        */
    aegis_tool_approval_fn      ap_fn;   /**< Borrowed gate; NULL = allow all.  */
    void*                       ap_user; /**< Borrowed, passed to ap_fn.        */
    bool                        owns_tools;
};

/**
 * @brief Duplicate a string, tolerating NULL.
 *
 * @param[in] s Source string, or NULL.
 * @return Heap copy of @p s, or NULL when @p s is NULL or out of memory.
 *   The caller owns the result and must free() it.
 */
static char* dup_or_null(const char* s)
{
    return s ? strdup(s) : NULL;
}

#ifdef AEGIS_OPENAI_PROVIDER
/**
 * @brief Build the OpenAI-backed model client for the coding agent.
 *
 * Creates the provider context first, then wraps it in a model client. On
 * client-creation failure the provider context is destroyed and @p *out_ctx
 * is reset to NULL so the caller never sees a half-built pair.
 *
 * @param[in]  a          Agent holding the API key / base URL configuration.
 * @param[in]  model_name Model identifier passed to the provider.
 * @param[out] out_ctx    Receives the new provider context (NULL on failure).
 * @param[out] out_client Receives the new model client (untouched on failure).
 * @return AEGIS_OK on success, otherwise the provider/client error.
 */
static aegis_status_t build_openai_model(aegis_coding_agent_t* a, const char* model_name,
                                         void**                 out_ctx,
                                         aegis_model_client_t** out_client)
{
    aegis_model_backend_t     backend = {0};
    aegis_openai_model_ctx_t* ctx    = NULL;
    aegis_status_t            st =
        aegis_openai_model_create(a->api_key, a->base_url, model_name, &ctx, &backend);
    if (st != AEGIS_OK) {
        return st;
    }
    *out_ctx = ctx;
    st        = aegis_model_client_create_with_backend(model_name, &backend, out_client);
    if (st != AEGIS_OK) {
        aegis_openai_model_destroy(ctx);
        *out_ctx = NULL;
    }
    return st;
}

void destroy_openai_ctx(void* ctx)
{
    aegis_openai_model_destroy((aegis_openai_model_ctx_t*)ctx);
}
#endif

#ifdef AEGIS_ANTHROPIC_PROVIDER
/**
 * @brief Build the Anthropic-backed model client for the coding agent.
 *
 * Mirrors build_openai_model: creates the provider context, wraps it in a
 * model client, and resets the ctx on client-creation failure.
 *
 * @param[in]  a          Agent holding the API key / base URL configuration.
 * @param[in]  model_name Model identifier passed to the provider.
 * @param[out] out_ctx    Receives the new provider context (NULL on failure).
 * @param[out] out_client Receives the new model client (untouched on failure).
 * @return AEGIS_OK on success, otherwise the provider/client error.
 */
static aegis_status_t build_anthropic_model(aegis_coding_agent_t* a, const char* model_name,
                                            void**                 out_ctx,
                                            aegis_model_client_t** out_client)
{
    aegis_model_backend_t       backend = {0};
    aegis_anthropic_model_ctx_t* ctx    = NULL;
    aegis_status_t              st =
        aegis_anthropic_model_create(a->api_key, a->base_url, model_name, &ctx, &backend);
    if (st != AEGIS_OK) {
        return st;
    }
    *out_ctx = ctx;
    st        = aegis_model_client_create_with_backend(model_name, &backend, out_client);
    if (st != AEGIS_OK) {
        aegis_anthropic_model_destroy(ctx);
        *out_ctx = NULL;
    }
    return st;
}

void destroy_anthropic_ctx(void* ctx)
{
    aegis_anthropic_model_destroy((aegis_anthropic_model_ctx_t*)ctx);
}
#endif

/**
 * @brief Create a coding agent: session, model client, tools and agent loop.
 *
 * Uses the caller-supplied tool registry when @p cfg->tools is set
 * (borrowed, never destroyed); otherwise builds an owned registry with the
 * built-in coding tools, a mutation queue and best-effort skill loading from
 * $HOME/.aegis/skills and <project_root>/.aegis/skills. The model defaults to
 * "mock" unless @p cfg->model names another backend ("llm-openai" provider
 * when compiled with AEGIS_OPENAI_PROVIDER). Provider strings are retained so
 * set_model() can rebuild the backend later. Event/approval callbacks are not
 * part of the config; install them afterwards with the set_* accessors.
 * Any failure unwinds all already-created sub-objects.
 *
 * @param[in]  cfg Configuration (project root, model, provider, tools).
 * @param[out] out Receives the new agent; untouched on failure.
 * @return AEGIS_OK on success, AEGIS_ERR_INVALID on NULL args,
 *   AEGIS_ERR_NOMEM on allocation failure, else the sub-object error.
 */
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
    a->provider_destroy = NULL;
#ifdef AEGIS_OPENAI_PROVIDER
    if (a->provider && strcmp(a->provider, "llm-openai") == 0) {
        st = build_openai_model(a, model_name, &a->provider_ctx, &a->model);
        a->provider_destroy = destroy_openai_ctx;
    } else
#endif
#ifdef AEGIS_ANTHROPIC_PROVIDER
    if (a->provider && strcmp(a->provider, "llm-anthropic") == 0) {
        st = build_anthropic_model(a, model_name, &a->provider_ctx, &a->model);
        a->provider_destroy = destroy_anthropic_ctx;
    } else
#endif
    {
        st = aegis_model_client_create(model_name, &a->model);
    }
    if (st != AEGIS_OK) {
        if (a->provider_destroy) {
            a->provider_destroy(a->provider_ctx);
        }
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
    lcfg.token         = a->token;
    lcfg.tool_approval = a->ap_fn;
    lcfg.approval_user = a->ap_user;
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

/**
 * @brief Destroy a coding agent and every sub-object it owns.
 *
 * Destroys the loop, skills, model client/session/token and heap strings.
 * The tool registry and mutation queue are destroyed only when the agent
 * owns them (built internally); a caller-supplied registry is left alone.
 * NULL is a no-op.
 *
 * @param[in] a Agent to destroy, or NULL.
 */
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
    if (a->provider_destroy) {
        a->provider_destroy(a->provider_ctx);
    }
    if (a->session) {
        aegis_session_destroy(a->session);
    }
    if (a->token) {
        aegis_cancellation_token_destroy(a->token);
    }
    free(a->model_name);
    free(a->provider);
    free(a->api_key);
    free(a->base_url);
    free(a);
}

/**
 * @brief Borrow the agent's session.
 *
 * @param[in] a Agent, or NULL.
 * @return The owned session pointer (do NOT destroy), or NULL when @p a
 *   is NULL.
 */
aegis_session_t* aegis_coding_agent_session(aegis_coding_agent_t* a)
{
    return a ? a->session : NULL;
}

/**
 * @brief Replace the agent's session, rebuilding the agent loop around it.
 *
 * Builds the replacement loop first; only on success are the old loop and
 * old session destroyed and the agent repointed, so a failure leaves the
 * agent untouched. The agent takes ownership of @p session on success.
 *
 * @param[in] a       Agent to update.
 * @param[in] session New session; consumed on success, untouched on failure.
 * @return AEGIS_OK on success, AEGIS_ERR_INVALID on NULL args, else the
 *   loop-creation error (agent unchanged).
 */
aegis_status_t aegis_coding_agent_replace_session(aegis_coding_agent_t* a, aegis_session_t* session)
{
    if (!a || !session) {
        return AEGIS_ERR_INVALID;
    }
    aegis_agent_loop_set_token(a->loop, NULL); /* unbind before rebuild */
    aegis_agent_loop_t*       replacement = NULL;
    aegis_agent_loop_config_t cfg         = {
        .session       = session,
        .model         = a->model,
        .tools         = a->tools,
        .system_prompt = CODING_AGENT_SYSTEM_PROMPT,
        .on_event      = a->ev_fn,
        .event_user    = a->ev_user,
        .token         = a->token,
        .tool_approval = a->ap_fn,
        .approval_user = a->ap_user,
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

/**
 * @brief Run one user turn through the agent loop.
 *
 * Creates a fresh cancellation token per turn (cancellation is one-shot and
 * must not leak into the next run), binds it to the loop, then runs to
 * completion. When the loop reports dropped context, the session is compacted
 * as a best-effort follow-up. Not thread-safe with concurrent run/interrupt
 * calls on the same agent.
 *
 * @param[in] a          Agent to run.
 * @param[in] user_input User message starting the turn.
 * @return AEGIS_OK on success, AEGIS_ERR_INVALID on NULL args, else the
 *   token-creation or loop error.
 */
aegis_status_t aegis_coding_agent_run(aegis_coding_agent_t* a, const char* user_input)
{
    if (!a || !user_input) {
        return AEGIS_ERR_INVALID;
    }
    /* Fresh token per turn: cancellation is one-shot and must not leak
     * into the next run. */
    aegis_cancellation_token_t* tok = NULL;
    aegis_status_t              st  = aegis_cancellation_token_create(&tok);
    if (st != AEGIS_OK) {
        return st;
    }
    if (a->token) {
        aegis_cancellation_token_destroy(a->token);
    }
    a->token = tok;
    aegis_agent_loop_set_token(a->loop, tok);
    st = aegis_agent_loop_run(a->loop, user_input);
    if (aegis_agent_loop_context_dropped(a->loop) > 0) {
        (void)aegis_session_compact(a->session, AEGIS_LOOP_CONTEXT_WINDOW);
    }
    return st;
}

/**
 * @brief Request cancellation of the in-flight turn.
 *
 * Signals the loop's bound cancellation token; the turn unwinds at the next
 * cancellation checkpoint. Safe to call from another thread while run() is
 * executing.
 *
 * @param[in] a Agent whose turn should stop.
 * @return AEGIS_OK on success, AEGIS_ERR_INVALID on NULL agent/loop, else
 *   the loop-cancel error.
 */
aegis_status_t aegis_coding_agent_interrupt(const aegis_coding_agent_t* a)
{
    if (!a || !a->loop) {
        return AEGIS_ERR_INVALID;
    }
    return aegis_agent_loop_cancel((aegis_agent_loop_t*)a->loop);
}

/**
 * @brief Hot-swap the model client, rebuilding the loop around it.
 *
 * Builds the replacement loop first; only on success are the old loop and
 * old client destroyed and the agent repointed. The agent takes ownership of
 * @p client on success; on failure @p client stays with the caller.
 * Session, tools and event/approval callbacks are untouched.
 *
 * @param[in] a      Agent to update.
 * @param[in] client New model client; consumed on success.
 * @return AEGIS_OK on success, AEGIS_ERR_INVALID on NULL args, else the
 *   loop-creation error (agent and @p client unchanged).
 */
aegis_status_t aegis_coding_agent_set_model_client(aegis_coding_agent_t* a,
                                                   aegis_model_client_t* client)
{
    if (!a || !client) {
        return AEGIS_ERR_INVALID;
    }
    aegis_agent_loop_set_token(a->loop, NULL); /* unbind before rebuild */
    aegis_agent_loop_t*       replacement = NULL;
    aegis_agent_loop_config_t lcfg        = {
        .session       = a->session,
        .model         = client,
        .tools         = a->tools,
        .system_prompt = CODING_AGENT_SYSTEM_PROMPT,
        .on_event      = a->ev_fn,
        .event_user    = a->ev_user,
        .token         = a->token,
        .tool_approval = a->ap_fn,
        .approval_user = a->ap_user,
    };
    aegis_status_t st = aegis_agent_loop_create(&lcfg, &replacement);
    if (st != AEGIS_OK) {
        return st;
    }
    aegis_agent_loop_t*   old_loop   = a->loop;
    aegis_model_client_t* old_client = a->model;
    a->loop                          = replacement;
    a->model                         = client;
    aegis_agent_loop_destroy(old_loop);
    aegis_model_client_destroy(old_client);
    return AEGIS_OK;
}

/**
 * @brief Borrow the active model name.
 *
 * @param[in] a Agent, or NULL.
 * @return Model name string owned by the agent (do NOT free), or NULL when
 *   @p a is NULL.
 */
const char* aegis_coding_agent_model_name(const aegis_coding_agent_t* a)
{
    return a ? a->model_name : NULL;
}

/**
 * @brief Borrow the agent's tool registry.
 *
 * @param[in]  a   Agent, or NULL.
 * @param[out] out Receives the registry pointer (still owned by the agent;
 *   do NOT destroy). Untouched on failure.
 * @return AEGIS_OK on success, AEGIS_ERR_INVALID on NULL args or when no
 *   registry is attached.
 */
aegis_status_t aegis_coding_agent_tools(const aegis_coding_agent_t* a, aegis_tool_registry_t** out)
{
    if (!a || !out) {
        return AEGIS_ERR_INVALID;
    }
    *out = a->tools;
    return (*out) ? AEGIS_OK : AEGIS_ERR_INVALID;
}

/**
 * @brief Snapshot last-turn and cumulative token usage.
 *
 * @param[in]  a     Agent to query.
 * @param[out] last  Receives the most recent turn's usage.
 * @param[out] total Receives the cumulative usage across turns.
 * @return AEGIS_OK on success, AEGIS_ERR_INVALID on NULL args, else the
 *   loop-usage error (@p total then untouched).
 */
aegis_status_t aegis_coding_agent_usage(aegis_coding_agent_t* a, aegis_usage_t* last,
                                        aegis_usage_t* total)
{
    if (!a || !last || !total) {
        return AEGIS_ERR_INVALID;
    }
    aegis_status_t st = aegis_agent_loop_last_usage(a->loop, last);
    if (st != AEGIS_OK) {
        return st;
    }
    return aegis_agent_loop_usage(a->loop, total);
}

/**
 * @brief Register (or clear) the agent-loop event observer.
 *
 * Stored on the agent and forwarded to the loop; @p user is borrowed and
 * passed through to @p fn. NULL @p fn disables event emission.
 *
 * @param[in] a    Agent to configure.
 * @param[in] fn   Event callback, or NULL to disable.
 * @param[in] user Opaque pointer forwarded to @p fn (borrowed).
 * @return AEGIS_OK on success, AEGIS_ERR_INVALID on NULL agent.
 */
aegis_status_t aegis_coding_agent_set_event_callback(aegis_coding_agent_t* a,
                                                     aegis_agent_event_fn fn, void* user)
{
    if (!a) {
        return AEGIS_ERR_INVALID;
    }
    a->ev_fn   = fn;
    a->ev_user = user;
    return aegis_agent_loop_set_event_callback(a->loop, fn, user);
}

/**
 * @brief Install or clear the tool approval gate.
 *
 * Stored on the agent and forwarded to the loop; @p user is borrowed and
 * passed through to @p fn. NULL @p fn allows all tool calls.
 *
 * @param[in] a    Agent to configure.
 * @param[in] fn   Approval callback, or NULL to allow everything.
 * @param[in] user Opaque pointer forwarded to @p fn (borrowed).
 * @return AEGIS_OK on success, AEGIS_ERR_INVALID on NULL agent.
 */
aegis_status_t aegis_coding_agent_set_tool_approval(aegis_coding_agent_t*  a,
                                                    aegis_tool_approval_fn fn, void* user)
{
    if (!a) {
        return AEGIS_ERR_INVALID;
    }
    a->ap_fn   = fn;
    a->ap_user = user;
    return aegis_agent_loop_set_tool_approval(a->loop, fn, user);
}

/**
 * @brief Switch the model by name, rebuilding client and loop atomically.
 *
 * Creates the new client (OpenAI backend when the stored provider is
 * "llm-openai", Anthropic when "llm-anthropic", plain mock client otherwise)
 * and a fresh loop bound to it, then
 * swaps all three (loop, client, name) at once and destroys the old ones.
 * NOMEM after a successful build keeps everything old and discards the
 * replacement, so the agent is never left half-swapped. The session, tools
 * and callbacks are untouched.
 *
 * @param[in] a     Agent to update.
 * @param[in] model New model identifier (non-empty).
 * @return AEGIS_OK on success, AEGIS_ERR_INVALID on NULL/empty args, else
 *   the client/loop error (agent unchanged).
 */
aegis_status_t aegis_coding_agent_set_model(aegis_coding_agent_t* a, const char* model)
{
    if (!a || !model || model[0] == '\0') {
        return AEGIS_ERR_INVALID;
    }
    void*            new_ctx    = NULL;
    aegis_model_client_t* new_client = NULL;
    aegis_status_t   st;
#ifdef AEGIS_OPENAI_PROVIDER
    if (a->provider && strcmp(a->provider, "llm-openai") == 0) {
        st = build_openai_model(a, model, &new_ctx, &new_client);
    } else
#endif
#ifdef AEGIS_ANTHROPIC_PROVIDER
    if (a->provider && strcmp(a->provider, "llm-anthropic") == 0) {
        st = build_anthropic_model(a, model, &new_ctx, &new_client);
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
        .token         = a->token,
        .tool_approval = a->ap_fn,
        .approval_user = a->ap_user,
    };
    st = aegis_agent_loop_create(&lcfg, &replacement);
    if (st != AEGIS_OK) {
        aegis_model_client_destroy(new_client);
        if (new_ctx) {
            // Destroy via the provider-specific destroy fn captured on the agent
            // for the CURRENT provider (new_ctx was just built with that fn).
            void (*destroy_fn)(void*) = a->provider_destroy;
            if (destroy_fn) {
                destroy_fn(new_ctx);
            }
        }
        return st;
    }

    // Atomic swap: new loop/client in, old ones destroyed.
    aegis_agent_loop_t*   old_loop   = a->loop;
    aegis_model_client_t* old_client = a->model;
    char*                 old_name   = a->model_name;
    void*                 old_ctx    = a->provider_ctx;
    void (*old_destroy_fn)(void*)    = a->provider_destroy;
    char* new_name = strdup(model);
    if (!new_name) {
        // Out of memory: keep everything old, discard the replacement.
        aegis_agent_loop_destroy(replacement);
        aegis_model_client_destroy(new_client);
        if (new_ctx) {
            void (*destroy_fn)(void*) = a->provider_destroy;
            if (destroy_fn) {
                destroy_fn(new_ctx);
            }
        }
        return AEGIS_ERR_NOMEM;
    }
    a->loop       = replacement;
    a->model      = new_client;
    a->model_name = new_name;
    a->provider_ctx = new_ctx;
    // provider_destroy stays bound to the provider string (unchanged by set_model)
    aegis_agent_loop_destroy(old_loop);
    aegis_model_client_destroy(old_client);
    if (old_ctx) {
        if (old_destroy_fn) {
            old_destroy_fn(old_ctx);
        }
    }
    free(old_name);
    return AEGIS_OK;
}