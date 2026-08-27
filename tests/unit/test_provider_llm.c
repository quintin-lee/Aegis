/**
 * @file test_provider_llm.c
 * @brief Unit tests for the mock LLM provider.
 */
#include "aegis/llm.h"
#include "aegis/provider_llm_mock.h"
#include "aegis/provider.h"
#include "aegis/cancellation.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void expect_ok(aegis_status_t rc, const char* msg)
{
    assert(rc == AEGIS_OK);
    (void)msg;
}

/* ── Tests ─────────────────────────────────────────────────────────────────── */

static void test_llm_mock_basic(void)
{
    llm_mock_ctx_t* ctx = NULL;
    const aegis_llm_ops_t* ops = NULL;
    aegis_provider_def_t def = {0};
    expect_ok(aegis_llm_mock_create(&ctx, &ops, &def), "create");
    assert(ctx != NULL);
    assert(ops != NULL);
    assert(strcmp(def.name, "llm-mock") == 0);
    assert(def.kind == AEGIS_PROVIDER_LLM);
    assert(def.abi_version == AEGIS_PROVIDER_ABI_VERSION);

    /* Create registry and register. */
    aegis_provider_registry_t* reg = NULL;
    expect_ok(aegis_provider_registry_create(&reg), "create reg");
    expect_ok(aegis_provider_register(reg, &def), "register");
    assert(aegis_provider_count(reg) == 1);

    /* Init. */
    expect_ok(aegis_provider_init(reg, "llm-mock"), "init");

    /* Dispatch. */
    aegis_llm_request_t req = {0};
    req.prompt = "Hello world";
    req.prompt_len = strlen("Hello world");
    req.max_tokens = 100;
    req.temperature = 0.7f;

    aegis_llm_response_t resp = {0};
    aegis_cancellation_token_t* token = NULL;
    expect_ok(aegis_cancellation_token_create(&token), "token");

    expect_ok(aegis_llm_complete(reg, "llm-mock", &req, token, &resp), "complete");
    assert(resp.data != NULL);
    assert(resp.len > 0);
    assert(strncmp((char*)resp.data, "Mock: ", 6) == 0);
    assert(strstr((char*)resp.data, "Hello world") != NULL);

    aegis_llm_response_destroy(&resp);
    aegis_cancellation_token_destroy(token);
    aegis_provider_registry_destroy(reg);
}

static void test_llm_mock_cancelled(void)
{
    llm_mock_ctx_t* ctx = NULL;
    const aegis_llm_ops_t* ops = NULL;
    aegis_provider_def_t def = {0};
    expect_ok(aegis_llm_mock_create(&ctx, &ops, &def), "create");

    aegis_provider_registry_t* reg = NULL;
    expect_ok(aegis_provider_registry_create(&reg), "create reg");
    expect_ok(aegis_provider_register(reg, &def), "register");
    expect_ok(aegis_provider_init(reg, "llm-mock"), "init");

    /* Create cancelled token. */
    aegis_cancellation_token_t* token = NULL;
    expect_ok(aegis_cancellation_token_create(&token), "token");
    aegis_cancellation_token_request_cancel(token);

    aegis_llm_request_t req = {"cancel test", 11, 100, 0.5f};
    aegis_llm_response_t resp = {0};
    assert(aegis_llm_complete(reg, "llm-mock", &req, token, &resp) == AEGIS_ERR_CANCELLED);
    assert(resp.data == NULL);

    aegis_cancellation_token_destroy(token);
    aegis_provider_registry_destroy(reg);
}

static void test_llm_mock_not_found(void)
{
    aegis_provider_registry_t* reg = NULL;
    expect_ok(aegis_provider_registry_create(&reg), "create reg");

    aegis_llm_request_t req = {"test", 4, 0, 0.0f};
    aegis_llm_response_t resp = {0};
    aegis_cancellation_token_t* token = NULL;
    expect_ok(aegis_cancellation_token_create(&token), "token");

    assert(aegis_llm_complete(reg, "nonexistent", &req, token, &resp) == AEGIS_ERR_NOT_FOUND);

    aegis_cancellation_token_destroy(token);
    aegis_provider_registry_destroy(reg);
    aegis_llm_mock_destroy(ctx, ops);
}

static void test_llm_mock_uninitialized(void)
{
    llm_mock_ctx_t* ctx = NULL;
    const aegis_llm_ops_t* ops = NULL;
    aegis_provider_def_t def = {0};
    expect_ok(aegis_llm_mock_create(&ctx, &ops, &def), "create");

    aegis_provider_registry_t* reg = NULL;
    expect_ok(aegis_provider_registry_create(&reg), "create reg");
    expect_ok(aegis_provider_register(reg, &def), "register");
    /* Do NOT init — should get PERMISSION error. */

    aegis_llm_request_t req = {"test", 4, 0, 0.0f};
    aegis_llm_response_t resp = {0};
    aegis_cancellation_token_t* token = NULL;
    expect_ok(aegis_cancellation_token_create(&token), "token");

    assert(aegis_llm_complete(reg, "llm-mock", &req, token, &resp) == AEGIS_ERR_PERM);

    aegis_cancellation_token_destroy(token);
    aegis_provider_registry_destroy(reg);
    aegis_llm_mock_destroy(ctx, ops);
}

static void test_llm_mock_invalid_args(void)
{
    llm_mock_ctx_t* ctx = NULL;
    const aegis_llm_ops_t* ops = NULL;
    aegis_provider_def_t def = {0};
    expect_ok(aegis_llm_mock_create(&ctx, &ops, &def), "create");

    aegis_provider_registry_t* reg = NULL;
    expect_ok(aegis_provider_registry_create(&reg), "create reg");
    aegis_cancellation_token_t* token = NULL;
    expect_ok(aegis_cancellation_token_create(&token), "token");

    aegis_llm_response_t resp = {0};
    assert(aegis_llm_complete(NULL, "x", NULL, token, &resp) == AEGIS_ERR_INVALID);
    assert(aegis_llm_complete(reg, NULL, NULL, token, &resp) == AEGIS_ERR_INVALID);

    aegis_cancellation_token_destroy(token);
    aegis_provider_registry_destroy(reg);
    aegis_llm_mock_destroy(ctx, ops);
}

/* ── Main ──────────────────────────────────────────────────────────────────── */

int main(void)
{
    test_llm_mock_invalid_args();
    test_llm_mock_not_found();
    test_llm_mock_uninitialized();
    test_llm_mock_basic();
    test_llm_mock_cancelled();

    printf("provider_llm: all tests passed\n");
    return 0;
}
