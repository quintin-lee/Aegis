/**
 * @file storage_sqlite.c
 * @brief SQLite-backed storage provider.
 *
 * Implements aegis_storage_ops_t backed by SQLite.
 *
 * Configuration (via init user context):
 *   The user pointer must point to an aegis_sqlite_storage_ctx_t:
 *   - db_path: path to SQLite database file (owned, freed on shutdown)
 *   - in_memory: if true, use :memory: instead of db_path
 *
 * Thread model: SINGLE_THREAD (SQLite serialized mode handled internally)
 */
#define _POSIX_C_SOURCE 200809L
#include "aegis/storage/storage.h"
#include "aegis/status.h"
#include "../internal/lifecycle.h"
#include "aegis/common/cancellation/cancellation.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sqlite3.h>

/* ── Provider context ──────────────────────────────────────────────────────── */

typedef struct aegis_sqlite_storage_ctx {
    char*    db_path;
    int      in_memory;
    sqlite3* db;
} aegis_sqlite_storage_ctx_t;

/* ── Init / Shutdown ───────────────────────────────────────────────────────── */

static aegis_status_t sqlite_storage_init(void* user)
{
    const aegis_storage_ops_t*  ops = (const aegis_storage_ops_t*)user;
    aegis_sqlite_storage_ctx_t* ctx = ops ? (aegis_sqlite_storage_ctx_t*)ops->ctx : NULL;
    if (!ctx) {
        return AEGIS_ERR_INVALID;
    }

    const char* db_path = ctx->in_memory ? ":memory:" : ctx->db_path;
    if (!db_path) {
        return AEGIS_ERR_INVALID;
    }

    int rc = sqlite3_open(db_path, &ctx->db);
    if (rc != SQLITE_OK) {
        return AEGIS_ERR_INTERNAL;
    }

    /* Create table if not exists. */
    const char* sql =
        "CREATE TABLE IF NOT EXISTS aegis_store ("
        "key TEXT PRIMARY KEY,"
        "value BLOB"
        ");";
    char* err_msg = NULL;
    rc            = sqlite3_exec(ctx->db, sql, NULL, NULL, &err_msg);
    if (rc != SQLITE_OK) {
        sqlite3_close(ctx->db);
        ctx->db = NULL;
        if (err_msg) {
            sqlite3_free(err_msg);
        }
        return AEGIS_ERR_INTERNAL;
    }
    if (err_msg) {
        sqlite3_free(err_msg);
    }

    return AEGIS_OK;
}

static void sqlite_storage_shutdown(void* user)
{
    if (!user) {
        return;
    }
    const aegis_storage_ops_t*  ops = (const aegis_storage_ops_t*)user;
    aegis_sqlite_storage_ctx_t* ctx = (aegis_sqlite_storage_ctx_t*)ops->ctx;
    if (!ctx) {
        return;
    }
    if (ctx->db) {
        sqlite3_close(ctx->db);
        ctx->db = NULL;
    }
    free(ctx->db_path);
    ctx->db_path = NULL;
}

/* ── Storage callbacks ─────────────────────────────────────────────────────── */

static aegis_status_t sqlite_storage_put(void* user, const void* key, size_t key_len,
                                         const void* value, size_t value_len,
                                         const aegis_cancellation_token_t* token)
{
    (void)token;
    aegis_sqlite_storage_ctx_t* ctx = (aegis_sqlite_storage_ctx_t*)user;
    if (!ctx || !ctx->db || !key) {
        return AEGIS_ERR_INVALID;
    }

    const char*   sql  = "INSERT OR REPLACE INTO aegis_store (key, value) VALUES (?, ?);";
    sqlite3_stmt* stmt = NULL;
    int           rc   = sqlite3_prepare_v2(ctx->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        return AEGIS_ERR_INTERNAL;
    }

    sqlite3_bind_text(stmt, 1, (const char*)key, (int)key_len, SQLITE_TRANSIENT);
    if (value && value_len > 0) {
        sqlite3_bind_blob(stmt, 2, value, (int)value_len, SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_null(stmt, 2);
    }

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        return AEGIS_ERR_INTERNAL;
    }
    return AEGIS_OK;
}

static aegis_status_t sqlite_storage_get(void* user, const void* key, size_t key_len,
                                         const aegis_cancellation_token_t* token,
                                         aegis_storage_blob_t*             out)
{
    (void)token;
    aegis_sqlite_storage_ctx_t* ctx = (aegis_sqlite_storage_ctx_t*)user;
    if (!ctx || !ctx->db || !key || !out) {
        return AEGIS_ERR_INVALID;
    }

    memset(out, 0, sizeof(*out));

    const char*   sql  = "SELECT value FROM aegis_store WHERE key = ?;";
    sqlite3_stmt* stmt = NULL;
    int           rc   = sqlite3_prepare_v2(ctx->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        return AEGIS_ERR_INTERNAL;
    }

    sqlite3_bind_text(stmt, 1, (const char*)key, (int)key_len, SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        const void* blob = sqlite3_column_blob(stmt, 0);
        int         len  = sqlite3_column_bytes(stmt, 0);
        if (len > 0 && blob) {
            out->data = malloc((size_t)len);
            if (!out->data) {
                sqlite3_finalize(stmt);
                return AEGIS_ERR_NOMEM;
            }
            memcpy(out->data, blob, (size_t)len);
            out->len = (size_t)len;
        }
    }

    sqlite3_finalize(stmt);
    return AEGIS_OK;
}

static aegis_status_t sqlite_storage_del(void* user, const void* key, size_t key_len,
                                         const aegis_cancellation_token_t* token)
{
    (void)token;
    aegis_sqlite_storage_ctx_t* ctx = (aegis_sqlite_storage_ctx_t*)user;
    if (!ctx || !ctx->db || !key) {
        return AEGIS_ERR_INVALID;
    }

    const char*   sql  = "DELETE FROM aegis_store WHERE key = ?;";
    sqlite3_stmt* stmt = NULL;
    int           rc   = sqlite3_prepare_v2(ctx->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        return AEGIS_ERR_INTERNAL;
    }

    sqlite3_bind_text(stmt, 1, (const char*)key, (int)key_len, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        return AEGIS_ERR_INTERNAL;
    }
    return AEGIS_OK;
}

/* ── Factory ───────────────────────────────────────────────────────────────── */

aegis_status_t aegis_storage_sqlite_create(const char* db_path, int in_memory,
                                           aegis_sqlite_storage_ctx_t** out_ctx,
                                           aegis_storage_ops_t**        out_ops,
                                           aegis_provider_def_t*        out_def)
{
    AEGIS_CHECK_OUT(out_ctx);
    AEGIS_CHECK_OUT(out_ops);
    AEGIS_CHECK_OUT(out_def);

    /* Allocate context. */
    aegis_sqlite_storage_ctx_t* ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        return AEGIS_ERR_NOMEM;
    }
    if (db_path) {
        ctx->db_path = strdup(db_path);
        if (!ctx->db_path) {
            free(ctx);
            return AEGIS_ERR_NOMEM;
        }
    }
    ctx->in_memory = in_memory ? 1 : 0;
    ctx->db        = NULL;

    /* Allocate ops. */
    aegis_storage_ops_t* ops = malloc(sizeof(*ops));
    if (!ops) {
        free(ctx->db_path);
        free(ctx);
        return AEGIS_ERR_NOMEM;
    }
    ops->ctx = ctx;
    ops->put = sqlite_storage_put;
    ops->get = sqlite_storage_get;
    ops->del = sqlite_storage_del;

    /* Allocate provider def. */
    aegis_provider_def_t* def = malloc(sizeof(*def));
    if (!def) {
        free(ops);
        free(ctx->db_path);
        free(ctx);
        return AEGIS_ERR_NOMEM;
    }
    def->name         = in_memory ? "storage-sqlite-memory" : "storage-sqlite";
    def->description  = "SQLite-backed key/value storage";
    def->abi_version  = AEGIS_PROVIDER_ABI_VERSION;
    def->kind         = AEGIS_PROVIDER_STORAGE;
    def->capabilities = AEGIS_CAP_WRITE_FILE;
    def->thread_model = AEGIS_PROVIDER_SINGLE_THREAD;
    def->init         = sqlite_storage_init;
    def->shutdown     = sqlite_storage_shutdown;
    def->user         = ops;

    *out_ctx = ctx;
    *out_ops = ops;
    *out_def = *def;
    free(def);

    return AEGIS_OK;
}

void aegis_storage_sqlite_destroy(aegis_sqlite_storage_ctx_t* ctx, const aegis_storage_ops_t* ops)
{
    free((void*)ops);
    if (!ctx) {
        return;
    }
    if (ctx->db) {
        sqlite3_close(ctx->db);
        ctx->db = NULL;
    }
    free(ctx->db_path);
    free(ctx);
}
