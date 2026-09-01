/**
 * @file openai_llm.c
 * @brief OpenAI-compatible Chat Completions provider implementation.
 *
 * Uses libcurl for HTTPS. Parses the JSON response by scanning for
 * "choices[0].message.content" — no external JSON library needed.
 * Supports any OpenAI-compatible endpoint (OpenAI, Azure, vLLM, Ollama).
 */
#define _POSIX_C_SOURCE 200809L
#include "aegis/provider/openai_llm.h"

#include "aegis/common/cancellation/cancellation.h"
#include "aegis/types.h"

#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Defaults ─────────────────────────────────────────────────────────────── */

#define OPENAI_DEFAULT_BASE_URL "https://api.openai.com/v1"
#define OPENAI_DEFAULT_MODEL    "gpt-4o-mini"
#define OPENAI_MAX_TOKENS       2048u
#define OPENAI_TEMPERATURE      0.7f
#define OPENAI_INIT_BUF_SIZE    4096u

/* ── Context ──────────────────────────────────────────────────────────────── */

typedef struct openai_llm_ctx {
    char* api_key;
    char* base_url;
    char* model;
} openai_llm_ctx_t;

/* ── curl write buffer ────────────────────────────────────────────────────── */

typedef struct {
    char* buf;
    size_t len;
    size_t cap;
} write_buf_t;

static size_t on_write(void* ptr, size_t size, size_t nmemb, void* userdata)
{
    write_buf_t* w = (write_buf_t*)userdata;
    size_t       total = size * nmemb;
    if (total == 0) {
        return 0;
    }
    char* nb = (char*)realloc(w->buf, w->cap + total + 1);
    if (!nb) {
        return 0;
    }
    w->buf      = nb;
    w->cap     += total;
    memcpy(w->buf + w->len, ptr, total);
    w->len     += total;
    w->buf[w->len] = '\0';
    return total;
}

/* ── JSON content extractor ───────────────────────────────────────────────── */

/**
 * Pull "choices[0].message.content" from a JSON body.
 * Handles escaped quotes inside the string value.
 * Returns a heap-allocated string (caller must free) or NULL.
 */
static char* pull_content(const char* json, size_t json_len)
{
    if (!json || json_len == 0) {
        return NULL;
    }

    /* Locate "choices":[ */
    const char* p = strstr(json, "\"choices\"");
    if (!p) {
        return NULL;
    }
    p = strchr(p, '[');
    if (!p || p[1] != '[') {
        return NULL;
    }
    p++; /* advance past '[' */

    /* Locate first "{" => start of first choice object */
    p = strchr(p, '{');
    if (!p) {
        return NULL;
    }

    /* Scan at most 2k bytes for "message":{"content":...} */
    const char* limit = p + 2048;
    if ((size_t)(limit - json) > json_len) {
        limit = json + json_len;
    }

    const char* msg   = strstr(p, "\"message\"");
    if (!msg || msg > limit) {
        return NULL;
    }
    const char* mb = strchr(msg, '{');
    if (!mb) {
        return NULL;
    }
    const char* ck = strstr(mb, "\"content\"");
    if (!ck || ck > mb + 512) {
        return NULL;
    }
    const char* col = strchr(ck, ':');
    if (!col) {
        return NULL;
    }
    const char* v = col + 1;
    while (*v == ' ' || *v == '\t') {
        v++;
    }
    if (*v != '"') {
        return NULL;
    }
    v++; /* skip opening quote */

    /* Accumulate until unescaped closing quote */
    size_t cap = 256, n = 0;
    char*  out = (char*)malloc(cap);
    if (!out) {
        return NULL;
    }
    while (*v && n + 2 < cap) {
        if (*v == '\\' && *(v + 1) == '"') {
            out[n++] = '"';
            v += 2;
        } else if (*v == '\\' && *(v + 1) == '\\') {
            out[n++] = '\\';
            v += 2;
        } else if (*v == '\n') {
            out[n++] = '\n';
            v++;
        } else if (*v == '\r') {
            out[n++] = '\n';
            v++;
        } else if (*v == '\t') {
            out[n++] = '\t';
            v++;
        } else if (*v == '"') {
            break;
        } else {
            if (n + 2 >= cap) {
                cap *= 2;
                char* nb = (char*)realloc(out, cap);
                if (!nb) {
                    free(out);
                    return NULL;
                }
                out = nb;
            }
            out[n++] = *v++;
        }
    }
    out[n] = '\0';
    return out;
}

/* ── Request builder ─────────────────────────────────────────────────────── */

static char* build_body(const openai_llm_ctx_t* ctx,
                        const aegis_llm_request_t* req)
{
    size_t prompt_len = req ? req->prompt_len : 0;
    const char* prompt = req ? (const char*)req->prompt : NULL;

    /* Estimate: fixed overhead ~200 bytes + prompt */
    size_t est = 256 + prompt_len;
    char* buf = (char*)malloc(est);
    if (!buf) {
        return NULL;
    }

    uint32_t max_tokens = req ? req->max_tokens : OPENAI_MAX_TOKENS;
    float    temp       = req ? req->temperature : OPENAI_TEMPERATURE;

    /* Write prefix */
    /* Use configured model or fall back to env var or default */
    const char* model = ctx->model;
    if (!model || model[0] == '\0') {
        model = getenv("OPENAI_MODEL");
    }
    if (!model || model[0] == '\0') {
        model = getenv("AEGIS_OPENAI_MODEL");
    }
    if (!model || model[0] == '\0') {
        model = OPENAI_DEFAULT_MODEL;
    }
    int n = snprintf(buf, est,
        "{\"model\":\"%s\","
         "\"messages\":[{\"role\":\"user\",\"content\":\"",
        model);
        free(buf);
        return NULL;
    }

    /* Escape and append prompt */
    char* dst = buf + n;
    size_t rem = est - (size_t)n;
    for (size_t i = 0; i < prompt_len; i++) {
        unsigned char c = (unsigned char)prompt[i];
        if (c == '\\' || c == '"') {
            if (rem < 3) { free(buf); return NULL; }
            *dst++ = '\\';
            *dst++ = (c == '"') ? '"' : '\\';
            rem -= 2;
        } else if (c == '\n') {
            if (rem < 3) { free(buf); return NULL; }
            *dst++ = '\\'; *dst++ = 'n';
            rem -= 2;
        } else if (c == '\r') {
            if (rem < 3) { free(buf); return NULL; }
            *dst++ = '\\'; *dst++ = 'r';
            rem -= 2;
        } else if (c == '\t') {
            if (rem < 3) { free(buf); return NULL; }
            *dst++ = '\\'; *dst++ = 't';
            rem -= 2;
        } else if (c < 0x20) {
            if (rem < 6) { free(buf); return NULL; }
            dst += (int)snprintf(dst, rem, "\\u00%02x", c);
            rem -= 6;
        } else {
            if (rem < 1) { free(buf); return NULL; }
            *dst++ = (char)c;
            rem--;
        }
    }

    /* Suffix */
    int n2 = snprintf(dst, rem,
        "\"}],"
         "\"max_tokens\":%u,"
         "\"temperature\":%.4f"
         "}",
        max_tokens, temp);
    if (n2 < 0 || (size_t)n2 >= rem) {
        free(buf);
        return NULL;
    }
    return buf;
}

/* ── Init / Shutdown ─────────────────────────────────────────────────────── */

static aegis_status_t openai_llm_init(void* user)
{
    (void)user;
    CURL* curl = curl_easy_init();
    if (!curl) {
        return AEGIS_ERR_NOMEM;
    }
    curl_easy_cleanup(curl);
    return AEGIS_OK;
}

static void openai_llm_shutdown(void* user)
{
    (void)user;
}

/* ── Completion callback ─────────────────────────────────────────────────── */

static aegis_status_t openai_llm_complete(void* ctx_ptr,
                                           const aegis_llm_request_t* req,
                                           const aegis_cancellation_token_t* token,
                                           aegis_llm_response_t* out)
{
    openai_llm_ctx_t* ctx = (openai_llm_ctx_t*)ctx_ptr;
    if (!ctx || !out) {
        return AEGIS_ERR_INVALID;
    }
    memset(out, 0, sizeof(*out));

    if (token && aegis_cancellation_token_is_cancelled(token)) {
        return AEGIS_ERR_CANCELLED;
    }

    char* body = build_body(ctx, req);
    if (!body) {
        return AEGIS_ERR_NOMEM;
    }

    /* Resolve API key */
    const char* api_key = ctx->api_key;
    if (!api_key || api_key[0] == '\0') {
        api_key = getenv("OPENAI_API_KEY");
    }
    if (!api_key || api_key[0] == '\0') {
        api_key = getenv("AEGIS_OPENAI_API_KEY");
    }
    if (!api_key || api_key[0] == '\0') {
        free(body);
        fprintf(stderr, "error: no OpenAI API key (set --api-key or OPENAI_API_KEY)\n");
        return AEGIS_ERR_PERM;
    }

    /* Resolve base URL */
    const char* base_url = ctx->base_url;
    if (!base_url || base_url[0] == '\0') {
        base_url = getenv("AEGIS_OPENAI_BASE_URL");
    }
    if (!base_url || base_url[0] == '\0') {
        base_url = OPENAI_DEFAULT_BASE_URL;
    }

    char url[2048];
    snprintf(url, sizeof(url), "%s/chat/completions", base_url);

    CURL* curl = curl_easy_init();
    if (!curl) {
        free(body);
        return AEGIS_ERR_NOMEM;
    }

    write_buf_t wbuf = {NULL, 0, 0};
    wbuf.cap = OPENAI_INIT_BUF_SIZE;
    wbuf.buf = (char*)malloc(wbuf.cap);
    if (!wbuf.buf) {
        curl_easy_cleanup(curl);
        free(body);
        return AEGIS_ERR_NOMEM;
    }

    char auth_hdr[1024];
    snprintf(auth_hdr, sizeof(auth_hdr), "Authorization: Bearer %s", api_key);

    struct curl_slist* headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, auth_hdr);

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, on_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &wbuf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 60000L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);
    curl_slist_free_all(headers);
    free(body);

    if (res != CURLE_OK) {
        fprintf(stderr, "error: curl: %s\n", curl_easy_strerror(res));
        free(wbuf.buf);
        return AEGIS_ERR_PROVIDER;
    }

    if (http_code != 200) {
        fprintf(stderr, "error: HTTP %ld\n", http_code);
        if (wbuf.buf) {
            fprintf(stderr, "response: %.512s\n", wbuf.buf);
        }
        free(wbuf.buf);
        return AEGIS_ERR_PROVIDER;
    }

    char* content = pull_content(wbuf.buf, wbuf.len);
    free(wbuf.buf);

    if (!content) {
        fprintf(stderr, "error: could not parse OpenAI response\n");
        return AEGIS_ERR_PROVIDER;
    }

    out->data = content;
    out->len  = strlen(content);
    return AEGIS_OK;
}
/* ── Streaming callback ─────────────────────────────────────────────────── */

static aegis_status_t openai_llm_stream(void* ctx_ptr,
                                         const aegis_llm_request_t* req,
                                         const aegis_cancellation_token_t* token,
                                         aegis_llm_stream_fn yield,
                                         void* yield_user)
{
    openai_llm_ctx_t* ctx = (openai_llm_ctx_t*)ctx_ptr;
    if (!ctx || !yield) {
        return AEGIS_ERR_INVALID;
    }

    if (token && aegis_cancellation_token_is_cancelled(token)) {
        return AEGIS_ERR_CANCELLED;
    }

    char* body = build_body(ctx, req);
    if (!body) {
        return AEGIS_ERR_NOMEM;
    }

    /* Resolve API key */
    const char* api_key = ctx->api_key;
    if (!api_key || api_key[0] == '\0') {
        api_key = getenv("OPENAI_API_KEY");
    }
    if (!api_key || api_key[0] == '\0') {
        api_key = getenv("AEGIS_OPENAI_API_KEY");
    }
    if (!api_key || api_key[0] == '\0') {
        free(body);
        fprintf(stderr, "error: no OpenAI API key\n");
        return AEGIS_ERR_PERM;
    }

    /* Resolve base URL */
    const char* base_url = ctx->base_url;
    if (!base_url || base_url[0] == '\0') {
        base_url = getenv("AEGIS_OPENAI_BASE_URL");
    }
    if (!base_url || base_url[0] == '\0') {
        base_url = OPENAI_DEFAULT_BASE_URL;
    }

    char url[2048];
    snprintf(url, sizeof(url), "%s/chat/completions", base_url);

    CURL* curl = curl_easy_init();
    if (!curl) {
        free(body);
        return AEGIS_ERR_NOMEM;
    }

    /* Buffer for SSE line accumulation */
    char line_buf[4096];
    size_t line_len = 0;

    struct curl_slist* headers = NULL;
    char auth_hdr[1024];
    snprintf(auth_hdr, sizeof(auth_hdr), "Authorization: Bearer %s", api_key);
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, auth_hdr);
    headers = curl_slist_append(headers, "Accept: text/event-stream");

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 60000L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);

    /* Use a custom write function that processes SSE lines */
    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    curl_slist_free_all(headers);
    free(body);

    if (res != CURLE_OK) {
        fprintf(stderr, "error: curl: %s\n", curl_easy_strerror(res));
        return AEGIS_ERR_PROVIDER;
    }

    return AEGIS_OK;
}
/* ── Factory ─────────────────────────────────────────────────────────────── */

aegis_status_t aegis_openai_llm_create(openai_llm_ctx_t** out_ctx,
                                       const aegis_llm_ops_t** out_ops,
                                       aegis_provider_def_t* out_def)
{
    if (!out_ctx || !out_ops || !out_def) {
        return AEGIS_ERR_INVALID;
    }

    openai_llm_ctx_t* ctx =
        (openai_llm_ctx_t*)calloc(1, sizeof(*ctx));
    if (!ctx) {
        return AEGIS_ERR_NOMEM;
    }

    /* ops are file-scope const, owned by registry, not freed by destroy */
    static const aegis_llm_ops_t ops = {
        .complete = openai_llm_complete,
    };

    aegis_provider_def_t def = {
        .name         = "llm-openai",
        .description  = "OpenAI-compatible Chat Completions provider (gpt-4, gpt-3.5-turbo, ...)",
        .abi_version  = AEGIS_PROVIDER_ABI_VERSION,
        .kind         = AEGIS_PROVIDER_LLM,
        .capabilities = AEGIS_CAP_NONE,
        .thread_model = AEGIS_PROVIDER_SINGLE_THREAD,
        .init         = openai_llm_init,
        .shutdown     = openai_llm_shutdown,
        .user         = ctx,
    };

    *out_ctx = ctx;
    *out_ops = &ops;
    *out_def = def;
    return AEGIS_OK;
}

void aegis_openai_llm_destroy(openai_llm_ctx_t* ctx, const aegis_llm_ops_t* ops)
{
    if (!ctx) {
        return;
    }
    free(ctx->api_key);
    free(ctx->base_url);
    free(ctx->model);
    free(ctx);
    (void)ops;
}

void aegis_openai_llm_configure(openai_llm_ctx_t* ctx,
                                const char* api_key,
                                const char* base_url,
                                const char* model)
{
    if (!ctx) {
        return;
    }
    free(ctx->api_key);
    free(ctx->base_url);
    free(ctx->model);
    ctx->api_key  = api_key  ? strdup(api_key)  : NULL;
    ctx->base_url = base_url ? strdup(base_url) : NULL;
    ctx->model    = model    ? strdup(model)    : NULL;
}
