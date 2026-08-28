/**
 * @file embedding_hash.c
 * @brief Deterministic hash-based embedding provider.
 *
 * This provider does NOT call external embedding models. Instead, it
 * produces deterministic float vectors from text using a simple hash
 * function. Useful for:
 *   - Testing without external dependencies
 *   - Prototyping vector search logic
 *   - Local development
 *
 * The hash function is NOT cryptographically secure — it's a fast
 * DJB2 variant mapped to float range [-1, 1].
 */
#define _POSIX_C_SOURCE 200809L
#include "aegis/provider/embedding.h"
#include "aegis/status.h"
#include "../internal/lifecycle.h"
#include "aegis/common/cancellation/cancellation.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ── Constants ─────────────────────────────────────────────────────────────── */

#define EMBEDDING_DIM 64

/* ── Provider context ──────────────────────────────────────────────────────── */

typedef struct hash_embed_ctx {
    int call_count;
} hash_embed_ctx_t;

/* ── Hash function ─────────────────────────────────────────────────────────── */

static uint64_t hash_djb2(const unsigned char* data, size_t len)
{
    uint64_t hash = 5381;
    for (size_t i = 0; i < len; i++) {
        hash = hash * 131u + data[i];
    }
    return hash;
}

/* ── Embed callback ────────────────────────────────────────────────────────── */

static aegis_status_t hash_embed_embed(void* ctx, const char* text, size_t text_len,
                                       const aegis_cancellation_token_t* token,
                                       aegis_embedding_result_t*         out)
{
    (void)token;
    hash_embed_ctx_t* mock = (hash_embed_ctx_t*)ctx;
    if (!mock || !out) {
        return AEGIS_ERR_INVALID;
    }
    if (!text && text_len > 0) {
        return AEGIS_ERR_INVALID;
    }

    mock->call_count++;

    /* Allocate result vector. */
    float* vec = calloc(EMBEDDING_DIM, sizeof(float));
    if (!vec) {
        return AEGIS_ERR_NOMEM;
    }

    if (text_len > 0) {
        uint64_t h = hash_djb2((const unsigned char*)text, text_len);
        /* Fill vector deterministically from hash. */
        for (int i = 0; i < EMBEDDING_DIM; i++) {
            h = h * 6364136223846793005u + 1442695040888963407u;
            /* Map to [-1, 1] via sine. */
            vec[i] = (float)sin((double)h / 1e18);
        }
    }

    /* Normalize to unit length. */
    double norm = 0.0;
    for (int i = 0; i < EMBEDDING_DIM; i++) {
        norm += (double)vec[i] * (double)vec[i];
    }
    norm = sqrt(norm);
    if (norm > 0.0) {
        for (int i = 0; i < EMBEDDING_DIM; i++) {
            vec[i] /= (float)norm;
        }
    }

    out->vector = vec;
    out->dim    = EMBEDDING_DIM;
    return AEGIS_OK;
}

/* ── Init / Shutdown ───────────────────────────────────────────────────────── */

static aegis_status_t hash_embed_init(void* user)
{
    (void)user;
    return AEGIS_OK;
}

static void hash_embed_shutdown(void* user)
{
    (void)user;
}

/* ── Factory ───────────────────────────────────────────────────────────────── */

aegis_status_t aegis_embedding_hash_create(hash_embed_ctx_t**            out_ctx,
                                           const aegis_embedding_ops_t** out_ops,
                                           aegis_provider_def_t*         out_def)
{
    AEGIS_CHECK_OUT(out_ctx);
    AEGIS_CHECK_OUT(out_ops);
    AEGIS_CHECK_OUT(out_def);

    hash_embed_ctx_t* ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        return AEGIS_ERR_NOMEM;
    }
    ctx->call_count = 0;

    aegis_embedding_ops_t* ops = malloc(sizeof(*ops));
    if (!ops) {
        free(ctx);
        return AEGIS_ERR_NOMEM;
    }
    ops->ctx   = ctx;
    ops->embed = hash_embed_embed;

    aegis_provider_def_t* def = malloc(sizeof(*def));
    if (!def) {
        free(ops);
        free(ctx);
        return AEGIS_ERR_NOMEM;
    }
    def->name         = "embedding-hash";
    def->description  = "Deterministic hash-based embedding provider";
    def->abi_version  = AEGIS_PROVIDER_ABI_VERSION;
    def->kind         = AEGIS_PROVIDER_EMBEDDING;
    def->capabilities = AEGIS_CAP_NONE;
    def->thread_model = AEGIS_PROVIDER_SINGLE_THREAD;
    def->init         = hash_embed_init;
    def->shutdown     = hash_embed_shutdown;
    def->user         = ops;

    *out_ctx = ctx;
    *out_ops = ops;
    *out_def = *def;
    free(def);

    return AEGIS_OK;
}

void aegis_embedding_hash_destroy(hash_embed_ctx_t* ctx, const aegis_embedding_ops_t* ops)
{
    free((void*)ops);
    free(ctx);
}
