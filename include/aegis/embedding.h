#ifndef AEGIS_EMBEDDING_H
#define AEGIS_EMBEDDING_H

#include "aegis/cancellation.h"
#include "aegis/provider.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file embedding.h
 * @brief Typed dispatch interface for AEGIS_PROVIDER_EMBEDDING providers.
 *
 * Vectors are plain float arrays with an explicit dimension — no model
 * or vendor format is baked into the ABI.
 *
 * Ownership: input text is borrowed; the result OWNS its float array,
 * released via aegis_embedding_result_destroy() (idempotent).
 *
 * Thread safety: same contract as llm.h dispatch (registry-locked resolve,
 * lock-free callback, thread_model honored by callers).
 */

/** Embedding result. @c vector is owned by the holder of this struct. */
typedef struct aegis_embedding_result {
    float* vector; /**< Dim floats (owned; NULL on failure). */
    size_t dim;    /**< Vector dimensionality.               */
} aegis_embedding_result_t;

/**
 * @brief Free result payload and zero the struct. Idempotent.
 */
void aegis_embedding_result_destroy(aegis_embedding_result_t* res);

/**
 * @brief Provider-side embedding callback (invoked lock-free).
 *
 * Fill @c out with an owned malloc'd vector on success; on failure
 * return non-OK and leave @c out zeroed.
 */
typedef aegis_status_t (*aegis_embed_fn)(void* ctx, const char* text, size_t text_len,
                                         const aegis_cancellation_token_t* token,
                                         aegis_embedding_result_t*         out);

/**
 * @brief Operation set published by an embedding provider.
 *
 * Register by setting def.user to a pointer to this struct (BORROWED,
 * typically a file-scope const object); @c ctx carries the provider
 * instance state. A NULL fn reports AEGIS_ERR_PROVIDER from dispatch.
 */
typedef struct aegis_embedding_ops {
    void*          ctx;   /**< Provider instance state (borrowed). */
    aegis_embed_fn embed; /**< Required for dispatch.             */
} aegis_embedding_ops_t;

/**
 * @brief Dispatch an embedding request through the registry.
 *
 * Same gate order as aegis_llm_complete(): NOT_FOUND / kind INVALID /
 * uninitialized PERM / pre-cancelled CANCELLED / verbatim rc.
 *
 * @return AEGIS_OK or any status above.
 */
aegis_status_t aegis_embed(const aegis_provider_registry_t* reg, const char* name, const char* text,
                           size_t text_len, const aegis_cancellation_token_t* token,
                           aegis_embedding_result_t* out);

#ifdef __cplusplus
}
#endif

#endif /* AEGIS_EMBEDDING_H */
