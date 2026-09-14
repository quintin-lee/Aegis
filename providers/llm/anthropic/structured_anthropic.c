/**
 * @file structured_anthropic.c
 * @brief Anthropic (Claude) structured/streaming model backend (libcurl).
 *
 * Speaks the Anthropic Messages API (POST {base}/v1/messages). Request body:
 * system is a top-level parameter (multiple system-role messages are joined
 * into an array of text blocks), tool results ride in user messages as
 * tool_result content blocks, assistant tool calls are tool_use content
 * blocks, and tools use the flat input_schema shape. When req->thinking_budget
 * is set, a thinking block is enabled (budget must be < max_tokens).
 *
 * Two paths share the aegis_llm_shared transport: SSE streaming reassembled
 * into model stream events and one-shot complete-response parsing. Cancellation
 * is cooperative via the curl progress callback and always wins over provider
 * errors. HTTP 429 maps to MODEL_RATE_LIMIT, 413 to CONTEXT_OVERFLOW; anything
 * else non-2xx is a generic provider error.
 */
#define _POSIX_C_SOURCE 200809L
#include "structured_anthropic.h"
#ifdef AEGIS_ANTHROPIC_TEST_API
#include "structured_anthropic_test.h"
#endif
#include "aegis/message/message.h"
#include "aegis/message/role.h"
#include "aegis/message/tool_call.h"
#include "aegis/tool/tool.h"
#include "llm_shared.h"
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define ANTHROPIC_DEFAULT_URL   "https://api.anthropic.com"
#define ANTHROPIC_DEFAULT_MODEL "claude-sonnet-4-5"
#define ANTHROPIC_API_VERSION   "2023-06-01"
#define ANTHROPIC_MAX_RESPONSE  (16u * 1024u * 1024u)
#define ANTHROPIC_MAX_THINKING  (32u * 1024u)

typedef struct aegis_anthropic_model_ctx {
    char* api_key;
    char* base_url;
    char* model;
} aegis_anthropic_model_ctx_t;

/* Per-block type of the content block currently being streamed. */
typedef enum {
    BLOCK_NONE = 0,
    BLOCK_TEXT,
    BLOCK_THINKING,
    BLOCK_TOOL_USE
} block_kind_t;

typedef struct {
    aegis_anthropic_model_ctx_t*    ctx;
    const aegis_model_request_t*    req;
    const aegis_cancellation_token_t* token;
    aegis_model_stream_callback_fn  callback;
    void*                           callback_user;
    aegis_sse_state_t               sse;
    uint32_t                        input_tokens;
    uint32_t                        output_tokens;
    bool                            saw_stop;
    block_kind_t                    block_type[16];
    bool                            block_started[16];
    char*                           tool_args[16];
    size_t                          tool_args_len[16];
    size_t                          tool_args_cap[16];
    char                            tool_id[16][128];
    char                            tool_name[16][256];
} anthro_state_t;

/* ── JSON body helpers (Anthropic wire shape) ────────────────────────── */

static int append_system_param(aegis_json_builder_t* b, const aegis_message_list_t* msgs)
{
    /* Collect system-role messages; join into a top-level "system" array of
     * {type:"text",text} blocks. */
    int  first = 1;
    int  count = 0;
    for (size_t i = 0; i < aegis_message_list_count(msgs); ++i) {
        const aegis_message_t* m = aegis_message_list_at(msgs, i);
        if (aegis_message_role(m) != AEGIS_MESSAGE_SYSTEM) {
            continue;
        }
        const char* text = aegis_message_content(m);
        if (!text) {
            continue;
        }
        if (first) {
            if (!aegis_json_append_raw(b, "\"system\":[", strlen("\"system\":["))) {
                return 0;
            }
            first = 0;
        } else {
            if (!aegis_json_append_raw(b, ",", 1)) {
                return 0;
            }
        }
        if (!aegis_json_append_raw(b, "{\"type\":\"text\",\"text\":", strlen("{\"type\":\"text\",\"text\":")) ||
            !aegis_json_append_string(b, text) || !aegis_json_append_raw(b, "}", 1)) {
            return 0;
        }
        ++count;
    }
    if (first) {
        return 1; /* no system messages; nothing appended */
    }
    return aegis_json_append_raw(b, "]", 1) && (count > 0);
}

static int append_content_block_text(aegis_json_builder_t* b, const char* text)
{
    if (!aegis_json_append_raw(b, "{\"type\":\"text\",\"text\":",
                               strlen("{\"type\":\"text\",\"text\":")) ||
        !aegis_json_append_string(b, text) || !aegis_json_append_raw(b, "}", 1)) {
        return 0;
    }
    return 1;
}

static int append_message_blocks(aegis_json_builder_t* b, const aegis_message_t* m)
{
    const char* role = aegis_message_role_str(aegis_message_role(m));
    aegis_json_append_raw(b, "{\"role\":", strlen("{\"role\":"));
    if (!aegis_json_append_string(b, role)) {
        return 0;
    }

    switch (aegis_message_role(m)) {
    case AEGIS_MESSAGE_TOOL: {
        /* tool_result content block inside a user message */
        if (!aegis_json_append_raw(b, ",\"content\":[{\"type\":\"tool_result\",\"tool_use_id\":",
                                   strlen(",\"content\":[{\"type\":\"tool_result\",\"tool_use_id\":"))) {
            return 0;
        }
        const char* id = aegis_message_tool_call_id(m);
        if (!aegis_json_append_string(b, id ? id : "")) {
            return 0;
        }
        const char* content = aegis_message_content(m);
        if (!aegis_json_append_raw(b, ",\"content\":", strlen(",\"content\":")) ||
            !aegis_json_append_string(b, content ? content : "")) {
            return 0;
        }
        return aegis_json_append_raw(b, "}]", strlen("}]"));
    }
    case AEGIS_MESSAGE_ASSISTANT: {
        const char* content = aegis_message_content(m);
        size_t      tc     = aegis_message_tool_call_count(m);
        if (!content && tc == 0) {
            return aegis_json_append_raw(b, ",\"content\":[]", strlen(",\"content\":[]"));
        }
        if (!aegis_json_append_raw(b, ",\"content\":[", strlen(",\"content\":["))) {
            return 0;
        }
        int  first = 1;
        if (content) {
            if (!append_content_block_text(b, content)) {
                return 0;
            }
            first = 0;
        }
        for (size_t i = 0; i < tc; ++i) {
            const aegis_tool_call_t* c = aegis_message_tool_call_at(m, i);
            if (!first && !aegis_json_append_raw(b, ",", 1)) {
                return 0;
            }
            first = 0;
            if (!aegis_json_append_raw(b, "{\"type\":\"tool_use\",\"id\":",
                                       strlen("{\"type\":\"tool_use\",\"id\":")) ||
                !aegis_json_append_string(b, aegis_tool_call_id(c)) ||
                !aegis_json_append_raw(b, ",\"name\":", strlen(",\"name\":")) ||
                !aegis_json_append_string(b, aegis_tool_call_name(c)) ||
                !aegis_json_append_raw(b, ",\"input\":", strlen(",\"input\":"))) {
                return 0;
            }
            /* input is a JSON object in Anthropic, not a string. */
            const char* args = aegis_tool_call_arguments(c);
            if (args && *args) {
                if (!aegis_json_append_raw(b, args, strlen(args))) {
                    return 0;
                }
            } else {
                if (!aegis_json_append_raw(b, "{}", 2)) {
                    return 0;
                }
            }
            if (!aegis_json_append_raw(b, "}", 1)) {
                return 0;
            }
        }
        return aegis_json_append_raw(b, "]", 1);
    }
    case AEGIS_MESSAGE_USER:
    default: {
        const char* content = aegis_message_content(m);
        if (content) {
            if (!aegis_json_append_raw(b, ",\"content\":", strlen(",\"content\":"))) {
                return 0;
            }
            /* plain user content can stay a string */
            return aegis_json_append_string(b, content);
        }
        return aegis_json_append_raw(b, ",\"content\":null", strlen(",\"content\":null"));
    }
    }
}

typedef struct {
    aegis_json_builder_t* buffer;
    aegis_status_t        status;
    bool                 first;
} tool_schema_context_t;

static aegis_status_t append_tool_schema(const aegis_tool_def_t* def, void* user)
{
    tool_schema_context_t* ctx = user;
    if (!def || !ctx || ctx->status != AEGIS_OK) {
        return AEGIS_ERR_INVALID;
    }
    aegis_json_builder_t* b = ctx->buffer;
    if (!ctx->first && !aegis_json_append_raw(b, ",", 1)) {
        ctx->status = AEGIS_ERR_NOMEM;
        return ctx->status;
    }
    ctx->first = false;
    if (!aegis_json_append_raw(b, "{\"name\":", strlen("{\"name\":")) ||
        !aegis_json_append_string(b, def->name) ||
        !aegis_json_append_raw(b, ",\"description\":", strlen(",\"description\":")) ||
        !aegis_json_append_string(b, def->description ? def->description : "") ||
        !aegis_json_append_raw(b, ",\"input_schema\":{\"type\":\"object\",\"properties\":{",
                                strlen(",\"input_schema\":{\"type\":\"object\",\"properties\":{"))) {
        ctx->status = AEGIS_ERR_NOMEM;
        return ctx->status;
    }
    for (size_t i = 0; i < def->schema.param_count; ++i) {
        const aegis_tool_param_spec_t* p = &def->schema.params[i];
        if (i && !aegis_json_append_raw(b, ",", 1)) {
            ctx->status = AEGIS_ERR_NOMEM;
            return ctx->status;
        }
        if (!aegis_json_append_string(b, p->name) ||
            !aegis_json_append_raw(b, ":{\"type\":", strlen(":{\"type\":"))) {
            ctx->status = AEGIS_ERR_NOMEM;
            return ctx->status;
        }
        const char* type = p->type == AEGIS_TOOL_VAL_BOOL    ? "boolean"
                           : p->type == AEGIS_TOOL_VAL_INT   ? "integer"
                           : p->type == AEGIS_TOOL_VAL_FLOAT ? "number"
                                                              : "string";
        if (!aegis_json_append_string(b, type)) {
            ctx->status = AEGIS_ERR_NOMEM;
            return ctx->status;
        }
        if (p->description &&
            (!aegis_json_append_raw(b, ",\"description\":", strlen(",\"description\":")) ||
             !aegis_json_append_string(b, p->description))) {
            ctx->status = AEGIS_ERR_NOMEM;
            return ctx->status;
        }
        if (!aegis_json_append_raw(b, "}", 1)) {
            ctx->status = AEGIS_ERR_NOMEM;
            return ctx->status;
        }
    }
    if (!aegis_json_append_raw(b, "},\"required\":[", strlen("},\"required\":["))) {
        ctx->status = AEGIS_ERR_NOMEM;
        return ctx->status;
    }
    bool first = true;
    for (size_t i = 0; i < def->schema.param_count; ++i) {
        const aegis_tool_param_spec_t* p = &def->schema.params[i];
        if (!p->required) {
            continue;
        }
        if (!first && !aegis_json_append_raw(b, ",", 1)) {
            ctx->status = AEGIS_ERR_NOMEM;
            return ctx->status;
        }
        first = false;
        if (!aegis_json_append_string(b, p->name)) {
            ctx->status = AEGIS_ERR_NOMEM;
            return ctx->status;
        }
    }
    if (!aegis_json_append_raw(b, "]}}", strlen("]}]"))) {
        ctx->status = AEGIS_ERR_NOMEM;
        return ctx->status;
    }
    return ctx->status;
}

static char* build_body(const aegis_model_request_t* req, const char* fallback_model)
{
    const char* model_name = req->model && *req->model ? req->model : fallback_model;
    aegis_json_builder_t b;
    aegis_json_builder_init(&b);
    if (!aegis_json_append_raw(&b, "{\"model\":", strlen("{\"model\":")) ||
        !aegis_json_append_string(&b, model_name) ||
        !aegis_json_append_raw(&b, ",\"max_tokens\":", strlen(",\"max_tokens\":"))) {
        goto fail;
    }
    char mt[16];
    int  k = snprintf(mt, sizeof(mt), "%u", req->max_tokens ? req->max_tokens : 4096u);
    if (k < 0 || !aegis_json_append_raw(&b, mt, (size_t)k)) {
        goto fail;
    }
    if (!append_system_param(&b, req->messages)) {
        goto fail;
    }
    /* system was appended with its own comma; start messages array. */
    if (!aegis_json_append_raw(&b, ",\"messages\":[", strlen(",\"messages\":["))) {
        goto fail;
    }
    size_t n = req->messages ? aegis_message_list_count(req->messages) : 0;
    for (size_t i = 0; i < n; ++i) {
        /* skip system-role messages here; they are in the top-level param */
        if (aegis_message_role(aegis_message_list_at(req->messages, i)) == AEGIS_MESSAGE_SYSTEM) {
            continue;
        }
        if (i && !aegis_json_append_raw(&b, ",", 1)) {
            goto fail;
        }
        if (!append_message_blocks(&b, aegis_message_list_at(req->messages, i))) {
            goto fail;
        }
    }
    if (!aegis_json_append_raw(&b, "]", 1)) {
        goto fail;
    }
    if (req->tools && aegis_tool_registry_count(req->tools) > 0) {
        if (!aegis_json_append_raw(&b, ",\"tools\":[", strlen(",\"tools\":["))) {
            goto fail;
        }
        tool_schema_context_t ctx = {.buffer = &b, .status = AEGIS_OK, .first = true};
        if (aegis_tool_registry_visit(req->tools, append_tool_schema, &ctx) != AEGIS_OK ||
            ctx.status != AEGIS_OK) {
            goto fail;
        }
        if (!aegis_json_append_raw(&b, "]", 1)) {
            goto fail;
        }
    }
    if (req->stream && !aegis_json_append_raw(&b, ",\"stream\":true", strlen(",\"stream\":true"))) {
        goto fail;
    }
    if (req->thinking_budget &&
        !aegis_json_append_raw(&b, ",\"thinking\":{\"type\":\"enabled\",\"budget_tokens\":",
                               strlen(",\"thinking\":{\"type\":\"enabled\",\"budget_tokens\":"))) {
        goto fail;
    }
    if (req->thinking_budget) {
        char tb[16];
        int  t = snprintf(tb, sizeof(tb), "%u}", req->thinking_budget);
        if (t < 0 || !aegis_json_append_raw(&b, tb, (size_t)t)) {
            goto fail;
        }
    }
    if (!aegis_json_append_raw(&b, "}", 1)) {
        goto fail;
    }
    return b.data;
fail:
    aegis_json_builder_free(&b);
    return NULL;
}

/* ── JSON value extraction (shared response parsing) ─────────────────── */

static const char* json_string_after(const char* json, const char* key)
{
    const char* p = strstr(json, key);
    if (!p) {
        return NULL;
    }
    p = strchr(p + strlen(key), ':');
    if (!p) {
        return NULL;
    }
    ++p;
    while (*p == ' ' || *p == '\t') {
        ++p;
    }
    if (*p != '"') {
        return NULL;
    }
    return p + 1;
}

static size_t copy_json_string(const char* p, char* out, size_t cap)
{
    if (!p || !out || cap == 0) {
        return 0;
    }
    size_t n = 0;
    while (*p && *p != '"') {
        if (*p == '\\' && p[1]) {
            ++p;
            if (*p == 'n') {
                out[n++] = '\n';
            } else if (*p == 'r') {
                out[n++] = '\r';
            } else if (*p == 't') {
                out[n++] = '\t';
            } else {
                out[n++] = *p;
            }
            ++p;
        } else {
            out[n++] = *p++;
        }
        if (n + 1 >= cap) {
            return 0;
        }
    }
    if (*p != '"') {
        return 0;
    }
    out[n] = '\0';
    return n;
}

static int json_uint_after(const char* json, const char* key, uint32_t* out)
{
    const char* p = strstr(json, key);
    if (!p) {
        return 0;
    }
    p = strchr(p + strlen(key), ':');
    if (!p) {
        return 0;
    }
    ++p;
    while (*p == ' ' || *p == '\t') {
        ++p;
    }
    char*         end   = NULL;
    unsigned long value = strtoul(p, &end, 10);
    if (end == p || value > UINT32_MAX) {
        return 0;
    }
    *out = (uint32_t)value;
    return 1;
}

/* ── SSE event interpretation ─────────────────────────────────────────── */

static int append_tool_args(anthro_state_t* s, uint32_t index, const char* chunk, size_t len)
{
    if (index >= 16) {
        return 0;
    }
    if (s->tool_args_len[index] + len + 1 > s->tool_args_cap[index]) {
        size_t cap = s->tool_args_cap[index] ? s->tool_args_cap[index] * 2 : 256;
        while (cap < s->tool_args_len[index] + len + 1) {
            cap *= 2;
        }
        char* p = realloc(s->tool_args[index], cap);
        if (!p) {
            return 0;
        }
        s->tool_args[index] = p;
        s->tool_args_cap[index] = cap;
    }
    memcpy(s->tool_args[index] + s->tool_args_len[index], chunk, len);
    s->tool_args[index][s->tool_args_len[index] + len] = '\0';
    s->tool_args_len[index] += len;
    return 1;
}

static int emit_event(anthro_state_t* s, const char* record, size_t len)
{
    /* record is the data: payload (JSON), already stripped of "data:" */
    char* json = malloc(len + 1);
    if (!json) {
        return 0;
    }
    memcpy(json, record, len);
    json[len] = '\0';

    const char* type = json_string_after(json, "\"type\"");
    if (!type) {
        free(json);
        return 1;
    }
    char ev_type[64] = {0};
    size_t ev_len     = copy_json_string(type, ev_type, sizeof(ev_type));
    if (!ev_len) {
        free(json);
        return 1;
    }

    if (strcmp(ev_type, "message_start") == 0) {
        json_uint_after(json, "\"input_tokens\"", &s->input_tokens);
        free(json);
        return 1;
    }
    if (strcmp(ev_type, "content_block_start") == 0) {
        uint32_t index = 0;
        json_uint_after(json, "\"index\"", &index);
        if (index < 16) {
            s->block_started[index] = true;
            s->block_type[index]    = BLOCK_NONE;
            const char* cb_type      = json_string_after(json, "\"type\"");
            char        cb[64] = {0};
            if (cb_type && copy_json_string(cb_type, cb, sizeof(cb))) {
                if (strcmp(cb, "text") == 0) {
                    s->block_type[index] = BLOCK_TEXT;
                } else if (strcmp(cb, "thinking") == 0) {
                    s->block_type[index] = BLOCK_THINKING;
                } else if (strcmp(cb, "tool_use") == 0) {
                    s->block_type[index] = BLOCK_TOOL_USE;
                    /* capture id + name */
                    const char* id   = json_string_after(json, "\"id\"");
                    const char* name = json_string_after(json, "\"name\"");
                    if (id) {
                        copy_json_string(id, s->tool_id[index], sizeof(s->tool_id[index]));
                    }
                    if (name) {
                        copy_json_string(name, s->tool_name[index], sizeof(s->tool_name[index]));
                    }
                }
            }
        }
        free(json);
        return 1;
    }
    if (strcmp(ev_type, "content_block_delta") == 0) {
        uint32_t index = 0;
        json_uint_after(json, "\"index\"", &index);
        if (index < 16) {
            const char* delta_type = json_string_after(json, "\"type\"");
            char        dt[64] = {0};
            if (delta_type && copy_json_string(delta_type, dt, sizeof(dt))) {
                if (strcmp(dt, "text_delta") == 0 && s->block_type[index] == BLOCK_TEXT) {
                    const char* text = json_string_after(json, "\"text\"");
                    if (text) {
                        char* decoded = malloc(len + 1);
                        if (!decoded) {
                            free(json);
                            return 0;
                        }
                        size_t n = copy_json_string(text, decoded, len + 1);
                        aegis_model_stream_event_t ev = {
                            .type = AEGIS_MODEL_STREAM_TEXT_DELTA, .data = decoded, .len = n};
                        aegis_status_t rc = s->callback(&ev, s->callback_user);
                        free(decoded);
                        if (rc != AEGIS_OK) {
                            free(json);
                            return 0;
                        }
                    }
                } else if (strcmp(dt, "thinking_delta") == 0 &&
                           s->block_type[index] == BLOCK_THINKING) {
                    const char* thinking = json_string_after(json, "\"thinking\"");
                    if (thinking) {
                        char* decoded = malloc(len + 1);
                        if (!decoded) {
                            free(json);
                            return 0;
                        }
                        size_t n = copy_json_string(thinking, decoded, len + 1);
                        aegis_model_stream_event_t ev = {
                            .type = AEGIS_MODEL_STREAM_REASONING_DELTA, .data = decoded, .len = n};
                        aegis_status_t rc = s->callback(&ev, s->callback_user);
                        free(decoded);
                        if (rc != AEGIS_OK) {
                            free(json);
                            return 0;
                        }
                    }
                } else if (strcmp(dt, "input_json_delta") == 0 &&
                           s->block_type[index] == BLOCK_TOOL_USE) {
                    const char* partial = json_string_after(json, "\"partial_json\"");
                    if (partial) {
                        /* copy the raw (unescaped) JSON substring */
                        size_t n = copy_json_string(partial, (char*)s->tool_name[index], 0);
                        (void)n;
                        char* chunk = malloc(len + 1);
                        if (!chunk) {
                            free(json);
                            return 0;
                        }
                        size_t cn = copy_json_string(partial, chunk, len + 1);
                        if (!append_tool_args(s, index, chunk, cn)) {
                            free(chunk);
                            free(json);
                            return 0;
                        }
                        free(chunk);
                    }
                }
            }
        }
        free(json);
        return 1;
    }
    if (strcmp(ev_type, "content_block_stop") == 0) {
        uint32_t index = 0;
        json_uint_after(json, "\"index\"", &index);
        if (index < 16 && s->block_type[index] == BLOCK_TOOL_USE) {
            /* emit accumulated tool args as a single DELTA */
            char* args = s->tool_args[index];
            if (args) {
                aegis_model_stream_event_t delta = {.type      = AEGIS_MODEL_STREAM_TOOL_CALL_DELTA,
                                                    .data      = args,
                                                    .len       = s->tool_args_len[index],
                                                    .index     = index,
                                                    .tool_name = s->tool_name[index],
                                                    .call_id   = s->tool_id[index]};
                aegis_status_t rc = s->callback(&delta, s->callback_user);
                if (rc != AEGIS_OK) {
                    free(json);
                    return 0;
                }
            }
            /* END the tool call */
            aegis_model_stream_event_t end = {.type = AEGIS_MODEL_STREAM_TOOL_CALL_END, .index = index};
            if (s->callback(&end, s->callback_user) != AEGIS_OK) {
                free(json);
                return 0;
            }
        }
        free(json);
        return 1;
    }
    if (strcmp(ev_type, "message_delta") == 0) {
        json_uint_after(json, "\"output_tokens\"", &s->output_tokens);
        free(json);
        return 1;
    }
    if (strcmp(ev_type, "message_stop") == 0) {
        aegis_usage_t u = {.input_tokens = s->input_tokens, .output_tokens = s->output_tokens};
        u.total_tokens   = u.input_tokens + u.output_tokens;
        aegis_model_stream_event_t usage = {
            .type = AEGIS_MODEL_STREAM_USAGE, .data = &u, .len = sizeof(u)};
        aegis_status_t rc = s->callback(&usage, s->callback_user);
        if (rc != AEGIS_OK) {
            free(json);
            return 0;
        }
        aegis_model_stream_event_t end = {.type = AEGIS_MODEL_STREAM_END};
        if (s->callback(&end, s->callback_user) != AEGIS_OK) {
            free(json);
            return 0;
        }
        s->saw_stop = true;
        free(json);
        return 1;
    }
    if (strcmp(ev_type, "error") == 0) {
        aegis_model_stream_event_t ev = {.type = AEGIS_MODEL_STREAM_ERROR, .data = json, .len = len};
        aegis_status_t              rc = s->callback(&ev, s->callback_user);
        free(json);
        return rc == AEGIS_OK;
    }
    /* ping / unknown: ignore */
    free(json);
    return 1;
}

/* Anthropic SSE frames look like:
 * event: <name>\n
 * data: <json>\n
 * \n
 * The pending buffer is split on blank lines; each record may carry an
 * "event:" line followed by a "data:" line. */
static int emit_record(anthro_state_t* s, const char* record, size_t len)
{
    while (len && (*record == '\r' || *record == '\n')) {
        ++record;
        --len;
    }
    if (len == 0) {
        return 1;
    }
    /* Find the data: line within the record */
    const char* dline = strstr(record, "data:");
    if (!dline) {
        return 1; /* event-only or blank; ignore */
    }
    dline += 5;
    while (*dline == ' ' || *dline == '\t') {
        ++dline;
    }
    size_t dlen = len - (dline - record);
    if (dlen == 0) {
        return 1;
    }
    /* dline is not NUL-terminated; emit_event will malloc its own copy */
    return emit_event(s, dline, dlen);
}

static int on_sse_progress(void* user, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal,
                           curl_off_t ulnow)
{
    (void)dltotal;
    (void)dlnow;
    (void)ultotal;
    (void)ulnow;
    anthro_state_t* s = user;
    if (s && s->token && aegis_cancellation_token_is_cancelled(s->token)) {
        return 1;
    }
    return 0;
}

static size_t on_sse_write(void* ptr, size_t size, size_t nmemb, void* user)
{
    anthro_state_t* s     = user;
    size_t          total = size * nmemb;
    if (aegis_sse_on_write(ptr, size, nmemb, &s->sse) != total) {
        return 0;
    }
    size_t start = 0;
    for (size_t i = 0; i + 1 < s->sse.pending_len; ++i) {
        if (s->sse.pending[i] == '\n' && s->sse.pending[i + 1] == '\n') {
            size_t record_len = i - start;
            if (!emit_record(s, s->sse.pending + start, record_len)) {
                return 0;
            }
            start = i + 2;
            i     = start ? start - 1 : 0;
        }
    }
    if (start) {
        aegis_sse_compact(&s->sse, start);
    }
    return total;
}

static aegis_status_t structured_stream(void* user, const aegis_model_request_t* req,
                                        const aegis_cancellation_token_t* token,
                                        aegis_model_stream_callback_fn    callback,
                                        void*                             callback_user)
{
    aegis_anthropic_model_ctx_t* ctx = user;
    if (!ctx || !req || !callback) {
        return AEGIS_ERR_INVALID;
    }
    if (token && aegis_cancellation_token_is_cancelled(token)) {
        return AEGIS_ERR_CANCELLED;
    }
    char* body = build_body(req, ctx->model);
    if (!body) {
        return AEGIS_ERR_NOMEM;
    }
    const char* key = ctx->api_key ? ctx->api_key : getenv("ANTHROPIC_API_KEY");
    if (!key || !*key) {
        free(body);
        return AEGIS_ERR_PERM;
    }
    const char* base = ctx->base_url && *ctx->base_url ? ctx->base_url : ANTHROPIC_DEFAULT_URL;
    char url[2048];
    int  u = snprintf(url, sizeof(url), "%s/v1/messages", base);
    if (u < 0 || (size_t)u >= sizeof(url)) {
        free(body);
        return AEGIS_ERR_INVALID;
    }
    CURL* curl = curl_easy_init();
    if (!curl) {
        free(body);
        return AEGIS_ERR_NOMEM;
    }
    char apikey[1024];
    int  a = snprintf(apikey, sizeof(apikey), "x-api-key: %s", key);
    if (a < 0 || (size_t)a >= sizeof(apikey)) {
        curl_easy_cleanup(curl);
        free(body);
        return AEGIS_ERR_INVALID;
    }
    struct curl_slist* headers = NULL;
    headers                    = curl_slist_append(headers, "Content-Type: application/json");
    headers                    = curl_slist_append(headers, "Accept: text/event-stream");
    headers                    = curl_slist_append(headers, apikey);
    headers                    =
        curl_slist_append(headers, "anthropic-version: 2023-06-01");
    anthro_state_t state = {.ctx           = ctx,
                            .req           = req,
                            .token         = token,
                            .callback      = callback,
                            .callback_user = callback_user};
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, on_sse_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &state);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, on_sse_progress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &state);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 10000L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 60000L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    CURLcode cr   = curl_easy_perform(curl);
    long     http = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http);
    if (cr == CURLE_OK && state.sse.pending_len) {
        if (!emit_record(&state, state.sse.pending, state.sse.pending_len)) {
            cr = CURLE_WRITE_ERROR;
        }
    }
    aegis_sse_free(&state.sse);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    free(body);
    if (token && aegis_cancellation_token_is_cancelled(token)) {
        return AEGIS_ERR_CANCELLED;
    }
    if (cr != CURLE_OK) {
        return AEGIS_ERR_PROVIDER;
    }
    if (http == 429) {
        return AEGIS_ERR_MODEL_RATE_LIMIT;
    }
    if (http == 413) {
        return AEGIS_ERR_CONTEXT_OVERFLOW;
    }
    if (http < 200 || http >= 300) {
        return AEGIS_ERR_PROVIDER;
    }
    if (!state.saw_stop) {
        return AEGIS_ERR_PROVIDER;
    }
    return AEGIS_OK;
}

/* ── complete-response parsing ───────────────────────────────────────── */

static int append_content_block_to_message(aegis_message_t* msg, const char* block_json, size_t len,
                                           char* scratch, size_t scratch_cap)
{
    (void)scratch;
    (void)scratch_cap;
    const char* text = json_string_after(block_json, "\"text\"");
    if (text) {
        char* decoded = malloc(len + 1);
        if (!decoded) {
            return 0;
        }
        size_t n = copy_json_string(text, decoded, len + 1);
        aegis_message_set_content(msg, n ? decoded : "");
        free(decoded);
    }
    return 1;
}

static aegis_status_t parse_complete_response(const char* json, size_t len,
                                               aegis_model_response_t** out)
{
    if (!json || !out || len == 0 || len > ANTHROPIC_MAX_RESPONSE) {
        return AEGIS_ERR_INVALID;
    }
    *out                            = NULL;
    aegis_model_response_t* response = NULL;
    aegis_status_t          status   = aegis_model_response_create(&response);
    if (status != AEGIS_OK) {
        return status;
    }
    aegis_message_t* message = NULL;
    status                  = aegis_message_create(AEGIS_MESSAGE_ASSISTANT, &message);
    if (status != AEGIS_OK) {
        aegis_model_response_destroy(response);
        return status;
    }
    /* Concatenate all text content blocks into one content string. */
    char* content = NULL;
    size_t content_len = 0, content_cap = 0;
    const char* p = json;
    while ((p = strstr(p, "\"type\":\"text\"")) != NULL) {
        const char* text = json_string_after(p, "\"text\"");
        if (text) {
            size_t need = len + 1;
            char*  dec  = malloc(need);
            if (!dec) {
                goto cleanup;
            }
            size_t n = copy_json_string(text, dec, need);
            if (content_len + n + 1 > content_cap) {
                content_cap = (content_cap ? content_cap * 2 : 256);
                while (content_cap < content_len + n + 1) {
                    content_cap *= 2;
                }
                char* np = realloc(content, content_cap);
                if (!np) {
                    free(dec);
                    goto cleanup;
                }
                content = np;
            }
            memcpy(content + content_len, dec, n);
            content_len += n;
            content[content_len] = '\0';
            free(dec);
        }
        p += 14;
    }
    if (content_len) {
        aegis_message_set_content(message, content);
    }
    response->message = message;
    json_uint_after(json, "\"input_tokens\"", &response->usage.input_tokens);
    json_uint_after(json, "\"output_tokens\"", &response->usage.output_tokens);
    response->usage.total_tokens =
        response->usage.input_tokens + response->usage.output_tokens;
    response->raw = malloc(len + 1);
    if (!response->raw) {
        free(content);
        aegis_model_response_destroy(response);
        return AEGIS_ERR_NOMEM;
    }
    memcpy(response->raw, json, len);
    response->raw[len] = '\0';
    free(content);
    *out = response;
    return AEGIS_OK;
cleanup:
    free(content);
    aegis_message_destroy(message);
    aegis_model_response_destroy(response);
    return AEGIS_ERR_NOMEM;
}

#ifdef AEGIS_ANTHROPIC_TEST_API
aegis_status_t aegis_anthropic_parse_complete_response(const char* json, size_t len,
                                                        aegis_model_response_t** out)
{
    return parse_complete_response(json, len, out);
}
#endif

static size_t on_complete_write(void* ptr, size_t size, size_t nmemb, void* user)
{
    aegis_json_builder_t* buffer = user;
    size_t total                = size * nmemb;
    return aegis_json_append_raw(buffer, ptr, total) ? total : 0;
}

static aegis_status_t structured_complete(void* user, const aegis_model_request_t* req,
                                          const aegis_cancellation_token_t* token,
                                          aegis_model_response_t**          out)
{
    if (!user || !req || !out) {
        return AEGIS_ERR_INVALID;
    }
    aegis_anthropic_model_ctx_t* ctx = user;
    *out                              = NULL;
    if (token && aegis_cancellation_token_is_cancelled(token)) {
        return AEGIS_ERR_CANCELLED;
    }
    char* body = build_body(req, ctx->model);
    if (!body) {
        return AEGIS_ERR_NOMEM;
    }
    const char* key = ctx->api_key ? ctx->api_key : getenv("ANTHROPIC_API_KEY");
    if (!key || !*key) {
        free(body);
        return AEGIS_ERR_PERM;
    }
    const char* base = ctx->base_url && *ctx->base_url ? ctx->base_url : ANTHROPIC_DEFAULT_URL;
    char        url[2048];
    int         u = snprintf(url, sizeof(url), "%s/v1/messages", base);
    if (u < 0 || (size_t)u >= sizeof(url)) {
        free(body);
        return AEGIS_ERR_INVALID;
    }
    CURL* curl = curl_easy_init();
    if (!curl) {
        free(body);
        return AEGIS_ERR_NOMEM;
    }
    char apikey[1024];
    int  a = snprintf(apikey, sizeof(apikey), "x-api-key: %s", key);
    if (a < 0 || (size_t)a >= sizeof(apikey)) {
        curl_easy_cleanup(curl);
        free(body);
        return AEGIS_ERR_INVALID;
    }
    struct curl_slist* headers = NULL;
    headers                    = curl_slist_append(headers, "Content-Type: application/json");
    headers                    = curl_slist_append(headers, apikey);
    headers                    = curl_slist_append(headers, "anthropic-version: 2023-06-01");
    aegis_json_builder_t response;
    aegis_json_builder_init(&response);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, on_complete_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, aegis_sse_progress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, (void*)token);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 10000L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 60000L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    CURLcode cr   = curl_easy_perform(curl);
    long     http = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http);
    aegis_status_t status = AEGIS_ERR_PROVIDER;
    if (cr == CURLE_OK && http == 429) {
        status = AEGIS_ERR_MODEL_RATE_LIMIT;
    } else if (cr == CURLE_OK && http == 413) {
        status = AEGIS_ERR_CONTEXT_OVERFLOW;
    } else if (cr == CURLE_OK && http >= 200 && http < 300 && response.data) {
        status = parse_complete_response(response.data, response.len, out);
    }
    aegis_json_builder_free(&response);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    free(body);
    if (token && aegis_cancellation_token_is_cancelled(token)) {
        return AEGIS_ERR_CANCELLED;
    }
    return status;
}

/* ── context lifecycle ───────────────────────────────────────────────── */

aegis_status_t aegis_anthropic_model_create(const char* api_key, const char* base_url,
                                            const char* model, aegis_anthropic_model_ctx_t** out,
                                            aegis_model_backend_t* backend)
{
    if (!out || !backend) {
        return AEGIS_ERR_INVALID;
    }
    *out                               = NULL;
    aegis_anthropic_model_ctx_t* ctx   = calloc(1, sizeof(*ctx));
    if (!ctx) {
        return AEGIS_ERR_NOMEM;
    }
    ctx->api_key  = api_key ? strdup(api_key) : NULL;
    ctx->base_url = base_url ? strdup(base_url) : NULL;
    ctx->model    = strdup(model && *model ? model : ANTHROPIC_DEFAULT_MODEL);
    if ((api_key && !ctx->api_key) || (base_url && !ctx->base_url) || !ctx->model) {
        aegis_anthropic_model_destroy(ctx);
        return AEGIS_ERR_NOMEM;
    }
    backend->user     = ctx;
    backend->complete = structured_complete;
    backend->stream   = structured_stream;
    backend->capabilities = AEGIS_MODEL_CAP_TEXT | AEGIS_MODEL_CAP_TOOL_CALLING |
                            AEGIS_MODEL_CAP_STREAMING | AEGIS_MODEL_CAP_REASONING;
    *out = ctx;
    return AEGIS_OK;
}

void aegis_anthropic_model_destroy(aegis_anthropic_model_ctx_t* ctx)
{
    if (!ctx) {
        return;
    }
    free(ctx->api_key);
    free(ctx->base_url);
    free(ctx->model);
    free(ctx);
}
