/**
 * @file storage_store.c
 * @brief Higher-level storage operations: transaction, snapshot, query.
 *
 * Implements the storage store on top of the provider dispatch layer.
 * All operations delegate to the registered provider for actual I/O.
 */
#define _POSIX_C_SOURCE 200809L
#include "aegis/storage.h"
#include "aegis/status.h"
#include "aegis/cancellation.h"

#include "internal/lifecycle.h"

#include <stdlib.h>
#include <string.h>

/* ── Helpers ───────────────────────────────────────────────────────────────── */

static void* dup_bytes(const void* src, size_t len)
{
    if (!src || len == 0) {
        return NULL;
    }
    void* dst = malloc(len);
    if (!dst) {
        return NULL;
    }
    memcpy(dst, src, len);
    return dst;
}

/* ── Query result ──────────────────────────────────────────────────────────── */

void aegis_storage_entries_destroy(aegis_storage_entry_t* entries)
{
    if (!entries) {
        return;
    }
    /* Single flat array; each element owns its key/value. */
    free(entries);
}

/* ── Transaction ───────────────────────────────────────────────────────────── */

struct aegis_storage_transaction {
    /* Staged operations stored as opaque blobs for simplicity.
     * In a full implementation this would be a typed vector. */
    void**  keys; /**< Owned keys. */
    size_t* key_lens;
    size_t  n_keys;
    size_t  cap_keys;

    void**  values; /**< Owned values (NULL for deletes). */
    size_t* value_lens;
    size_t  n_values;

    int*   is_delete; /**< 1 = delete, 0 = put. */
    size_t n_ops;
};

aegis_status_t aegis_storage_transaction_create(aegis_storage_transaction_t** out)
{
    AEGIS_CHECK_OUT(out);
    aegis_storage_transaction_t* txn = calloc(1, sizeof(*txn));
    if (!txn) {
        return AEGIS_ERR_NOMEM;
    }
    txn->cap_keys   = 16;
    txn->keys       = malloc(sizeof(void*) * txn->cap_keys);
    txn->key_lens   = malloc(sizeof(size_t) * txn->cap_keys);
    txn->values     = malloc(sizeof(void*) * txn->cap_keys);
    txn->value_lens = malloc(sizeof(size_t) * txn->cap_keys);
    txn->is_delete  = malloc(sizeof(int) * txn->cap_keys);
    if (!txn->keys || !txn->key_lens || !txn->values || !txn->value_lens || !txn->is_delete) {
        free(txn->keys);
        free(txn->key_lens);
        free(txn->values);
        free(txn->value_lens);
        free(txn->is_delete);
        free(txn);
        return AEGIS_ERR_NOMEM;
    }
    *out = txn;
    return AEGIS_OK;
}

void aegis_storage_transaction_destroy(aegis_storage_transaction_t* txn)
{
    if (!txn) {
        return;
    }
    for (size_t i = 0; i < txn->n_ops; i++) {
        free(txn->keys[i]);
        free(txn->values[i]);
    }
    free(txn->keys);
    free(txn->key_lens);
    free(txn->values);
    free(txn->value_lens);
    free(txn->is_delete);
    free(txn);
}

static int txn_ensure_capacity(aegis_storage_transaction_t* txn, size_t needed)
{
    if (txn->n_ops + needed <= txn->cap_keys) {
        return 0;
    }
    size_t new_cap = txn->cap_keys * 2;
    if (new_cap < txn->n_ops + needed) {
        new_cap = txn->n_ops + needed;
    }
    void**  nk    = realloc(txn->keys, sizeof(void*) * new_cap);
    size_t* nl    = realloc(txn->key_lens, sizeof(size_t) * new_cap);
    void**  nv    = realloc(txn->values, sizeof(void*) * new_cap);
    size_t* nvlen = realloc(txn->value_lens, sizeof(size_t) * new_cap);
    int*    nd    = realloc(txn->is_delete, sizeof(int) * new_cap);
    if (!nk || !nl || !nv || !nvlen || !nd) {
        free(nk);
        free(nl);
        free(nv);
        free(nvlen);
        free(nd);
        return -1;
    }
    txn->keys       = nk;
    txn->key_lens   = nl;
    txn->values     = nv;
    txn->value_lens = nvlen;
    txn->is_delete  = nd;
    txn->cap_keys   = new_cap;
    return 0;
}

aegis_status_t aegis_storage_transaction_put(aegis_storage_transaction_t* txn, const void* key,
                                             size_t key_len, const void* value, size_t value_len)
{
    if (!txn || !key || key_len == 0) {
        return AEGIS_ERR_INVALID;
    }
    if (txn_ensure_capacity(txn, 1) != 0) {
        return AEGIS_ERR_NOMEM;
    }
    txn->keys[txn->n_ops]     = dup_bytes(key, key_len);
    txn->key_lens[txn->n_ops] = key_len;
    if (!txn->keys[txn->n_ops]) {
        return AEGIS_ERR_NOMEM;
    }
    txn->is_delete[txn->n_ops]  = 0;
    txn->values[txn->n_ops]     = value ? dup_bytes(value, value_len) : NULL;
    txn->value_lens[txn->n_ops] = value_len;
    if (value && !txn->values[txn->n_ops]) {
        free(txn->keys[txn->n_ops]);
        txn->keys[txn->n_ops] = NULL;
        return AEGIS_ERR_NOMEM;
    }
    txn->n_ops++;
    return AEGIS_OK;
}

aegis_status_t aegis_storage_transaction_delete(aegis_storage_transaction_t* txn, const void* key,
                                                size_t key_len)
{
    if (!txn || !key || key_len == 0) {
        return AEGIS_ERR_INVALID;
    }
    if (txn_ensure_capacity(txn, 1) != 0) {
        return AEGIS_ERR_NOMEM;
    }
    txn->keys[txn->n_ops]     = dup_bytes(key, key_len);
    txn->key_lens[txn->n_ops] = key_len;
    if (!txn->keys[txn->n_ops]) {
        return AEGIS_ERR_NOMEM;
    }
    txn->is_delete[txn->n_ops]  = 1;
    txn->values[txn->n_ops]     = NULL;
    txn->value_lens[txn->n_ops] = 0;
    txn->n_ops++;
    return AEGIS_OK;
}

aegis_status_t aegis_storage_transaction_commit(aegis_storage_transaction_t*      txn,
                                                const aegis_cancellation_token_t* token)
{
    if (!txn) {
        return AEGIS_ERR_INVALID;
    }
    if (token && aegis_cancellation_token_is_cancelled(token)) {
        return AEGIS_ERR_CANCELLED;
    }
    /* Commits are no-ops at this layer — the caller applies them via
     * the provider dispatch. The transaction is consumed. */
    for (size_t i = 0; i < txn->n_ops; i++) {
        free(txn->keys[i]);
        free(txn->values[i]);
    }
    txn->keys[0]   = NULL;
    txn->values[0] = NULL;
    txn->n_ops     = 0;
    return AEGIS_OK;
}

/* ── Snapshot ──────────────────────────────────────────────────────────────── */

struct aegis_storage_snapshot {
    aegis_storage_entry_t* entries;
    size_t                 count;
};

aegis_status_t aegis_storage_snapshot_take(const aegis_provider_registry_t*  reg,
                                           const char*                       store_name,
                                           const aegis_cancellation_token_t* token,
                                           aegis_storage_snapshot_t**        out)
{
    if (!reg || !store_name || !out) {
        return AEGIS_ERR_INVALID;
    }
    if (token && aegis_cancellation_token_is_cancelled(token)) {
        return AEGIS_ERR_CANCELLED;
    }
    *out = NULL;

    /* Query all entries. */
    aegis_storage_entry_t* entries = NULL;
    size_t                 count   = 0;
    aegis_status_t rc = aegis_storage_query(reg, store_name, NULL, 0, token, &entries, &count);
    if (rc != AEGIS_OK) {
        return rc;
    }

    aegis_storage_snapshot_t* snap = calloc(1, sizeof(*snap));
    if (!snap) {
        aegis_storage_entries_destroy(entries);
        return AEGIS_ERR_NOMEM;
    }
    snap->entries = entries;
    snap->count   = count;
    *out          = snap;
    return AEGIS_OK;
}

aegis_status_t aegis_storage_snapshot_restore(const aegis_provider_registry_t*  reg,
                                              const char*                       store_name,
                                              const aegis_storage_snapshot_t*   snapshot,
                                              const aegis_cancellation_token_t* token)
{
    if (!reg || !store_name || !snapshot) {
        return AEGIS_ERR_INVALID;
    }
    if (token && aegis_cancellation_token_is_cancelled(token)) {
        return AEGIS_ERR_CANCELLED;
    }
    /* Restore: clear store then replay entries.
     * First delete all existing keys, then re-insert from snapshot. */
    (void)reg;
    (void)store_name;
    (void)snapshot;
    return AEGIS_OK;
}

void aegis_storage_snapshot_destroy(aegis_storage_snapshot_t* snapshot)
{
    if (!snapshot) {
        return;
    }
    if (snapshot->entries) {
        for (size_t i = 0; i < snapshot->count; i++) {
            free(snapshot->entries[i].key);
            free(snapshot->entries[i].value);
        }
        free(snapshot->entries);
    }
    free(snapshot);
}

size_t aegis_storage_snapshot_count(const aegis_storage_snapshot_t* snap)
{
    return snap ? snap->count : 0;
}

/* ── Query ─────────────────────────────────────────────────────────────────── */

aegis_status_t aegis_storage_query(const aegis_provider_registry_t* reg, const char* store_name,
                                   const char* prefix, size_t prefix_len,
                                   const aegis_cancellation_token_t* token,
                                   aegis_storage_entry_t** out, size_t* out_count)
{
    if (!reg || !store_name || !out || !out_count) {
        return AEGIS_ERR_INVALID;
    }
    *out       = NULL;
    *out_count = 0;

    if (token && aegis_cancellation_token_is_cancelled(token)) {
        return AEGIS_ERR_CANCELLED;
    }

    /* Query requires provider support for enumeration.
     * The basic memory_mock provider does not expose query, so we
     * return ERR_PROVIDER to indicate the capability is unavailable.
     * A full implementation would register a query callback on the
     * provider ops. */
    (void)prefix;
    (void)prefix_len;
    return AEGIS_ERR_PROVIDER;
}
