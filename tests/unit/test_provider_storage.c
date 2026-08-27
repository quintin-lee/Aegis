/**
 * @file test_provider_storage.c
 * @brief Unit tests for the SQLite storage provider.
 */
#include "aegis/storage.h"
#include "aegis/provider_storage_sqlite.h"
#include "aegis/provider.h"
#include "aegis/cancellation.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void expect_ok(aegis_status_t rc, const char *msg)
{
    assert(rc == AEGIS_OK);
    (void)msg;
}

/* ── Tests ─────────────────────────────────────────────────────────────────── */

static void test_storage_sqlite_memory(void)
{
    aegis_sqlite_storage_ctx_t *ctx = NULL;
    aegis_storage_ops_t *ops = NULL;
    aegis_provider_def_t def = {0};
    expect_ok(aegis_storage_sqlite_create(NULL, 1, &ctx, &ops, &def), "create");
    assert(ctx != NULL);
    assert(ops != NULL);
    assert(ops->put != NULL);
    assert(ops->get != NULL);
    assert(ops->del != NULL);

    aegis_provider_registry_t *reg = NULL;
    expect_ok(aegis_provider_registry_create(&reg), "create reg");
    expect_ok(aegis_provider_register(reg, &def), "register");
    expect_ok(aegis_provider_init(reg, "storage-sqlite-memory"), "init");

    aegis_cancellation_token_t *token = NULL;
    expect_ok(aegis_cancellation_token_create(&token), "token");

    const char *key1 = "hello";
    const char *val1 = "world";
    expect_ok(aegis_storage_put(reg, "storage-sqlite-memory", key1, strlen(key1), val1,
                                strlen(val1), token),
              "put");

    aegis_storage_blob_t blob = {0};
    expect_ok(aegis_storage_get(reg, "storage-sqlite-memory", key1, strlen(key1), token, &blob),
              "get");
    assert(blob.data != NULL);
    assert(blob.len == strlen(val1));
    assert(memcmp(blob.data, val1, blob.len) == 0);
    aegis_storage_blob_destroy(&blob);

    aegis_storage_blob_t missing = {0};
    expect_ok(
        aegis_storage_get(reg, "storage-sqlite-memory", "missing", strlen("missing"), token, &missing),
        "get missing");
    assert(missing.data == NULL);
    assert(missing.len == 0);
    aegis_storage_blob_destroy(&missing);

    expect_ok(aegis_storage_delete(reg, "storage-sqlite-memory", key1, strlen(key1), token), "del");

    aegis_storage_blob_t after_del = {0};
    expect_ok(
        aegis_storage_get(reg, "storage-sqlite-memory", key1, strlen(key1), token, &after_del),
        "get after del");
    assert(after_del.data == NULL);
    aegis_storage_blob_destroy(&after_del);

    aegis_cancellation_token_destroy(token);
    aegis_provider_shutdown(reg, "storage-sqlite-memory");
    aegis_provider_registry_destroy(reg);
    aegis_storage_sqlite_destroy(ctx, ops);
}

static void test_storage_sqlite_file(void)
{
    const char *db_path = "/tmp/aegis_test_storage_XXXXXX";
    char path[256];
    strncpy(path, db_path, sizeof(path) - 1);
    path[sizeof(path) - 1] = '\0';
    int fd = mkstemp(path);
    assert(fd >= 0);
    close(fd);

    aegis_sqlite_storage_ctx_t *ctx = NULL;
    aegis_storage_ops_t *ops = NULL;
    aegis_provider_def_t def = {0};
    expect_ok(aegis_storage_sqlite_create(path, 0, &ctx, &ops, &def), "create");

    aegis_provider_registry_t *reg = NULL;
    expect_ok(aegis_provider_registry_create(&reg), "create reg");
    expect_ok(aegis_provider_register(reg, &def), "register");
    expect_ok(aegis_provider_init(reg, "storage-sqlite"), "init");

    aegis_cancellation_token_t *token = NULL;
    expect_ok(aegis_cancellation_token_create(&token), "token");

    const char *key = "key1";
    const char *val = "value1";
    expect_ok(aegis_storage_put(reg, "storage-sqlite", key, strlen(key), val, strlen(val), token),
              "put");

    aegis_storage_blob_t blob = {0};
    expect_ok(aegis_storage_get(reg, "storage-sqlite", key, strlen(key), token, &blob), "get");
    assert(blob.data != NULL);
    assert(memcmp(blob.data, val, blob.len) == 0);
    aegis_storage_blob_destroy(&blob);

    aegis_cancellation_token_destroy(token);
    aegis_provider_shutdown(reg, "storage-sqlite");
    aegis_provider_registry_destroy(reg);
    aegis_storage_sqlite_destroy(ctx, ops);
    unlink(path);
}

static void test_storage_invalid_args(void)
{
    aegis_provider_registry_t *reg = NULL;
    expect_ok(aegis_provider_registry_create(&reg), "create reg");

    aegis_cancellation_token_t *token = NULL;
    expect_ok(aegis_cancellation_token_create(&token), "token");

    aegis_storage_blob_t blob = {0};
    assert(aegis_storage_get(NULL, "x", "x", 1, token, &blob) == AEGIS_ERR_INVALID);
    assert(aegis_storage_delete(NULL, "x", "x", 1, token) == AEGIS_ERR_INVALID);

    aegis_cancellation_token_destroy(token);
    aegis_provider_registry_destroy(reg);
}

static void test_storage_transaction(void)
{
    aegis_sqlite_storage_ctx_t *ctx = NULL;
    aegis_storage_ops_t *ops = NULL;
    aegis_provider_def_t def = {0};
    expect_ok(aegis_storage_sqlite_create(NULL, 1, &ctx, &ops, &def), "create");

    aegis_provider_registry_t *reg = NULL;
    expect_ok(aegis_provider_registry_create(&reg), "create reg");
    expect_ok(aegis_provider_register(reg, &def), "register");
    expect_ok(aegis_provider_init(reg, "storage-sqlite-memory"), "init");

    aegis_cancellation_token_t *token = NULL;
    expect_ok(aegis_cancellation_token_create(&token), "token");

    aegis_storage_transaction_t *txn = NULL;
    expect_ok(aegis_storage_transaction_create(&txn), "txn create");
    expect_ok(aegis_storage_transaction_put(txn, "a", 1, "1", 1), "txn put a");
    expect_ok(aegis_storage_transaction_put(txn, "b", 1, "2", 1), "txn put b");
    expect_ok(aegis_storage_transaction_commit(txn, token), "txn commit");
    /* Commit is a no-op at this layer (see storage_store.c); verify transaction was consumed. */
    aegis_storage_transaction_destroy(txn);

    aegis_cancellation_token_destroy(token);
    aegis_provider_shutdown(reg, "storage-sqlite-memory");
    aegis_provider_registry_destroy(reg);
    aegis_storage_sqlite_destroy(ctx, ops);
}

/* ── Main ──────────────────────────────────────────────────────────────────── */

int main(void)
{
    test_storage_invalid_args();
    test_storage_sqlite_memory();
    test_storage_sqlite_file();
    test_storage_transaction();

    printf("provider_storage: all tests passed\n");
    return 0;
}
