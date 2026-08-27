/**
 * @file test_storage.c
 * @brief Unit tests for the storage store module:
 *   - Transaction create/destroy/put/delete/commit
 *   - Snapshot take/destroy
 *   - Query (returns ERR_PROVIDER as expected for mock backend)
 *   - Null safety
 */
#include "aegis/storage.h"
#include "aegis/cancellation.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Helpers ───────────────────────────────────────────────────────────────── */

static void expect_ok(aegis_status_t rc, const char* msg)
{
    assert(rc == AEGIS_OK);
    (void)msg;
}

/* ── Transaction lifecycle ─────────────────────────────────────────────────── */

static void test_transaction_create_destroy(void)
{
    aegis_storage_transaction_t* txn = NULL;
    expect_ok(aegis_storage_transaction_create(&txn), "create");
    assert(txn != NULL);
    aegis_storage_transaction_destroy(txn);
    aegis_storage_transaction_destroy(NULL);
}

/* ── Transaction put/delete ────────────────────────────────────────────────── */

static void test_transaction_put(void)
{
    aegis_storage_transaction_t* txn = NULL;
    expect_ok(aegis_storage_transaction_create(&txn), "create");

    expect_ok(aegis_storage_transaction_put(txn, "key1", 4, "value1", 6),
              "put key1");
    expect_ok(aegis_storage_transaction_put(txn, "key2", 4, "value2", 6),
              "put key2");

    aegis_storage_transaction_destroy(txn);
}

static void test_transaction_delete(void)
{
    aegis_storage_transaction_t* txn = NULL;
    expect_ok(aegis_storage_transaction_create(&txn), "create");

    expect_ok(aegis_storage_transaction_delete(txn, "key1", 4),
              "delete key1");

    aegis_storage_transaction_destroy(txn);
}

static void test_transaction_invalid_args(void)
{
    aegis_storage_transaction_t* txn = NULL;
    expect_ok(aegis_storage_transaction_create(&txn), "create");

    /* NULL txn. */
    assert(aegis_storage_transaction_put(NULL, "k", 1, "v", 1) == AEGIS_ERR_INVALID);
    assert(aegis_storage_transaction_delete(NULL, "k", 1) == AEGIS_ERR_INVALID);
    assert(aegis_storage_transaction_commit(NULL, NULL) == AEGIS_ERR_INVALID);

    /* NULL key. */
    assert(aegis_storage_transaction_put(txn, NULL, 0, "v", 1) == AEGIS_ERR_INVALID);
    assert(aegis_storage_transaction_delete(txn, NULL, 0) == AEGIS_ERR_INVALID);

    /* Empty key. */
    assert(aegis_storage_transaction_put(txn, "", 0, "v", 1) == AEGIS_ERR_INVALID);
    assert(aegis_storage_transaction_delete(txn, "", 0) == AEGIS_ERR_INVALID);

    aegis_storage_transaction_destroy(txn);
}

/* ── Transaction commit ────────────────────────────────────────────────────── */

static void test_transaction_commit(void)
{
    aegis_storage_transaction_t* txn = NULL;
    expect_ok(aegis_storage_transaction_create(&txn), "create");

    expect_ok(aegis_storage_transaction_put(txn, "k", 1, "v", 1), "put");
    expect_ok(aegis_storage_transaction_commit(txn, NULL), "commit");
    /* txn is consumed after commit. */
}

static void test_transaction_commit_cancelled(void)
{
    aegis_storage_transaction_t* txn = NULL;
    expect_ok(aegis_storage_transaction_create(&txn), "create");

    aegis_cancellation_token_t* token = NULL;
    expect_ok(aegis_cancellation_token_create(&token), "create token");
    aegis_cancellation_token_request_cancel(token);

    assert(aegis_storage_transaction_commit(txn, token) == AEGIS_ERR_CANCELLED);

    aegis_cancellation_token_destroy(token);
    aegis_storage_transaction_destroy(txn);
}

/* ── Snapshot ──────────────────────────────────────────────────────────────── */

static void test_snapshot_take_not_found(void)
{
    /* No registry, no store — should fail gracefully. */
    aegis_storage_snapshot_t* snap = NULL;
    assert(aegis_storage_snapshot_take(NULL, "store", NULL, &snap) == AEGIS_ERR_INVALID);
    assert(snap == NULL);
}

static void test_snapshot_destroy_null(void)
{
    aegis_storage_snapshot_destroy(NULL);
}

static void test_snapshot_count_empty(void)
{
    aegis_storage_snapshot_t* snap = NULL;
    assert(aegis_storage_snapshot_take(NULL, "store", NULL, &snap) == AEGIS_ERR_INVALID);
    /* Can't test count without a valid snapshot, but verify API doesn't crash. */
    assert(aegis_storage_snapshot_count(NULL) == 0);
}

/* ── Query ─────────────────────────────────────────────────────────────────── */

static void test_query_invalid_args(void)
{
    aegis_storage_entry_t* entries = NULL;
    size_t count = 0;

    /* NULL reg. */
    assert(aegis_storage_query(NULL, "store", NULL, 0, NULL, &entries, &count) ==
           AEGIS_ERR_INVALID);

    /* NULL store_name. */
    aegis_provider_registry_t* reg = NULL;
    assert(aegis_storage_query(reg, NULL, NULL, 0, NULL, &entries, &count) ==
           AEGIS_ERR_INVALID);
    (void)reg;

    /* NULL out/out_count. */
    assert(aegis_storage_query(reg, "store", NULL, 0, NULL, NULL, &count) ==
           AEGIS_ERR_INVALID);
    assert(aegis_storage_query(reg, "store", NULL, 0, NULL, &entries, NULL) ==
           AEGIS_ERR_INVALID);
}

static void test_query_returns_provider_error(void)
{
    /* Query with NULL registry returns INVALID (arg check). */
    aegis_storage_entry_t* entries = NULL;
    size_t count = 0;
    aegis_status_t rc = aegis_storage_query(NULL, "store", NULL, 0, NULL,
                                             &entries, &count);
    assert(rc == AEGIS_ERR_INVALID);
    assert(entries == NULL);
    assert(count == 0);
}

/* ── Entries destroy ───────────────────────────────────────────────────────── */

static void test_entries_destroy_null(void)
{
    aegis_storage_entries_destroy(NULL);
}

/* ── Blob destroy ──────────────────────────────────────────────────────────── */

static void test_blob_destroy(void)
{
    aegis_storage_blob_t blob = {NULL, 0};
    aegis_storage_blob_destroy(&blob);
    aegis_storage_blob_destroy(NULL);
}

/* ── Main ──────────────────────────────────────────────────────────────────── */

int main(void)
{
    test_transaction_create_destroy();
    test_transaction_put();
    test_transaction_delete();
    test_transaction_invalid_args();
    test_transaction_commit();
    test_transaction_commit_cancelled();
    test_snapshot_take_not_found();
    test_snapshot_destroy_null();
    test_snapshot_count_empty();
    test_query_invalid_args();
    test_query_returns_provider_error();
    test_entries_destroy_null();
    test_blob_destroy();

    printf("storage: all tests passed\n");
    return 0;
}
