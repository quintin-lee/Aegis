/**
 * @file provider_embedding_hash.h
 * @brief Factory for the hash-based embedding provider.
 */
#ifndef AEGIS_PROVIDER_EMBEDDING_HASH_H
#define AEGIS_PROVIDER_EMBEDDING_HASH_H

#include "aegis/provider/embedding.h"
#include "aegis/provider/provider.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Opaque hash embedding context. */
typedef struct hash_embed_ctx hash_embed_ctx_t;

/**
 * @brief Create a hash-based embedding provider instance and its registry definition.
 *
 * @param[out] out_ctx Receives the embedding context. Ownership: transferred.
 * @param[out] out_ops Receives the ops struct (BORROWED — owned by registry entry).
 * @param[out] out_def Receives a shallow copy of the provider def.
 * @return AEGIS_OK or AEGIS_ERR_NOMEM.
 */
aegis_status_t aegis_embedding_hash_create(hash_embed_ctx_t**            out_ctx,
                                           const aegis_embedding_ops_t** out_ops,
                                           aegis_provider_def_t*         out_def);

/**
 * @brief Destroy the hash embedding context.
 *
 * Safe to call with NULL. Does NOT free ops (it's borrowed from registry).
 *
 * @param ctx Context to destroy (ownership: consumed).
 * @param ops Ops struct (borrowed; untouched).
 */
void aegis_embedding_hash_destroy(hash_embed_ctx_t* ctx, const aegis_embedding_ops_t* ops);

#ifdef __cplusplus
}
#endif

#endif /* AEGIS_PROVIDER_EMBEDDING_HASH_H */
