#ifndef AEGIS_STORAGE_H
#define AEGIS_STORAGE_H

#include "aegis/cancellation.h"
#include "aegis/provider.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file storage.h
 * @brief Typed dispatch interface for AEGIS_PROVIDER_STORAGE providers.
 *
 * Minimal key/value blob contract over arbitrary backends (in-memory,
 * embedded DB, remote object store). Keys and values are raw bytes;
 * no SQL, serialization format, or vendor SDK appears in the ABI.
 *
 * Ownership: keys/values passed IN are borrowed; retrieved blobs are
 * OWNED by the returned struct and freed via aegis_storage_blob_destroy()
 * (idempotent).
 *
 * Thread safety: same contract as llm.h dispatch (registry-locked resolve,
 * lock-free callback, per-provider thread_model honored by callers).
 */

/** Retrieved value. @c data is owned by the holder of this struct. */
typedef struct aegis_storage_blob {
    void*  data; /**< Value bytes (owned; NULL when absent/empty). */
    size_t len;  /**< Value length in bytes.                       */
} aegis_storage_blob_t;
/**
 * @brief Free blob payload and zero the struct. Idempotent.
 */
void aegis_storage_blob_destroy(aegis_storage_blob_t* blob);

/**
 * @brief Provider-side callbacks (invoked lock-free).
 *
 * put: stores a copy of @p value under @p key (backend-defined TTL/persistence).
 * get: fills @c out with an owned copy of the stored value; AEGIS_ERR_NOT_FOUND
 *      when the key is absent (@c out stays zeroed).
 * del: removes the key; AEGIS_ERR_NOT_FOUND when it was absent.
 */
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
 * Register by setting def.user to a pointer to this struct (BORROWED,
 * typically a file-scope const object); @c ctx carries the provider
 * instance state and is handed to every callback.
 *
 * Providers implement whichever subset applies; a NULL op reports
 * AEGIS_ERR_PROVIDER from dispatch (declared-but-missing capability).
 */
typedef struct aegis_storage_ops {
    void*                ctx; /**< Provider instance state (borrowed). */
    aegis_storage_put_fn put; /**< Optional. */
    aegis_storage_get_fn get; /**< Optional. */
    aegis_storage_del_fn del; /**< Optional. */
} aegis_storage_ops_t;

/**
 * @brief Dispatch storage operations through the registry.
 *
 * Gate order per call: NOT_FOUND (unknown name) / kind INVALID /
 * uninitialized PERM / pre-cancelled CANCELLED / missing op PROVIDER /
 * verbatim backend rc.
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

#ifdef __cplusplus
}
#endif

#endif /* AEGIS_STORAGE_H */
