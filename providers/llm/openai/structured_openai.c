#define _POSIX_C_SOURCE 200809L
#include "structured_openai.h"
#ifdef AEGIS_OPENAI_TEST_API
#include "structured_openai_test.h"
#endif
#include "aegis/message/message.h"
#include "aegis/message/role.h"
#include "aegis/message/tool_call.h"
#include "aegis/tool/tool.h"
#include <curl/curl.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>

#define OPENAI_DEFAULT_URL "https://api.openai.com/v1"
#define OPENAI_DEFAULT_MODEL "gpt-4o-mini"
#define OPENAI_MAX_RESPONSE (16u * 1024u * 1024u)

typedef struct {
    char* data;
    size_t len;
    size_t cap;
} json_buf_t;

typedef struct aegis_openai_model_ctx {
    char* api_key;
    char* base_url;
    char* model;
} aegis_openai_model_ctx_t;

typedef struct {
    aegis_openai_model_ctx_t* ctx;
    const aegis_model_request_t* req;
    const aegis_cancellation_token_t* token;
    aegis_model_stream_callback_fn callback;
    void* callback_user;
    char* pending;
    size_t pending_len;
    size_t pending_cap;
    size_t response_bytes;
    bool saw_done;
    bool call_started[16];
    bool call_ended[16];
} sse_state_t;

static int checked_grow(json_buf_t* b, size_t extra)
{
    if (extra > SIZE_MAX - b->len - 1) return 0;
    size_t need = b->len + extra + 1;
    if (need <= b->cap) return 1;
    size_t cap = b->cap ? b->cap : 256;
    while (cap < need) {
        if (cap > SIZE_MAX / 2) return 0;
        cap *= 2;
    }
    char* p = realloc(b->data, cap);
    if (!p) return 0;
    b->data = p;
    b->cap = cap;
    return 1;
}

static int append_raw(json_buf_t* b, const char* s, size_t n)
{
    if (!checked_grow(b, n)) return 0;
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
    return 1;
}

static int append_json_string(json_buf_t* b, const char* s)
{
    if (!s) s = "";
    if (!append_raw(b, "\"", 1)) return 0;
    for (const unsigned char* p = (const unsigned char*)s; *p; ++p) {
        char esc[7];
        size_t n = 1;
        switch (*p) {
        case '"': esc[0] = '\\'; esc[1] = '"'; n = 2; break;
        case '\\': esc[0] = '\\'; esc[1] = '\\'; n = 2; break;
        case '\n': esc[0] = '\\'; esc[1] = 'n'; n = 2; break;
        case '\r': esc[0] = '\\'; esc[1] = 'r'; n = 2; break;
        case '\t': esc[0] = '\\'; esc[1] = 't'; n = 2; break;
        default:
            if (*p < 0x20) {
                int written = snprintf(esc, sizeof(esc), "\\u%04x", *p);
                if (written != 6) return 0;
                n = 6;
            } else {
                esc[0] = (char)*p;
            }
            break;
        }
        if (!append_raw(b, esc, n)) return 0;
    }
    return append_raw(b, "\"", 1);
}

static int append_message(json_buf_t* b, const aegis_message_t* m)
{
    const char* role = aegis_message_role_str(aegis_message_role(m));
    if (!append_raw(b, "{\"role\":", strlen("{\"role\":")) || !append_json_string(b, role)) return 0;
    const char* content = aegis_message_content(m);
    if (content) {
        if (!append_raw(b, ",\"content\":", strlen(",\"content\":")) || !append_json_string(b, content)) return 0;
    } else {
        if (!append_raw(b, ",\"content\":null", strlen(",\"content\":null"))) return 0;
    }
    if (aegis_message_role(m) == AEGIS_MESSAGE_ASSISTANT && aegis_message_tool_call_count(m) > 0) {
        if (                !append_raw(b, ",\"tool_calls\":[", strlen(",\"tool_calls\":["))) return 0;

        for (size_t i = 0; i < aegis_message_tool_call_count(m); ++i) {
            const aegis_tool_call_t* c = aegis_message_tool_call_at(m, i);
            if (i && !append_raw(b, ",", 1)) return 0;
            if (!append_raw(b, "{\"id\":", strlen("{\"id\":")) || !append_json_string(b, aegis_tool_call_id(c)) ||
                !append_raw(b, ",\"type\":\"function\",\"function\":{\"name\":", strlen(",\"type\":\"function\",\"function\":{\"name\":")) ||
                !append_json_string(b, aegis_tool_call_name(c)) ||
                !append_raw(b, ",\"arguments\":", strlen(",\"arguments\":")) ||
                !append_json_string(b, aegis_tool_call_arguments(c)) || !append_raw(b, "}}", 2)) return 0;
        }
        if (!append_raw(b, "]", 1)) return 0;
    }
    if (aegis_message_role(m) == AEGIS_MESSAGE_TOOL) {
        const char* id = aegis_message_tool_call_id(m);            if (id && (!append_raw(b, ",\"tool_call_id\":", strlen(",\"tool_call_id\":")) || !append_json_string(b, id))) return 0;

    }
    return append_raw(b, "}", 1);
}

typedef struct {
    json_buf_t* buffer;
    aegis_status_t status;
    bool first;
} tool_json_context_t;

static aegis_status_t append_tool_json(const aegis_tool_def_t* def, void* user)
{
    tool_json_context_t* ctx = user;
    if (!def || !ctx || ctx->status != AEGIS_OK) return AEGIS_ERR_INVALID;
    json_buf_t* b = ctx->buffer;
    if (!ctx->first && !append_raw(b, ",", 1)) { ctx->status = AEGIS_ERR_NOMEM; return ctx->status; }
    ctx->first = false;
    if (!append_raw(b, "{\"type\":\"function\",\"function\":{\"name\":", strlen("{\"type\":\"function\",\"function\":{\"name\":")) ||
        !append_json_string(b, def->name) || append_raw(b, ",\"description\":", strlen(",\"description\":")) == 0 ||
        !append_json_string(b, def->description ? def->description : "") ||
        !append_raw(b, ",\"parameters\":{\"type\":\"object\",\"properties\":{", strlen(",\"parameters\":{\"type\":\"object\",\"properties\":{"))) {
        ctx->status = AEGIS_ERR_NOMEM; return ctx->status;
    }
    for (size_t i = 0; i < def->schema.param_count; ++i) {
        const aegis_tool_param_spec_t* p = &def->schema.params[i];
        if (i && !append_raw(b, ",", 1)) { ctx->status = AEGIS_ERR_NOMEM; return ctx->status; }
        if (!append_json_string(b, p->name) || !append_raw(b, ":{\"type\":", strlen(":{\"type\":"))) {
            ctx->status = AEGIS_ERR_NOMEM; return ctx->status;
        }
        const char* type = p->type == AEGIS_TOOL_VAL_BOOL ? "boolean" :
                          p->type == AEGIS_TOOL_VAL_INT ? "integer" :
                          p->type == AEGIS_TOOL_VAL_FLOAT ? "number" : "string";
        if (!append_json_string(b, type)) { ctx->status = AEGIS_ERR_NOMEM; return ctx->status; }
        if (p->description && (!append_raw(b, ",\"description\":", strlen(",\"description\":")) || !append_json_string(b, p->description))) {
            ctx->status = AEGIS_ERR_NOMEM; return ctx->status;
        }
        if (!append_raw(b, "}", 1)) { ctx->status = AEGIS_ERR_NOMEM; return ctx->status; }
    }
    if (!append_raw(b, "},\"required\":[", strlen("},\"required\":["))) { ctx->status = AEGIS_ERR_NOMEM; return ctx->status; }
    bool first = true;
    for (size_t i = 0; i < def->schema.param_count; ++i) {
        const aegis_tool_param_spec_t* p = &def->schema.params[i];
        if (!p->required) continue;
        if (!first && !append_raw(b, ",", 1)) { ctx->status = AEGIS_ERR_NOMEM; return ctx->status; }
        first = false;
        if (!append_json_string(b, p->name)) { ctx->status = AEGIS_ERR_NOMEM; return ctx->status; }
    }
    if (!append_raw(b, "]}}}", strlen("]}}}"))) { ctx->status = AEGIS_ERR_NOMEM; return ctx->status; }
    return ctx->status;
}

static char* build_body(const aegis_model_request_t* req, const char* fallback_model)
{
    const char* model_name = req->model && *req->model ? req->model : fallback_model;
    json_buf_t  b          = {0};
    if (!append_raw(&b, "{\"model\":", strlen("{\"model\":")) ||
        !append_json_string(&b, model_name) ||
        !append_raw(&b, ",\"messages\":[", strlen(",\"messages\":["))) goto fail;
    size_t n = req->messages ? aegis_message_list_count(req->messages) : 0;
    for (size_t i = 0; i < n; ++i) {
        if (i && !append_raw(&b, ",", 1)) goto fail;
        if (!append_message(&b, aegis_message_list_at(req->messages, i))) goto fail;
    }
    if (!append_raw(&b, "]", 1)) goto fail;
    if (req->tools && aegis_tool_registry_count(req->tools) > 0) {
        if (!append_raw(&b, ",\"tools\":[", strlen(",\"tools\":["))) goto fail;
        tool_json_context_t ctx = {.buffer = &b, .status = AEGIS_OK, .first = true};
        if (aegis_tool_registry_visit(req->tools, append_tool_json, &ctx) != AEGIS_OK || ctx.status != AEGIS_OK) goto fail;
        if (!append_raw(&b, "]", 1)) goto fail;
    }
    if (req->stream && !append_raw(&b, ",\"stream\":true", strlen(",\"stream\":true"))) goto fail;
    if (req->max_tokens) {
        char tmp[64]; int k = snprintf(tmp, sizeof(tmp), ",\"max_tokens\":%u", req->max_tokens);
        if (k < 0 || !append_raw(&b, tmp, (size_t)k)) goto fail;
    }
    if (!append_raw(&b, "}", 1)) goto fail;
    return b.data;
fail:
    free(b.data);
    return NULL;
}

static int json_uint_after(const char* json, const char* key, uint32_t* out);

static const char* json_string_after(const char* json, const char* key)
{
    const char* p = strstr(json, key);
    if (!p) return NULL;
    p = strchr(p + strlen(key), ':');
    if (!p) return NULL;
    ++p;
    while (*p == ' ' || *p == '\t') ++p;
    if (*p != '"') return NULL;
    return p + 1;
}

static size_t copy_json_string(const char* p, char* out, size_t cap)
{
    if (!p || !out || cap == 0) return 0;
    size_t n = 0;
    while (*p && *p != '"') {
        if (*p == '\\' && p[1]) {
            ++p;
            if (*p == 'n') out[n++] = '\n';
            else if (*p == 'r') out[n++] = '\r';
            else if (*p == 't') out[n++] = '\t';
            else out[n++] = *p;
            ++p;
        } else {
            out[n++] = *p++;
        }
        if (n + 1 >= cap) return 0;
    }
    if (*p != '\"') return 0;
    out[n] = '\0';
    return n;
}

static int emit_record(sse_state_t* s, const char* record, size_t len)
{
    while (len && (*record == '\r' || *record == '\n')) { ++record; --len; }
    if (len == 0 || record[0] == ':') return 1;
    if (len < 5 || memcmp(record, "data:", 5) != 0) return 1;
    record += 5; len -= 5;
    while (len && (*record == ' ' || *record == '\t')) { ++record; --len; }
    if (len == 6 && memcmp(record, "[DONE]", 6) == 0) {
        aegis_model_stream_event_t ev = {.type = AEGIS_MODEL_STREAM_END};
        for (uint32_t i = 0; i < 16; ++i) {
            if (s->call_started[i] && !s->call_ended[i]) {
                aegis_model_stream_event_t end = {.type = AEGIS_MODEL_STREAM_TOOL_CALL_END, .index = i};
                if (s->callback(&end, s->callback_user) != AEGIS_OK) return 0;
                s->call_ended[i] = true;
            }
        }
        if (s->callback(&ev, s->callback_user) != AEGIS_OK) return 0;
        s->saw_done = true;
        return 1;
    }
    if (len > OPENAI_MAX_RESPONSE || s->response_bytes > OPENAI_MAX_RESPONSE - len) return 0;
    s->response_bytes += len;
    char* json = malloc(len + 1);
    if (!json) return 0;
    memcpy(json, record, len); json[len] = '\0';
    /* Reasoning first: check the long key before the short one so a record
     * carrying "reasoning_content" never double-matches "reasoning". If the
     * long key exists but its value is not a string, the short-key lookup
     * lands on the same position and fails identically (returns NULL). */
    const char* rcontent = json_string_after(json, "\"reasoning_content\"");
    if (!rcontent) rcontent = json_string_after(json, "\"reasoning\"");
    if (rcontent) {
        char* decoded = malloc(len + 1);
        if (!decoded) { free(json); return 0; }
        size_t n = copy_json_string(rcontent, decoded, len + 1);
        aegis_model_stream_event_t ev = {.type = AEGIS_MODEL_STREAM_REASONING_DELTA, .data = decoded, .len = n};
        aegis_status_t rc = s->callback(&ev, s->callback_user);
        free(decoded);
        if (rc != AEGIS_OK) { free(json); return 0; }
    }
    const char* content = json_string_after(json, "\"content\"");
    if (content) {
        char* decoded = malloc(len + 1);
        if (!decoded) { free(json); return 0; }
        size_t n = copy_json_string(content, decoded, len + 1);
        if (!n && *content != '"') { free(decoded); free(json); return 0; }
        aegis_model_stream_event_t ev = {.type = AEGIS_MODEL_STREAM_TEXT_DELTA, .data = decoded, .len = n};
        aegis_status_t rc = s->callback(&ev, s->callback_user);
        free(decoded);
        if (rc != AEGIS_OK) { free(json); return 0; }
    }
    const char* tool_name = json_string_after(json, "\"name\"");
    const char* call_id = json_string_after(json, "\"id\"");
    const char* arguments = json_string_after(json, "\"arguments\"");
    uint32_t tool_index = 0;
    const char* index_key = strstr(json, "\"index\"");
    if (index_key) {
        uint32_t parsed_index = 0;
        if (json_uint_after(json, "\"index\"", &parsed_index)) tool_index = parsed_index;
    }
    if (tool_name || arguments) {
        char name[512] = {0}, id[256] = {0};
        size_t name_len = tool_name ? copy_json_string(tool_name, name, sizeof(name)) : 0;
        size_t id_len = call_id ? copy_json_string(call_id, id, sizeof(id)) : 0;
        if ((tool_name && !name_len) || (call_id && !id_len)) { free(json); return 0; }
        aegis_model_stream_event_t start = {
            .type = AEGIS_MODEL_STREAM_TOOL_CALL_START, .index = tool_index,
            .tool_name = tool_name ? name : NULL, .call_id = call_id ? id : NULL};
        if (tool_index >= 16) { free(json); return 0; }
        if (!s->call_started[tool_index] && (tool_name || call_id)) {
            if (s->callback(&start, s->callback_user) != AEGIS_OK) { free(json); return 0; }
            s->call_started[tool_index] = true;
        }
        if (arguments) {
            char* arg = malloc(len + 1);
            if (!arg) { free(json); return 0; }
            size_t arg_len = copy_json_string(arguments, arg, len + 1);
            /* Only carry name/id when present in THIS chunk; NULL lets the
             * accumulator keep values from the START chunk. */
            aegis_model_stream_event_t delta = {.type = AEGIS_MODEL_STREAM_TOOL_CALL_DELTA,
                                                 .data = arg, .len = arg_len, .index = tool_index,
                                                 .tool_name = tool_name ? name : NULL,
                                                 .call_id = call_id ? id : NULL};
            aegis_status_t rc = s->callback(&delta, s->callback_user);
            free(arg);
            if (rc != AEGIS_OK) { free(json); return 0; }
        }
    }
    free(json);
    return 1;
}

/* curl progress callback: abort the transfer as soon as the token is
 * cancelled. libcurl polls this at least once per second while idle and
 * on every data event while streaming, so an interrupt takes effect at
 * the next chunk boundary (or within ~1s during a silent stall). */
static int sse_progress(void* user, curl_off_t dltotal, curl_off_t dlnow,
                        curl_off_t ultotal, curl_off_t ulnow)
{
    (void)dltotal; (void)dlnow; (void)ultotal; (void)ulnow;
    sse_state_t* s = user;
    if (s && s->token && aegis_cancellation_token_is_cancelled(s->token)) {
        return 1; /* non-zero aborts the transfer */
    }
    return 0;
}

static size_t on_sse_write(void* ptr, size_t size, size_t nmemb, void* user)
{
    sse_state_t* s = user;
    size_t total = size * nmemb;
    if (!total || s->pending_len > SIZE_MAX - total - 1) return 0;
    if (s->pending_len + total + 1 > s->pending_cap) {
        size_t cap = s->pending_cap ? s->pending_cap * 2 : 4096;
        while (cap < s->pending_len + total + 1) {
            if (cap > SIZE_MAX / 2) return 0;
            cap *= 2;
        }
        char* p = realloc(s->pending, cap);
        if (!p) return 0;
        s->pending = p; s->pending_cap = cap;
    }
    memcpy(s->pending + s->pending_len, ptr, total);
    s->pending_len += total; s->pending[s->pending_len] = '\0';
    size_t start = 0;
    for (size_t i = 0; i + 1 < s->pending_len; ++i) {
        if (s->pending[i] == '\n' && s->pending[i + 1] == '\n') {
            size_t record_len = i - start;
            if (!emit_record(s, s->pending + start, record_len)) return 0;
            start = i + 2; i = start ? start - 1 : 0;
        }
    }
    if (start) {
        memmove(s->pending, s->pending + start, s->pending_len - start);
        s->pending_len -= start;
        s->pending[s->pending_len] = '\0';
    }
    return total;
}

static aegis_status_t structured_stream(void* user, const aegis_model_request_t* req,
                                        const aegis_cancellation_token_t* token,
                                        aegis_model_stream_callback_fn callback, void* callback_user)
{
    aegis_openai_model_ctx_t* ctx = user;
    if (!ctx || !req || !callback) return AEGIS_ERR_INVALID;
    if (token && aegis_cancellation_token_is_cancelled(token)) return AEGIS_ERR_CANCELLED;
    char* body = build_body(req, ctx->model);
    if (!body) return AEGIS_ERR_NOMEM;
    const char* key = ctx->api_key ? ctx->api_key : getenv("OPENAI_API_KEY");
    if (!key || !*key) { free(body); return AEGIS_ERR_PERM; }
    const char* base = ctx->base_url && *ctx->base_url ? ctx->base_url : OPENAI_DEFAULT_URL;
    const char* model = req->model && *req->model ? req->model : ctx->model;
    (void)model;
    char url[2048];    int u = snprintf(url, sizeof(url), "%s/chat/completions", base);
    if (u < 0 || (size_t)u >= sizeof(url)) { free(body); return AEGIS_ERR_INVALID; }
    CURL* curl = curl_easy_init();
    if (!curl) { free(body); return AEGIS_ERR_NOMEM; }
    char auth[1024];
    int a = snprintf(auth, sizeof(auth), "Authorization: Bearer %s", key);
    if (a < 0 || (size_t)a >= sizeof(auth)) { curl_easy_cleanup(curl); free(body); return AEGIS_ERR_INVALID; }
    struct curl_slist* headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "Accept: text/event-stream");
    headers = curl_slist_append(headers, auth);
    sse_state_t state = {.ctx = ctx, .req = req, .token = token, .callback = callback, .callback_user = callback_user};
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, on_sse_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &state);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, sse_progress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &state);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 10000L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 60000L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    CURLcode cr = curl_easy_perform(curl);
    long http = 0; curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http);
    if (cr == CURLE_OK && state.pending_len) {
        if (!emit_record(&state, state.pending, state.pending_len)) cr = CURLE_WRITE_ERROR;
    }
    free(state.pending); curl_slist_free_all(headers); curl_easy_cleanup(curl); free(body);
    /* Cancellation takes precedence: an aborted-by-callback transfer is a
     * cancel, not a provider failure. */
    if (token && aegis_cancellation_token_is_cancelled(token)) return AEGIS_ERR_CANCELLED;
    if (cr != CURLE_OK) return AEGIS_ERR_PROVIDER;
    if (http < 200 || http >= 300) return AEGIS_ERR_PROVIDER;
    if (!state.saw_done) return AEGIS_ERR_PROVIDER;
    return AEGIS_OK;
}

static int json_uint_after(const char* json, const char* key, uint32_t* out)
{
    const char* p = strstr(json, key);
    if (!p) return 0;
    p = strchr(p + strlen(key), ':');
    if (!p) return 0;
    ++p;
    while (*p == ' ' || *p == '\t') ++p;
    char* end = NULL;
    unsigned long value = strtoul(p, &end, 10);
    if (end == p || value > UINT32_MAX) return 0;
    *out = (uint32_t)value;
    return 1;
}

static aegis_status_t parse_complete_response(const char* json, size_t len,
                                              aegis_model_response_t** out)
{
    if (!json || !out || len == 0 || len > OPENAI_MAX_RESPONSE) return AEGIS_ERR_INVALID;
    *out = NULL;
    aegis_model_response_t* response = NULL;
    aegis_status_t status = aegis_model_response_create(&response);
    if (status != AEGIS_OK) return status;
    const char* content = json_string_after(json, "\"content\"");
    if (!content) { aegis_model_response_destroy(response); return AEGIS_ERR_PROVIDER; }
    char* decoded = malloc(len + 1);
    if (!decoded) { aegis_model_response_destroy(response); return AEGIS_ERR_NOMEM; }
    size_t content_len = copy_json_string(content, decoded, len + 1);
    if (content_len == 0 && *content != '"') {
        free(decoded); aegis_model_response_destroy(response); return AEGIS_ERR_PROVIDER;
    }
    aegis_message_t* message = NULL;
    status = aegis_message_create(AEGIS_MESSAGE_ASSISTANT, &message);
    if (status == AEGIS_OK) status = aegis_message_set_content(message, decoded);
    free(decoded);
    if (status != AEGIS_OK) {
        aegis_message_destroy(message); aegis_model_response_destroy(response); return status;
    }
    response->message = message;
    (void)json_uint_after(json, "\"prompt_tokens\"", &response->usage.input_tokens);
    (void)json_uint_after(json, "\"completion_tokens\"", &response->usage.output_tokens);
    (void)json_uint_after(json, "\"total_tokens\"", &response->usage.total_tokens);
    response->raw = malloc(len + 1);
    if (!response->raw) { aegis_model_response_destroy(response); return AEGIS_ERR_NOMEM; }
    memcpy(response->raw, json, len); response->raw[len] = '\0';
    *out = response;
    return AEGIS_OK;
}

#ifdef AEGIS_OPENAI_TEST_API
aegis_status_t aegis_openai_parse_complete_response(const char* json, size_t len,
                                                     aegis_model_response_t** out)
{
    return parse_complete_response(json, len, out);
}
#endif

static size_t on_complete_write(void* ptr, size_t size, size_t nmemb, void* user)
{
    json_buf_t* buffer = user;
    size_t total = size * nmemb;
    return append_raw(buffer, ptr, total) ? total : 0;
}

static aegis_status_t structured_complete(void* user, const aegis_model_request_t* req,
                                          const aegis_cancellation_token_t* token,
                                          aegis_model_response_t** out)
{
    if (!user || !req || !out) return AEGIS_ERR_INVALID;
    aegis_openai_model_ctx_t* ctx = user;
    *out = NULL;
    if (token && aegis_cancellation_token_is_cancelled(token)) return AEGIS_ERR_CANCELLED;
    char* body = build_body(req, ctx->model);
    if (!body) return AEGIS_ERR_NOMEM;
    const char* key = ctx->api_key ? ctx->api_key : getenv("OPENAI_API_KEY");
    if (!key || !*key) { free(body); return AEGIS_ERR_PERM; }
    const char* base = ctx->base_url && *ctx->base_url ? ctx->base_url : OPENAI_DEFAULT_URL;
    char url[2048];
    int u = snprintf(url, sizeof(url), "%s/chat/completions", base);
    if (u < 0 || (size_t)u >= sizeof(url)) { free(body); return AEGIS_ERR_INVALID; }
    CURL* curl = curl_easy_init();
    if (!curl) { free(body); return AEGIS_ERR_NOMEM; }
    char auth[1024];
    int a = snprintf(auth, sizeof(auth), "Authorization: Bearer %s", key);
    if (a < 0 || (size_t)a >= sizeof(auth)) { curl_easy_cleanup(curl); free(body); return AEGIS_ERR_INVALID; }
    struct curl_slist* headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, auth);
    json_buf_t response = {0};
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, on_complete_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    { sse_state_t ps = {.ctx = ctx, .req = req, .token = token};
      curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
      curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, sse_progress);
      curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &ps); }
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 10000L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 60000L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    CURLcode cr = curl_easy_perform(curl);
    long http = 0; curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http);
    aegis_status_t status = AEGIS_ERR_PROVIDER;
    if (cr == CURLE_OK && http >= 200 && http < 300 && response.data) {
        status = parse_complete_response(response.data, response.len, out);
    }
    free(response.data); curl_slist_free_all(headers); curl_easy_cleanup(curl); free(body);
    if (token && aegis_cancellation_token_is_cancelled(token)) return AEGIS_ERR_CANCELLED;
    return status;
}

aegis_status_t aegis_openai_model_create(const char* api_key, const char* base_url,
                                         const char* model, aegis_openai_model_ctx_t** out,
                                         aegis_model_backend_t* backend)
{
    if (!out || !backend) return AEGIS_ERR_INVALID;
    *out = NULL;
    aegis_openai_model_ctx_t* ctx = calloc(1, sizeof(*ctx));
    if (!ctx) return AEGIS_ERR_NOMEM;
    ctx->api_key = api_key ? strdup(api_key) : NULL;
    ctx->base_url = base_url ? strdup(base_url) : NULL;
    ctx->model = strdup(model && *model ? model : OPENAI_DEFAULT_MODEL);
    if ((api_key && !ctx->api_key) || (base_url && !ctx->base_url) || !ctx->model) {
        aegis_openai_model_destroy(ctx); return AEGIS_ERR_NOMEM;
    }
    backend->user = ctx;
    backend->complete = structured_complete;
    backend->stream = structured_stream;
    backend->capabilities = AEGIS_MODEL_CAP_TEXT | AEGIS_MODEL_CAP_TOOL_CALLING | AEGIS_MODEL_CAP_STREAMING;
    *out = ctx;
    return AEGIS_OK;
}

void aegis_openai_model_destroy(aegis_openai_model_ctx_t* ctx)
{
    if (!ctx) return;
    free(ctx->api_key); free(ctx->base_url); free(ctx->model); free(ctx);
}
