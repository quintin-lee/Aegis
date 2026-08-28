/**
 * @file storage.h
 * @brief Storage abstractions: provider dispatch + higher-level store ops.
 *
 * This header defines two layers:
 *
 * 1. Provider dispatch (backward-compatible):
 *    - aegis_storage_blob_t: retrieved value wrapper
 *    - aegis_storage_ops_t: provider callbacks (put/get/delete)
 *    - aegis_storage_put/get/delete(): registry-dispatched operations
 *
 * 2. Higher-level store operations:
 *    - Transaction: batched put/delete with commit semantics.
 *    - Snapshot: take/restore point-in-time copies.
 *    - Query: prefix-based key enumeration.
 *
 * The store operations are independent of any specific backend; they
 * delegate to the provider layer for actual I/O.
 */
#ifndef AEGIS_STORAGE_H
#define AEGIS_STORAGE_H

#include "aegis/executor/cancellation.h"
#include "aegis/provider/provider.h"
#include "aegis/status.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ═══════════════════════════════════════════════════════════════════════════
 * Layer 1: Provider dispatch (backward-compatible)
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Retrieved value. @c data is owned by the holder; free with
 *        aegis_storage_blob_destroy().
 */
typedef struct aegis_storage_blob {
    void*  data; /**< Value bytes (owned; NULL when absent). */
    size_t len;  /**< Value length in bytes.               */
} aegis_storage_blob_t;

/**
 * @brief Free blob payload and zero the struct. Idempotent.
 */
void aegis_storage_blob_destroy(aegis_storage_blob_t* blob);

/** Provider-side callbacks (invoked lock-free). */
typedef aegis_status_t (*aegis_storage_put_fn)(void* ctx, const void* key, size_t key_len,
                                               const void* value, size_t value_len,
                                               const aegis_cancellation_token_t* token);
typedef aegis_status_t (*aegis_storage_get_fn)(void* ctx, const void* key, size_t key_len,
                                               const aegis_cancellation_token_t* token,
                                               aegis_storage_blob_t*             out);
typedef aegis_status_t (*aegis_storage_del_fn)(void* ctx, const void* key, size_t key_len,
                                               const aegis_cancellation_token_t* token);

/**
 * @brief Operation set published by a storage provider.
 *
 * Register by setting def.user to a pointer to this struct (BORROWED);
 * @p ctx carries the provider instance state.
 */
typedef struct aegis_storage_ops {
    void*                ctx; /**< Provider instance state (borrowed). */
    aegis_storage_put_fn put; /**< Optional. */
    aegis_storage_get_fn get; /**< Optional. */
    aegis_storage_del_fn del; /**< Optional. */
} aegis_storage_ops_t;

/**
 * @brief Dispatch storage operations through the registry.
 */
aegis_status_t aegis_storage_put(const aegis_provider_registry_t* reg, const char* name,
                                 const void* key, size_t key_len, const void* value,
                                 size_t value_len, const aegis_cancellation_token_t* token);
aegis_status_t aegis_storage_get(const aegis_provider_registry_t* reg, const char* name,
                                 const void* key, size_t key_len,
                                 const aegis_cancellation_token_t* token,
                                 aegis_storage_blob_t*             out);
aegis_status_t aegis_storage_delete(const aegis_provider_registry_t* reg, const char* name,
                                    const void* key, size_t key_len,
                                    const aegis_cancellation_token_t* token);

/* ═══════════════════════════════════════════════════════════════════════════
 * Layer 2: Higher-level store operations
 * ═══════════════════════════════════════════════════════════════════════════ */

/** ABI version of the storage store interface. */
#define AEGIS_STORAGE_ABI_VERSION 1u

/**
 * @brief A single key/value pair returned by a query.
 *
 * @c key and @c value are owned by the entries handle; do not free them.
 */
typedef struct aegis_storage_entry {
    void*  key;       /**< Owned copy of the key bytes.     */
    size_t key_len;   /**< Length of @p key in bytes.       */
    void*  value;     /**< Owned copy of the value bytes.   */
    size_t value_len; /**< Length of @p value in bytes.     */
} aegis_storage_entry_t;

/**
 * @brief Destroy a query result set. Frees all entries and their keys/values.
 * Safe to call with NULL.
 */
void aegis_storage_entries_destroy(aegis_storage_entry_t* entries);

/* ── Transaction ──────────────────────────────────────────────────────────── */

/** Opaque transaction handle. */
typedef struct aegis_storage_transaction aegis_storage_transaction_t;

/**
 * @brief Begin a new transaction.
 *
 * @param[out] out  Receives the transaction. Ownership: transferred.
 * @return AEGIS_OK or AEGIS_ERR_NOMEM.
 */
aegis_status_t aegis_storage_transaction_create(aegis_storage_transaction_t** out);

/**
 * @brief Destroy a transaction without committing. Discards all pending changes.
 * Safe to call with NULL.
 */
void aegis_storage_transaction_destroy(aegis_storage_transaction_t* txn);

/**
 * @brief Stage a put operation within the transaction.
 *
 * @param txn       Transaction (borrowed).
 * @param key       Key bytes (borrowed; required).
 * @param key_len   Length of @p key.
 * @param value     Value bytes (borrowed; may be NULL if value_len is 0).
 * @param value_len Length of @p value.
 * @return AEGIS_OK or AEGIS_ERR_INVALID.
 */
aegis_status_t aegis_storage_transaction_put(aegis_storage_transaction_t* txn, const void* key,
                                             size_t key_len, const void* value, size_t value_len);

/**
 * @brief Stage a delete operation within the transaction.
 *
 * @param txn     Transaction (borrowed).
 * @param key     Key to delete (borrowed; required).
 * @param key_len Length of @p key.
 * @return AEGIS_OK.
 */
aegis_status_t aegis_storage_transaction_delete(aegis_storage_transaction_t* txn, const void* key,
                                                size_t key_len);

/**
 * @brief Commit the transaction: apply all staged operations.
 *
 * After commit, the transaction is consumed. Returns AEGIS_ERR_CANCELLED
 * if the token is already tripped.
 *
 * @param txn     Transaction (ownership: consumed on success).
 * @param token   Cancellation token (borrowed; may be NULL).
 * @return AEGIS_OK, AEGIS_ERR_CANCELLED, or AEGIS_ERR_INVALID.
 */
aegis_status_t aegis_storage_transaction_commit(aegis_storage_transaction_t*      txn,
                                                const aegis_cancellation_token_t* token);

/* ── Snapshot ─────────────────────────────────────────────────────────────── */

/** Opaque snapshot handle. */
typedef struct aegis_storage_snapshot aegis_storage_snapshot_t;

/**
 * @brief Take a snapshot of the current store state.
 *
 * Enumerates all entries via aegis_storage_query() and stores deep
 * copies. The caller retains ownership.
 *
 * @param reg          Provider registry (borrowed).
 * @param store_name   Storage provider name (borrowed).
 * @param token        Cancellation token (borrowed; may be NULL).
 * @param[out] out      Receives the snapshot. Ownership: transferred.
 * @return AEGIS_OK, AEGIS_ERR_NOT_FOUND, or AEGIS_ERR_NOMEM.
 */
aegis_status_t aegis_storage_snapshot_take(const aegis_provider_registry_t*  reg,
                                           const char*                       store_name,
                                           const aegis_cancellation_token_t* token,
                                           aegis_storage_snapshot_t**        out);

/**
 * @brief Restore a snapshot into the store.
 *
 * Clears the store and replays all entries from the snapshot.
 *
 * @param reg          Provider registry (borrowed).
 * @param store_name   Storage provider name (borrowed).
 * @param snapshot     Snapshot to restore (borrowed).
 * @param token        Cancellation token (borrowed; may be NULL).
 * @return AEGIS_OK, AEGIS_ERR_NOT_FOUND, or AEGIS_ERR_NOMEM.
 */
aegis_status_t aegis_storage_snapshot_restore(const aegis_provider_registry_t*  reg,
                                              const char*                       store_name,
                                              const aegis_storage_snapshot_t*   snapshot,
                                              const aegis_cancellation_token_t* token);

/**
 * @brief Destroy a snapshot. Safe to call with NULL.
 */
void aegis_storage_snapshot_destroy(aegis_storage_snapshot_t* snapshot);

/** Number of entries in the snapshot. */
size_t aegis_storage_snapshot_count(const aegis_storage_snapshot_t* snap);

/* ── Query ─────────────────────────────────────────────────────────────────── */

/**
 * @brief Query entries by prefix match.
 *
 * Returns all key/value pairs whose key starts with @p prefix.
 * Results are transferred to the caller; free with
 * aegis_storage_entries_destroy().
 *
 * @param reg          Provider registry (borrowed).
 * @param store_name   Storage provider name (borrowed).
 * @param prefix       Key prefix to match (borrowed; NULL matches all).
 * @param prefix_len   Length of @p prefix (0 if NULL).
 * @param token        Cancellation token (borrowed; may be NULL).
 * @param[out] out      Receives the entry array. Ownership: transferred.
 * @param[out] out_count Receives the count.
 * @return AEGIS_OK, AEGIS_ERR_NOT_FOUND, or AEGIS_ERR_PROVIDER.
 */
aegis_status_t aegis_storage_query(const aegis_provider_registry_t* reg, const char* store_name,
                                   const char* prefix, size_t prefix_len,
                                   const aegis_cancellation_token_t* token,
                                   aegis_storage_entry_t** out, size_t* out_count);

#ifdef __cplusplus
}
#endif

#endif /* AEGIS_STORAGE_H */
