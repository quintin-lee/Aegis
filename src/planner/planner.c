/**
 * @file planner.c
 * @brief LLM-backed goal -> plan front end with a strict response parser.
 *
 * The planner is deliberately dumb about intelligence: it formats one
 * prompt, dispatches it through the Provider Registry, and parses the
 * answer as a rigid line DSL. Anything the parser does not fully
 * understand fails the whole planning attempt - a partially understood
 * plan would be worse than no plan.
 */
#include "planner_internal.h"

#include "aegis/common/cancellation/cancellation.h"
#include "aegis/provider/llm.h"
#include "aegis/planner/planner.h"
#include "aegis/strategy/strategy.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PLANNER_MAX_TOKENS  1024L
#define PLANNER_TEMPERATURE 0.2

/* ── Lifecycle ─────────────────────────────────────────────────────────────── */
/* struct aegis_planner lives in planner_internal.h (shared with replanner). */

aegis_status_t aegis_planner_create(aegis_planner_t** out, const aegis_planner_config_t* cfg)
{
    if (!out || !cfg || !cfg->provider_registry || !cfg->llm_provider_name ||
        cfg->llm_provider_name[0] == '\0') {
        return AEGIS_ERR_INVALID;
    }
    aegis_planner_t* p = calloc(1, sizeof(*p));
    if (!p) {
        return AEGIS_ERR_NOMEM;
    }
    p->registry      = cfg->provider_registry;
    p->provider_name = strdup(cfg->llm_provider_name);
    if (!p->provider_name) {
        free(p);
        return AEGIS_ERR_NOMEM;
    }
    *out = p;
    return AEGIS_OK;
}

void aegis_planner_destroy(aegis_planner_t* planner)
{
    if (!planner) {
        return;
    }
    free(planner->provider_name);
    free(planner->strategy_name);
    free(planner);
}

/* ── Prompt ────────────────────────────────────────────────────────────────── */

static const char k_instructions[] =
    "You are a planning engine. Convert the goal below into an execution plan.\n"
    "Answer ONLY with lines in this exact format:\n"
    "STEP|<step_id>|<type>|<deps>|<name>|<description>\n"
    "- <step_id>: integer (use -1 for automatic assignment)\n"
    "- <type>: one of computational io network shell tool provision sync custom\n"
    "- <deps>: comma-separated step ids this step depends on, or empty\n"
    "- <name>: short identifier (required)\n"
    "- <description>: free text\n"
    "Lines starting with # are comments. Output nothing else.\n\n";

static aegis_status_t build_prompt(const char* body, const char* goal, char** out)
{
    return aegis_planner_compose_prompt(body, goal, out);
}

aegis_status_t aegis_planner_compose_prompt(const char* body, const char* goal, char** out)
{
    if (!body || !goal || !out) {
        return AEGIS_ERR_INVALID;
    }
    size_t need   = strlen(k_instructions) + strlen(body) + strlen(goal) + 4;
    char*  prompt = malloc(need);
    if (!prompt) {
        return AEGIS_ERR_NOMEM;
    }
    snprintf(prompt, need, "%s%s%s\n", k_instructions, body, goal);
    *out = prompt;
    return AEGIS_OK;
}

/* ── Strict DSL parser ─────────────────────────────────────────────────────── */

static const struct {
    const char*       word;
    aegis_task_type_t type;
} k_type_words[] = {
    {"computational", AEGIS_TASK_TYPE_COMPUTATIONAL},
    {"io", AEGIS_TASK_TYPE_IO},
    {"network", AEGIS_TASK_TYPE_NETWORK},
    {"shell", AEGIS_TASK_TYPE_SHELL},
    {"tool", AEGIS_TASK_TYPE_TOOL},
    {"provision", AEGIS_TASK_TYPE_PROVISION},
    {"sync", AEGIS_TASK_TYPE_SYNCHRONIZATION},
    {"custom", AEGIS_TASK_TYPE_CUSTOM},
};

static bool parse_type_word(const char* word, size_t len, aegis_task_type_t* out)
{
    for (size_t i = 0; i < sizeof(k_type_words) / sizeof(k_type_words[0]); i++) {
        if (strlen(k_type_words[i].word) == len && strncmp(k_type_words[i].word, word, len) == 0) {
            *out = k_type_words[i].type;
            return true;
        }
    }
    return false;
}

/** Trim ASCII whitespace in place; returns start pointer and adjusts length. */
static char* trim(char** s, size_t* len)
{
    while (*len > 0 && (**s == ' ' || **s == '\t')) {
        (*s)++;
        (*len)--;
    }
    while (*len > 0 &&
           ((*s)[*len - 1] == ' ' || (*s)[*len - 1] == '\t' || (*s)[*len - 1] == '\r')) {
        (*len)--;
    }
    (*s)[*len] = '\0';
    return *s;
}

/** Strict integer parse: entire field must be a valid integer. */
static bool parse_int(const char* s, size_t len, int64_t* out)
{
    if (len == 0) {
        return false;
    }
    char buf[32];
    if (len >= sizeof(buf)) {
        return false;
    }
    memcpy(buf, s, len);
    buf[len]      = '\0';
    char*     end = NULL;
    long long v   = strtoll(buf, &end, 10);
    if (!end || *end != '\0') {
        return false;
    }
    *out = (int64_t)v;
    return true;
}

static aegis_status_t parse_dep_list(char* field, size_t len, int64_t* deps, size_t* dep_count)
{
    trim(&field, &len);
    *dep_count = 0;
    if (len == 0) {
        return AEGIS_OK; /* No dependencies. */
    }
    size_t count = 0;
    char*  save  = NULL;
    for (char* tok = strtok_r(field, ",", &save); tok != NULL; tok = strtok_r(NULL, ",", &save)) {
        size_t tlen = strlen(tok);
        trim(&tok, &tlen);
        int64_t v;
        if (!parse_int(tok, tlen, &v)) {
            return AEGIS_ERR_INVALID;
        }
        if (count >= AEGIS_PLAN_MAX_DEPS) {
            return AEGIS_ERR_INVALID;
        }
        deps[count++] = v;
    }
    *dep_count = count;
    return AEGIS_OK;
}

/**
 * Parse one DSL line into fields. Layout:
 *   STEP|<id>|<type>|<deps>|<name>|<desc>
 * Exactly six '|' separated fields are required.
 */
static aegis_status_t parse_step_line(char* line, aegis_plan_t* plan)
{
    char*  fields[6];
    size_t flens[6];
    size_t nfield = 0;
    char*  cur    = line;
    for (;;) {
        char* pipe = strchr(cur, '|');
        if (pipe) {
            if (nfield >= 5) {
                return AEGIS_ERR_INVALID; /* Would overflow: desc may not contain '|'. */
            }
            size_t len     = (size_t)(pipe - cur);
            fields[nfield] = cur;
            flens[nfield]  = len;
            nfield++;
            *pipe = '\0';
            cur   = pipe + 1;
        } else {
            fields[nfield] = cur;
            flens[nfield]  = strlen(cur);
            nfield++;
            break;
        }
    }
    if (nfield != 6) {
        return AEGIS_ERR_INVALID;
    }

    /* Field 0: literal "STEP". */
    trim(&fields[0], &flens[0]);
    if (flens[0] != 4 || strncmp(fields[0], "STEP", 4) != 0) {
        return AEGIS_ERR_INVALID;
    }

    /* Field 1: step id. */
    int64_t id;
    if (!parse_int(trim(&fields[1], &flens[1]), flens[1], &id)) {
        return AEGIS_ERR_INVALID;
    }

    /* Field 2: type word. */
    aegis_task_type_t type;
    if (!parse_type_word(trim(&fields[2], &flens[2]), flens[2], &type)) {
        return AEGIS_ERR_INVALID;
    }

    /* Field 3: dependency list. */
    int64_t        deps[AEGIS_PLAN_MAX_DEPS];
    size_t         dep_count;
    aegis_status_t rc = parse_dep_list(fields[3], flens[3], deps, &dep_count);
    if (rc != AEGIS_OK) {
        return rc;
    }

    /* Field 4: name (required non-empty). */
    char* name = trim(&fields[4], &flens[4]);
    if (flens[4] == 0) {
        return AEGIS_ERR_INVALID;
    }

    /* Field 5: description (optional). */
    char* desc      = trim(&fields[5], &flens[5]);
    char* desc_copy = NULL;
    if (flens[5] > 0) {
        desc_copy = strdup(desc);
        if (!desc_copy) {
            return AEGIS_ERR_NOMEM;
        }
    }
    char* name_copy = strdup(name);
    if (!name_copy) {
        free(desc_copy);
        return AEGIS_ERR_NOMEM;
    }

    static const aegis_task_retry_policy_t no_retry = {0, 0, false};
    if (id == AEGIS_PLAN_STEP_ID_AUTO) {
        id = aegis_plan_next_free_id(plan);
    }
    rc = aegis_planner_add_step_owned(plan, id, name_copy, desc_copy, type, 0, 0, no_retry, NULL,
                                      NULL, 0, deps, dep_count);
    /* On failure add_step_owned consumed the heap strings. */
    return rc;
}

static aegis_status_t parse_response_into(const char* text, aegis_plan_t* plan)
{
    const char* cursor = text;
    bool        any    = false;
    while (*cursor) {
        const char* nl  = strchr(cursor, '\n');
        size_t      len = nl ? (size_t)(nl - cursor) : strlen(cursor);

        /* Work on a mutable line copy (trimming writes NULs). */
        char* line = malloc(len + 1);
        if (!line) {
            return AEGIS_ERR_NOMEM;
        }
        memcpy(line, cursor, len);
        line[len] = '\0';

        char*  s    = line;
        size_t slen = len;
        trim(&s, &slen);
        if (slen > 0 && s[0] != '#') {
            aegis_status_t rc = parse_step_line(s, plan);
            if (rc != AEGIS_OK) {
                free(line);
                return rc;
            }
            any = true;
        }
        free(line);
        if (!nl) {
            break;
        }
        cursor = nl + 1;
    }
    if (!any) {
        return AEGIS_ERR_INVALID; /* A plan needs at least one step. */
    }
    return aegis_plan_validate(plan);
}

/* ── Shared generate path (planner + replanner) ───────────────────────────── */

aegis_status_t aegis_planner_generate(const aegis_provider_registry_t* registry,
                                      const char* provider_name, const char* prompt,
                                      const char* goal, const aegis_cancellation_token_t* token,
                                      aegis_plan_t** out)
{
    if (!registry || !provider_name || !prompt || !goal || goal[0] == '\0' || !out) {
        return AEGIS_ERR_INVALID;
    }

    aegis_llm_request_t req = {0};
    req.prompt              = prompt;
    req.prompt_len          = strlen(prompt);
    req.max_tokens          = PLANNER_MAX_TOKENS;
    req.temperature         = PLANNER_TEMPERATURE;

    aegis_llm_response_t resp;
    memset(&resp, 0, sizeof(resp));
    aegis_status_t rc = aegis_llm_complete(registry, provider_name, &req, token, &resp);
    if (rc != AEGIS_OK) {
        return rc; /* Propagate verbatim (NOT_FOUND/PERM/CANCELLED/...). */
    }

    /* Response data is NOT NUL-terminated; take an owned copy. */
    char* text = NULL;
    if (resp.data && resp.len > 0) {
        text = malloc(resp.len + 1);
        if (!text) {
            aegis_llm_response_destroy(&resp);
            return AEGIS_ERR_NOMEM;
        }
        memcpy(text, resp.data, resp.len);
        text[resp.len] = '\0';
    }
    aegis_llm_response_destroy(&resp);

    if (!text) {
        return AEGIS_ERR_INVALID; /* Empty model output can never be a plan. */
    }

    aegis_plan_t* plan = NULL;
    rc                 = aegis_plan_create(&plan, goal);
    if (rc != AEGIS_OK) {
        free(text);
        return rc;
    }

    rc = parse_response_into(text, plan);
    free(text);
    if (rc != AEGIS_OK) {
        aegis_plan_destroy(plan);
        return rc;
    }

    *out = plan;
    return AEGIS_OK;
}

/* ── Strategy binding ──────────────────────────────────────────────────────── */

aegis_status_t aegis_planner_attach_strategies(aegis_planner_t*                 planner,
                                               const aegis_strategy_registry_t* strategies)
{
    if (!planner || !strategies) {
        return AEGIS_ERR_INVALID;
    }
    planner->strategies = strategies; /* Borrowed. */
    return AEGIS_OK;
}

aegis_status_t aegis_planner_use_strategy(aegis_planner_t* planner, const char* name)
{
    if (!planner) {
        return AEGIS_ERR_INVALID;
    }
    /* Single-threaded builder semantics: plain swap is safe. */
    if (name && name[0] != '\0' && !planner->strategies) {
        return AEGIS_ERR_INVALID; /* Nothing attached to resolve against. */
    }
    char* copy = NULL;
    if (name && name[0] != '\0') {
        copy = strdup(name);
        if (!copy) {
            return AEGIS_ERR_NOMEM;
        }
    }
    free(planner->strategy_name);
    planner->strategy_name = copy; /* NULL clears the binding -> built-in path. */
    return AEGIS_OK;
}

/* Dispatch through the bound strategy, if any. Returns
 * AEGIS_ERR_NOT_FOUND when a strategy is selected but not registered,
 * AEGIS_ERR_INVALID when selected without an attached registry.
 * Returns false when no strategy is bound (caller uses built-in path). */
static bool dispatch_via_strategy(const aegis_planner_t* planner, const char* goal,
                                  const aegis_plan_t* previous_plan, const char* feedback,
                                  const aegis_cancellation_token_t* token, aegis_plan_t** out,
                                  aegis_status_t* rc)
{
    if (!planner->strategy_name) {
        return false;
    }
    if (!planner->strategies) {
        *rc = AEGIS_ERR_INVALID; /* Strategy chosen but nothing to resolve it with. */
        return true;
    }
    aegis_strategy_view_t view;
    *rc = aegis_strategy_find(planner->strategies, planner->strategy_name, &view);
    if (*rc != AEGIS_OK) {
        return true; /* NOT_FOUND propagates verbatim. */
    }
    aegis_strategy_input_t input = {goal, previous_plan, feedback};
    *rc                          = view.def.plan(view.def.user, &input, token, out);
    return true;
}

/* ── Public plan() ─────────────────────────────────────────────────────────── */

aegis_status_t aegis_planner_plan(const aegis_planner_t* planner, const char* goal,
                                  const aegis_cancellation_token_t* token, aegis_plan_t** out)
{
    if (!planner || !goal || goal[0] == '\0' || !out) {
        return AEGIS_ERR_INVALID;
    }

    aegis_status_t rc = AEGIS_OK;
    if (dispatch_via_strategy(planner, goal, NULL, NULL, token, out, &rc)) {
        return rc;
    }

    char* prompt = NULL;
    rc           = build_prompt("GOAL:\n", goal, &prompt);
    if (rc != AEGIS_OK) {
        return rc;
    }
    rc =
        aegis_planner_generate(planner->registry, planner->provider_name, prompt, goal, token, out);
    free(prompt);
    return rc;
}
