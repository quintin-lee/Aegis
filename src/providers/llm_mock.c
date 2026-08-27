/**
 * @file llm_mock.c
 * @brief Mock LLM provider for testing and local development.
 *
 * This provider does NOT make network calls. It simulates LLM completion
 * by echoing the prompt with a deterministic prefix. Use this for:
 *   - Unit testing without external dependencies
 *   - Local development when no API key is available
 *   - Integration testing with controlled output
 *
 * Configuration (via init user context):
 *   - response_prefix: string prepended to echoed prompt (default: "Mock: ")
 *   - delay_ms: artificial delay in milliseconds (default: 0)
 *   - fail_after: if non-zero, fail on the Nth call (0 = never fail)
 *
 * Thread model: SINGLE_THREAD (not safe for concurrent dispatch)
 */
#define _POSIX_C_SOURCE 200809L
#include "aegis/llm.h"
#include "aegis/status.h"
#include "../internal/lifecycle.h"
#include "aegis/cancellation.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ── Provider context ──────────────────────────────────────────────────────── */

typedef struct llm_mock_ctx {
    char* response_prefix; /**< Owned. */
    int   delay_ms;        /**< Artificial delay. */
    int   call_count;      /**< Number of calls made. */
    int   fail_after;      /**< 0 = never fail; N = fail on Nth call. */
} llm_mock_ctx_t;

/* ── Helpers ───────────────────────────────────────────────────────────────── */

static void sleep_ms(int ms)
{
    if (ms <= 0) {
        return;
    }
    struct timespec ts;
    ts.tv_sec  = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

static aegis_status_t check_cancel(const aegis_cancellation_token_t* token)
{
    if (token && aegis_cancellation_token_is_cancelled(token)) {
        return AEGIS_ERR_CANCELLED;
    }
    return AEGIS_OK;
}

/* ── Init / Shutdown ───────────────────────────────────────────────────────── */

static aegis_status_t llm_mock_init(void* user)
{
    /* user is the ctx — already allocated by caller. */
    (void)user;
    return AEGIS_OK;
}

static void llm_mock_shutdown(void* user)
{
    /* Caller (registry) owns ctx; this is a no-op. */
    (void)user;
}

/* ── Completion callback ───────────────────────────────────────────────────── */

static aegis_status_t llm_mock_complete(void* ctx, const aegis_llm_request_t* req,
                                        const aegis_cancellation_token_t* token,
                                        aegis_llm_response_t*             out)
{
    llm_mock_ctx_t* mock = (llm_mock_ctx_t*)ctx;
    if (!mock || !req || !out) {
        return AEGIS_ERR_INVALID;
    }

    /* Check cancellation before and during processing. */
    aegis_status_t rc = check_cancel(token);
    if (rc != AEGIS_OK) {
        return rc;
    }

    /* Simulate delay. */
    sleep_ms(mock->delay_ms);

    /* Check cancellation again after delay. */
    rc = check_cancel(token);
    if (rc != AEGIS_OK) {
        return rc;
    }

    /* Simulate failure after N calls. */
    mock->call_count++;
    if (mock->fail_after > 0 && mock->call_count >= mock->fail_after) {
        return AEGIS_ERR_PROVIDER;
    }

    /* Build response: prefix + prompt. */
    const char* prefix     = mock->response_prefix ? mock->response_prefix : "Mock: ";
    size_t      prefix_len = strlen(prefix);
    size_t      prompt_len = req->prompt_len;
    const char* prompt     = (const char*)req->prompt;

    /* Allocate response buffer. */
    size_t total_len = prefix_len + prompt_len;
    char*  buf       = malloc(total_len + 1);
    if (!buf) {
        return AEGIS_ERR_NOMEM;
    }

    memcpy(buf, prefix, prefix_len);
    if (prompt_len > 0 && prompt) {
        memcpy(buf + prefix_len, prompt, prompt_len);
    }
    buf[total_len] = '\0';

    out->data = buf;
    out->len  = total_len;
    return AEGIS_OK;
}

/* ── Factory ───────────────────────────────────────────────────────────────── */

aegis_status_t aegis_llm_mock_create(llm_mock_ctx_t** out_ctx, const aegis_llm_ops_t** out_ops,
                                     aegis_provider_def_t* out_def)
{
    AEGIS_CHECK_OUT(out_ctx);
    AEGIS_CHECK_OUT(out_ops);
    AEGIS_CHECK_OUT(out_def);

    /* Allocate context. */
    llm_mock_ctx_t* ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        return AEGIS_ERR_NOMEM;
    }
    ctx->response_prefix = strdup("Mock: ");
    ctx->delay_ms        = 0;
    ctx->call_count      = 0;
    ctx->fail_after      = 0;
    if (!ctx->response_prefix) {
        free(ctx);
        return AEGIS_ERR_NOMEM;
    }

    /* Allocate ops struct (file-scope const-like, but we need heap for user). */
    aegis_llm_ops_t* ops = malloc(sizeof(*ops));
    if (!ops) {
        free(ctx->response_prefix);
        free(ctx);
        return AEGIS_ERR_NOMEM;
    }
    ops->ctx      = ctx;
    ops->complete = llm_mock_complete;

    /* Allocate provider def. */
    aegis_provider_def_t* def = malloc(sizeof(*def));
    if (!def) {
        free(ops);
        free(ctx->response_prefix);
        free(ctx);
        return AEGIS_ERR_NOMEM;
    }
    def->name         = "llm-mock";
    def->description  = "Mock LLM provider for testing";
    def->abi_version  = AEGIS_PROVIDER_ABI_VERSION;
    def->kind         = AEGIS_PROVIDER_LLM;
    def->capabilities = AEGIS_CAP_NONE;
    def->thread_model = AEGIS_PROVIDER_SINGLE_THREAD;
    def->init         = llm_mock_init;
    def->shutdown     = llm_mock_shutdown;
    def->user         = ops; /* ops is borrowed; it lives as long as the registry entry. */

    *out_ctx = ctx;
    *out_ops = ops;
    *out_def = *def;
    free(def); /* We only needed to allocate for the shallow copy; def is now copied. */

    return AEGIS_OK;
}

void aegis_llm_mock_set_fail_after(llm_mock_ctx_t* ctx, int fail_after)
{
    if (ctx) {
        ctx->fail_after = fail_after;
    }
}

void aegis_llm_mock_destroy(llm_mock_ctx_t* ctx, const aegis_llm_ops_t* ops)
{
    free((void*)ops);
    if (!ctx) {
        return;
    }
    free(ctx->response_prefix);
    free(ctx);
}
