/**
 * @file context_internal.h
 * @brief Internal layout for the context module.
 *
 * NOT part of the public ABI.
 */
#ifndef AEGIS_CONTEXT_INTERNAL_H
#define AEGIS_CONTEXT_INTERNAL_H

#include "aegis/context/context.h"
#include "aegis/common/vector.h"

#include <stddef.h>

/** Default compression buffer size. */
#define AEGIS_CONTEXT_COMPRESS_BUF 1024u

/* ── Builder internals ─────────────────────────────────────────────────────── */

struct aegis_context_builder {
    aegis_vector_t*       sections;              /**< Vector of aegis_context_item_t*. */
    size_t                token_budget;          /**< 0 = unlimited.                  */
    aegis_context_compress_fn compress_fn;       /**< May be NULL.                    */
    void*                 compress_user;         /**< Passed to compress_fn.          */
    size_t                max_compress_len;      /**< Content length that triggers compression. */
};

/* ── Built context internals ───────────────────────────────────────────────── */

struct aegis_context {
    char*           content;        /**< Owned assembled prompt.            */
    size_t          token_estimate; /**< Total estimated tokens.            */
    bool            truncated;      /**< True if budget was exceeded.       */
};

#endif /* AEGIS_CONTEXT_INTERNAL_H */
