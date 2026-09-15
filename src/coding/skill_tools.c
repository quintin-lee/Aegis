/**
 * @file skill_tools.c
 * @brief The "use_skill" tool + skill-aware system-prompt builder.
 *
 * Mirrors the delegate_tools.c pattern: a heap-owned aegis_tool_def_t whose
 * .user is a skill_tools_ctx_t blob holding the borrowed skill registry. The
 * tool loads a named skill's full instructions on demand (progressive
 * disclosure); build_coding_system_prompt injects cheap metadata (name +
 * description) into the coding agent's system prompt.
 */
#define _POSIX_C_SOURCE 200809L
#include "aegis/coding/skill_tools.h"
#include "aegis/common/error.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SKILL_TOOL_NAME "use_skill"

/* Base prompt must stay in sync with CODING_AGENT_SYSTEM_PROMPT in
 * coding_agent.c. Replicated here so the builder is self-contained. */
#define CODING_SYSTEM_PROMPT_BASE "You are a coding agent. Use tools to help the user."

static aegis_status_t use_skill_tool_execute(void* user, const aegis_tool_args_t* args,
                                             const aegis_cancellation_token_t* token,
                                             aegis_tool_result_t* out)
{
    (void)token;
    skill_tools_ctx_t* ctx = user;
    if (!ctx || !ctx->skills || !args || !out) {
        return AEGIS_ERR_INVALID;
    }
    const aegis_tool_value_t* nv = NULL;
    if (!aegis_tool_args_find(args, "name", &nv) || !nv ||
        nv->type != AEGIS_TOOL_VAL_STRING || !nv->as.str.ptr) {
        return AEGIS_ERR_INVALID;
    }
    const aegis_skill_t* sk = NULL;
    aegis_status_t st = aegis_skill_registry_find(ctx->skills, nv->as.str.ptr, &sk);
    if (st != AEGIS_OK) {
        return st; /* AEGIS_ERR_NOT_FOUND for an unknown skill */
    }
    return aegis_tool_result_set_string(out, sk->instructions ? sk->instructions : "");
}

aegis_status_t aegis_coding_skill_tools_register(aegis_tool_registry_t* reg,
                                                 skill_tools_ctx_t*     ctx,
                                                 aegis_tool_def_t**     out_def)
{
    if (!reg || !out_def) {
        return AEGIS_ERR_INVALID;
    }
    *out_def = NULL;

    char* name = strdup(SKILL_TOOL_NAME);
    char* desc = strdup("Load the full instructions for a named skill. "
                        "Returns the skill's instruction text, or NOT_FOUND "
                        "if the skill does not exist.");
    if (!name || !desc) {
        free(name);
        free(desc);
        return AEGIS_ERR_NOMEM;
    }
    aegis_tool_param_spec_t* params = calloc(1, sizeof(*params));
    if (!params) {
        free(name);
        free(desc);
        return AEGIS_ERR_NOMEM;
    }
    char* p_name = strdup("name");
    char* p_desc = strdup("The skill to load.");
    if (!p_name || !p_desc) {
        free(p_name);
        free(p_desc);
        free(params);
        free(name);
        free(desc);
        return AEGIS_ERR_NOMEM;
    }
    params[0] = (aegis_tool_param_spec_t){
        .name        = p_name,
        .type        = AEGIS_TOOL_VAL_STRING,
        .required    = true,
        .description = p_desc,
    };

    aegis_tool_def_t* def = calloc(1, sizeof(*def));
    if (!def) {
        free(p_name);
        free(p_desc);
        free(params);
        free(name);
        free(desc);
        return AEGIS_ERR_NOMEM;
    }
    def->name            = name;
    def->description     = desc;
    def->schema.params   = params;
    def->schema.param_count = 1;
    def->execute         = use_skill_tool_execute;
    def->user            = ctx;

    aegis_status_t st = aegis_tool_registry_register(reg, def);
    if (st != AEGIS_OK) {
        aegis_coding_skill_tools_free(def, NULL);
        return st;
    }
    *out_def = def;
    return AEGIS_OK;
}

void aegis_coding_skill_tools_free(aegis_tool_def_t* def, skill_tools_ctx_t* ctx)
{
    if (def) {
        if (def->schema.params) {
            for (size_t i = 0; i < def->schema.param_count; ++i) {
                free((void*)def->schema.params[i].name);
                free((void*)def->schema.params[i].description);
            }
            free((void*)def->schema.params);
        }
        free((void*)def->name);
        free((void*)def->description);
        free(def);
    }
    free(ctx);
}

aegis_status_t build_coding_system_prompt(aegis_skill_registry_t* skills, char** out)
{
    if (!out) {
        return AEGIS_ERR_INVALID;
    }
    *out = NULL;

    size_t count = skills ? aegis_skill_registry_count(skills) : 0;
    if (count == 0) {
        *out = strdup(CODING_SYSTEM_PROMPT_BASE);
        return *out ? AEGIS_OK : AEGIS_ERR_NOMEM;
    }

    /* Assemble: base prompt, then the disclosure header, then one line per
     * skill. Grow the buffer as needed. */
    const char* header =
        "\nAvailable skills (load full instructions with the \"use_skill\" tool):";
    size_t cap = 512 + strlen(header);
    for (size_t i = 0; i < count; ++i) {
        const aegis_skill_t* sk = aegis_skill_registry_get(skills, i);
        if (!sk || !sk->name) {
            continue;
        }
        cap += 64 + strlen(sk->name) + (sk->description ? strlen(sk->description) : 0);
    }
    char* buf = malloc(cap);
    if (!buf) {
        return AEGIS_ERR_NOMEM;
    }
    size_t len = 0;
    int w = snprintf(buf + len, cap - len, "%s", CODING_SYSTEM_PROMPT_BASE);
    if (w < 0 || (size_t)w >= cap - len) {
        free(buf);
        return AEGIS_ERR_NOMEM;
    }
    len += (size_t)w;
    size_t hd = strlen(header);
    if (len + hd + 1 > cap) {
        free(buf);
        return AEGIS_ERR_NOMEM;
    }
    memcpy(buf + len, header, hd + 1);
    len += hd + 1;

    for (size_t i = 0; i < count; ++i) {
        const aegis_skill_t* sk = aegis_skill_registry_get(skills, i);
        if (!sk || !sk->name) {
            continue;
        }
        const char* desc = sk->description ? sk->description : "";
        char  line[1024];
        int   lw = snprintf(line, sizeof(line), "\n- %s: %s", sk->name, desc);
        if (lw <= 0) {
            lw = (int)snprintf(line, sizeof(line), "\n- %s", sk->name);
        }
        if ((size_t)lw >= sizeof(line) - 1) {
            continue; /* drop an overlong line rather than overflow */
        }
        if (len + (size_t)lw + 1 > cap) {
            cap = len + (size_t)lw + 1;
            char* nb = realloc(buf, cap);
            if (!nb) {
                free(buf);
                return AEGIS_ERR_NOMEM;
            }
            buf = nb;
        }
        memcpy(buf + len, line, (size_t)lw + 1);
        len += (size_t)lw + 1;
    }

    *out = buf;
    return AEGIS_OK;
}
