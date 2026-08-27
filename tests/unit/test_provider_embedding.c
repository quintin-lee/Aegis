/**
 * @file test_provider_embedding.c
 * @brief Unit tests for the hash-based embedding provider.
 */
#include "aegis/embedding.h"
#include "aegis/provider_embedding_hash.h"
#include "aegis/provider.h"
#include "aegis/cancellation.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static void expect_ok(aegis_status_t rc, const char* msg)
{
    assert(rc == AEGIS_OK);
    (void)msg;
}

/* ── Tests ─────────────────────────────────────────────────────────────────── */

static void test_embedding_hash_basic(void)
{
    hash_embed_ctx_t* ctx = NULL;
    const aegis_embedding_ops_t* ops = NULL;
    aegis_provider_def_t def = {0};
    expect_ok(aegis_embedding_hash_create(&ctx, &ops, &def), "create");
    assert(ctx != NULL);
    assert(ops != NULL);
    assert(strcmp(def.name, "embedding-hash") == 0);
    assert(def.kind == AEGIS_PROVIDER_EMBEDDING);

    aegis_provider_registry_t* reg = NULL;
    expect_ok(aegis_provider_registry_create(&reg), "create reg");
    expect_ok(aegis_provider_register(reg, &def), "register");
    expect_ok(aegis_provider_init(reg, "embedding-hash"), "init");

    aegis_cancellation_token_t* token = NULL;
    expect_ok(aegis_cancellation_token_create(&token), "token");

    /* Embed. */
    aegis_embedding_result_t res = {0};
    expect_ok(aegis_embed(reg, "embedding-hash", "hello world", strlen("hello world"), token, &res), "embed");
    assert(res.vector != NULL);
    assert(res.dim == 64);

    /* Check determinism. */
    aegis_embedding_result_t res2 = {0};
    expect_ok(aegis_embed(reg, "embedding-hash", "hello world", strlen("hello world"), token, &res2), "embed again");
    assert(res2.vector != NULL);
    assert(res2.dim == res.dim);
    assert(memcmp(res.vector, res2.vector, res.dim * sizeof(float)) == 0);

    /* Check normalization (unit length). */
    double norm = 0.0;
    for (size_t i = 0; i < res.dim; i++)
        norm += (double)res.vector[i] * (double)res.vector[i];
    assert(fabs(norm - 1.0) < 1e-6);

    aegis_embedding_result_destroy(&res);
    aegis_embedding_result_destroy(&res2);
    aegis_cancellation_token_destroy(token);
    aegis_provider_shutdown(reg, "embedding-hash");
    aegis_provider_registry_destroy(reg);
    aegis_embedding_hash_destroy(ctx, ops);
}

static void test_embedding_hash_empty(void)
{
    hash_embed_ctx_t* ctx = NULL;
    const aegis_embedding_ops_t* ops = NULL;
    aegis_provider_def_t def = {0};
    expect_ok(aegis_embedding_hash_create(&ctx, &ops, &def), "create");

    aegis_provider_registry_t* reg = NULL;
    expect_ok(aegis_provider_registry_create(&reg), "create reg");
    expect_ok(aegis_provider_register(reg, &def), "register");
    expect_ok(aegis_provider_init(reg, "embedding-hash"), "init");

    aegis_cancellation_token_t* token = NULL;
    expect_ok(aegis_cancellation_token_create(&token), "token");

    aegis_embedding_result_t res = {0};
    expect_ok(aegis_embed(reg, "embedding-hash", "", 0, token, &res), "embed empty");
    assert(res.vector != NULL);
    assert(res.dim == 64);
    /* Empty string should produce zero vector (all zeros). */
    for (size_t i = 0; i < res.dim; i++)
        assert(res.vector[i] == 0.0f);
    aegis_embedding_result_destroy(&res);

    aegis_cancellation_token_destroy(token);
    aegis_provider_shutdown(reg, "embedding-hash");
    aegis_provider_registry_destroy(reg);
    aegis_embedding_hash_destroy(ctx, ops);
}

static void test_embedding_hash_different_inputs(void)
{
    hash_embed_ctx_t* ctx = NULL;
    const aegis_embedding_ops_t* ops = NULL;
    aegis_provider_def_t def = {0};
    expect_ok(aegis_embedding_hash_create(&ctx, &ops, &def), "create");

    aegis_provider_registry_t* reg = NULL;
    expect_ok(aegis_provider_registry_create(&reg), "create reg");
    expect_ok(aegis_provider_register(reg, &def), "register");
    expect_ok(aegis_provider_init(reg, "embedding-hash"), "init");

    aegis_cancellation_token_t* token = NULL;
    expect_ok(aegis_cancellation_token_create(&token), "token");

    aegis_embedding_result_t r1 = {0}, r2 = {0};
    expect_ok(aegis_embed(reg, "embedding-hash", "foo", 3, token, &r1), "embed foo");
    expect_ok(aegis_embed(reg, "embedding-hash", "bar", 3, token, &r2), "embed bar");

    /* Different inputs should produce different vectors. */
    assert(memcmp(r1.vector, r2.vector, r1.dim * sizeof(float)) != 0);

    aegis_embedding_result_destroy(&r1);
    aegis_embedding_result_destroy(&r2);
    aegis_cancellation_token_destroy(token);
    aegis_provider_shutdown(reg, "embedding-hash");
    aegis_provider_registry_destroy(reg);
    aegis_embedding_hash_destroy(ctx, ops);
}

static void test_embedding_invalid_args(void)
{
    aegis_provider_registry_t* reg = NULL;
    expect_ok(aegis_provider_registry_create(&reg), "create reg");
    aegis_cancellation_token_t* token = NULL;
    expect_ok(aegis_cancellation_token_create(&token), "token");

    aegis_embedding_result_t res = {0};
    assert(aegis_embed(NULL, "x", "test", 4, token, &res) == AEGIS_ERR_INVALID);
    assert(aegis_embed(reg, NULL, "test", 4, token, &res) == AEGIS_ERR_INVALID);
    assert(aegis_embed(reg, "x", NULL, 4, token, &res) == AEGIS_ERR_INVALID);

    aegis_cancellation_token_destroy(token);
    aegis_provider_registry_destroy(reg);
}

/* ── Main ──────────────────────────────────────────────────────────────────── */

int main(void)
{
    test_embedding_invalid_args();
    test_embedding_hash_basic();
    test_embedding_hash_empty();
    test_embedding_hash_different_inputs();

    printf("provider_embedding: all tests passed\n");
    return 0;
}
