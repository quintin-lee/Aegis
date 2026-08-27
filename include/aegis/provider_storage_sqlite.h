/**
 * @file provider_storage_sqlite.h
 * @brief Factory for the SQLite storage provider.
 */
#ifndef AEGIS_PROVIDER_STORAGE_SQLITE_H
#define AEGIS_PROVIDER_STORAGE_SQLITE_H

#include "aegis/storage.h"
#include "aegis/provider.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Opaque SQLite storage context. */
typedef struct aegis_sqlite_storage_ctx aegis_sqlite_storage_ctx_t;

/**
 * @brief Create a SQLite storage provider instance and its registry definition.
 *
 * @param db_path    Path to SQLite database file (may be NULL if in_memory is true).
 *                   Owned — copied into the context.
 * @param in_memory  If true, use ":memory:" instead of db_path.
 * @param[out] out_ctx Receives the storage context. Ownership: transferred.
 * @param[out] out_ops Receives the ops struct (BORROWED — owned by registry entry).
 * @param[out] out_def Receives a shallow copy of the provider def.
 * @return AEGIS_OK, AEGIS_ERR_INVALID, or AEGIS_ERR_NOMEM.
 */
aegis_status_t aegis_storage_sqlite_create(const char* db_path, int in_memory,
                                           aegis_sqlite_storage_ctx_t** out_ctx,
                                           aegis_storage_ops_t**        out_ops,
                                           aegis_provider_def_t*        out_def);

/**
 * @brief Destroy the SQLite storage context.
 *
 * Closes the database and frees the path copy. Safe to call with NULL.
 *
 * @param ctx Context to destroy (ownership: consumed).
 * @param ops Ops struct (borrowed; untouched).
 */
void aegis_storage_sqlite_destroy(aegis_sqlite_storage_ctx_t* ctx, const aegis_storage_ops_t* ops);

#ifdef __cplusplus
}
#endif

#endif /* AEGIS_PROVIDER_STORAGE_SQLITE_H */
