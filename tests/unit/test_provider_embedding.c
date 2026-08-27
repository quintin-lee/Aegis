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

static void test_embedding_hash_basic(void)
{
    hash_embed_ctx_t*            ctx = NULL;
    const aegis_embedding_ops_t* ops = NULL;
    aegis_provider_def_t         def = {0};
    expect_ok(aegis_embedding_hash_create(&ctx, &ops, &def), "create");
    assert(ctx != NULL);
    assert(ops != NULL);
    assert(strcmp(def.name, "embedding-hash") == 0);
    assert(def.kind == AEGIS_PROVIDER_EMBEDDING);
    assert(def.abi_version == AEGIS_PROVIDER_ABI_VERSION);

    aegis_provider_registry_t* reg = NULL;
    expect_ok(aegis_provider_registry_create(&reg), "create reg");
    expect_ok(aegis_provider_register(reg, &def), "register");
    assert(aegis_provider_count(reg) == 1);
    expect_ok(aegis_provider_init(reg, "embedding-hash"), "init");

    aegis_cancellation_token_t* token = NULL;
    expect_ok(aegis_cancellation_token_create(&token), "token");

    aegis_embedding_result_t res = {0};
    expect_ok(aegis_embed(reg, "embedding-hash", "hello world", 11, token, &res), "embed");
    assert(res.vector != NULL);
    assert(res.dim == 64);

    float norm = 0.0f;
    for (size_t i = 0; i < res.dim; i++) {
        norm += res.vector[i] * res.vector[i];
    }
    norm = sqrtf(norm);
    assert(fabs(norm - 1.0) < 1e-6);

    aegis_embedding_result_destroy(&res);
    aegis_cancellation_token_destroy(token);
    aegis_provider_shutdown(reg, "embedding-hash");
    aegis_provider_registry_destroy(reg);
    aegis_embedding_hash_destroy(ctx, ops);
}

static void test_embedding_hash_empty(void)
{
    hash_embed_ctx_t*            ctx = NULL;
    const aegis_embedding_ops_t* ops = NULL;
    aegis_provider_def_t         def = {0};
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
    for (size_t i = 0; i < res.dim; i++) {
        assert(res.vector[i] == 0.0f);
    }
            aegis_embedding_result_destroy(&res);

    aegis_cancellation_token_destroy(token);
    aegis_provider_unregister(reg, "embedding-hash");
    aegis_embedding_hash_destroy(ctx, ops);
    aegis_provider_registry_destroy(reg);
}

static void test_embedding_hash_different_inputs(void)
{
    hash_embed_ctx_t*            ctx = NULL;
    const aegis_embedding_ops_t* ops = NULL;
    aegis_provider_def_t         def = {0};
    expect_ok(aegis_embedding_hash_create(&ctx, &ops, &def), "create");

    aegis_provider_registry_t* reg = NULL;
    expect_ok(aegis_provider_registry_create(&reg), "create reg");
    expect_ok(aegis_provider_register(reg, &def), "register");
    expect_ok(aegis_provider_init(reg, "embedding-hash"), "init");

    aegis_cancellation_token_t* token = NULL;
    expect_ok(aegis_cancellation_token_create(&token), "token");

    aegis_embedding_result_t res1 = {0};
    aegis_embedding_result_t res2 = {0};
    expect_ok(aegis_embed(reg, "embedding-hash", "test one", 8, token, &res1), "embed 1");
    expect_ok(aegis_embed(reg, "embedding-hash", "test two", 8, token, &res2), "embed 2");
    /* Different inputs should produce different vectors. */
    assert(res1.dim == res2.dim);
    bool different = false;
    for (size_t i = 0; i < res1.dim; i++) {
        if (res1.vector[i] != res2.vector[i]) {
            different = true;
            break;
        }
    }
    assert(different);

    aegis_embedding_result_destroy(&res1);
    aegis_embedding_result_destroy(&res2);
    aegis_cancellation_token_destroy(token);
    aegis_provider_shutdown(reg, "embedding-hash");
    aegis_provider_registry_destroy(reg);
    aegis_embedding_hash_destroy(ctx, ops);
}

static void test_embedding_invalid_args(void)
{
    hash_embed_ctx_t*            ctx = NULL;
    const aegis_embedding_ops_t* ops = NULL;
    aegis_provider_def_t         def = {0};
    expect_ok(aegis_embedding_hash_create(&ctx, &ops, &def), "create");

    aegis_provider_registry_t* reg = NULL;
    expect_ok(aegis_provider_registry_create(&reg), "create reg");
    aegis_cancellation_token_t* token = NULL;
    expect_ok(aegis_cancellation_token_create(&token), "token");

    aegis_embedding_result_t res = {0};
    assert(aegis_embed(NULL, "x", "test", 4, token, &res) == AEGIS_ERR_INVALID);
    assert(aegis_embed(reg, NULL, "test", 4, token, &res) == AEGIS_ERR_INVALID);
    assert(aegis_embed(reg, "x", NULL, 4, token, &res) == AEGIS_ERR_INVALID);

    aegis_embedding_hash_destroy(ctx, ops);
    aegis_cancellation_token_destroy(token);
    aegis_provider_registry_destroy(reg);
}

int main(void)
{
    test_embedding_invalid_args();
    test_embedding_hash_basic();
    test_embedding_hash_empty();
    test_embedding_hash_different_inputs();

    printf("provider_embedding: all tests passed");
    return 0;
}
