/**
 * @file storage_internal.h
 * @brief Internal layout for the storage store module.
 *
 * NOT part of the public ABI.
 */
#ifndef AEGIS_STORAGE_INTERNAL_H
#define AEGIS_STORAGE_INTERNAL_H

#include "aegis/storage.h"
#include "aegis/common/hashmap.h"
#include "aegis/common/vector.h"

#include <stddef.h>
#include <stdint.h>

/* ── Transaction internals ─────────────────────────────────────────────────── */

/** Operation type within a transaction. */
typedef enum aegis_storage_txn_op {
    AEGIS_STORAGE_TXN_PUT,
    AEGIS_STORAGE_TXN_DELETE,
} aegis_storage_txn_op_t;

/** One staged operation in a transaction. */
typedef struct aegis_storage_txn_op_entry {
    aegis_storage_txn_op_t op;
    void*                  key;       /**< Owned. */
    size_t                 key_len;
    void*                  value;     /**< Owned (PUT only). */
    size_t                 value_len; /**< Owned (PUT only). */
} aegis_storage_txn_op_entry_t;

struct aegis_storage_transaction {
    aegis_vector_t* ops; /**< Vector of aegis_storage_txn_op_entry_t*. */
};

/* ── Snapshot internals ────────────────────────────────────────────────────── */

struct aegis_storage_snapshot {
    aegis_hashmap_t* data; /**< Key -> value (both owned by the map). */
};

#endif /* AEGIS_STORAGE_INTERNAL_H */
