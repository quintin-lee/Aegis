/**
 * @file memory_mock.h
 * @brief Mock in-memory storage backend for unit testing memory modules.
 *
 * Provides a drop-in replacement for aegis_storage_ops_t that stores
 * everything in a simple hashmap. No external dependencies, no network,
 * no disk I/O.
 */
#ifndef AEGIS_MEMORY_MOCK_H
#define AEGIS_MEMORY_MOCK_H

#include "aegis/storage.h"
#include "aegis/status.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Create a mock storage context.
 *
 * @param[out] out  Receives the context. Ownership: transferred.
 * @return AEGIS_OK or AEGIS_ERR_NOMEM.
 */
aegis_status_t aegis_mock_storage_create(void** out);

/**
 * @brief Destroy a mock storage context.
 *
 * Safe to call with NULL.
 *
 * @param ctx Context (ownership: consumed).
 */
void aegis_mock_storage_destroy(void* ctx);

/**
 * @brief Fill @p ops with mock implementations.
 *
 * @param ctx The mock context returned by aegis_mock_storage_create().
 * @param[out] ops Receives the ops struct.
 */
void aegis_mock_storage_ops(void* ctx, aegis_storage_ops_t* ops);

#ifdef __cplusplus
}
#endif

#endif /* AEGIS_MEMORY_MOCK_H */
