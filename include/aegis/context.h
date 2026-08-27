/**
 * @file context.h
 * @brief Build structured LLM context from multiple sources with budget control.
 *
 * The Context module assembles a prompt from system instructions, goal, plan,
 * task, memory, tool definitions, and observations into a single text blob
 * suitable for LLM input.
 *
 * Design principles:
 *   - Context is independent of any specific LLM provider. It only produces
 *     a string; the caller decides how to send it.
 *   - Each section has a priority (higher = included first on truncation).
 *   - A token budget limits total size; sections below the cutoff are dropped.
 *   - Truncation is deterministic: sort by priority desc, include until budget
 *     exhausted, no randomness.
 *   - Ownership: the builder owns all sections; the built context owns its
 *     content string; the caller owns the returned context handle.
 *
 * Thread safety: a single builder is NOT thread-safe. Callers must synchronize
 * externally. Built contexts are immutable after creation.
 */
#ifndef AEGIS_CONTEXT_H
#define AEGIS_CONTEXT_H

#include "aegis/status.h"
#include "aegis/cancellation.h"
#include "aegis/types.h"

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Section source classification ─────────────────────────────────────────── */

/** Source category for a context section. */
typedef enum aegis_context_source {
    AEGIS_CONTEXT_SYSTEM,      /**< System-level instructions.               */
    AEGIS_CONTEXT_GOAL,        /**< Current agent goal.                      */
    AEGIS_CONTEXT_PLAN,        /**< Serialized plan steps.                   */
    AEGIS_CONTEXT_TASK,        /**< Current task being executed.             */
    AEGIS_CONTEXT_MEMORY,      /**< Relevant memory entries.                 */
    AEGIS_CONTEXT_TOOL_DEFS,   /**< Available tool definitions.              */
    AEGIS_CONTEXT_OBSERVATION, /**< Recent observation / tool output.        */
    AEGIS_CONTEXT_HISTORY,     /**< Recent conversation history.             */
} aegis_context_source_t;

/* ── Context item ──────────────────────────────────────────────────────────── */

/**
 * @brief One section within a context build.
 *
 * All strings are copied into the item on add. The builder owns the item;
 * the caller must NOT free @p content after passing it to a builder.
 */
typedef struct aegis_context_item {
    char*                  content;        /**< Owned text content.               */
    aegis_context_source_t source;         /**< Categorization.                   */
    int                    priority;       /**< Higher = more important (0-100).  */
    size_t                 token_estimate; /**< Approximate token count (for budget). */
} aegis_context_item_t;

/* ── Built context ─────────────────────────────────────────────────────────── */

/**
 * @brief The result of a context build.
 *
 * Ownership: the caller receives the handle and is responsible for
 * calling aegis_context_destroy() when done.
 */
typedef struct aegis_context aegis_context_t;

/* ── Compression callback ──────────────────────────────────────────────────── */

/**
 * @brief Optional compression function.
 *
 * Called on a section whose content exceeds @p max_chars before it is
 * added to the context. The implementation must write a compressed
 * version into @p out_buf (size @p out_buf_size) and return the actual
 * number of characters written. Return 0 to skip the section entirely.
 *
 * This is a pluggable hook — the default (NULL) performs no compression.
 *
 * @param content      Original content (borrowed).
 * @param content_len  Length in bytes.
 * @param out_buf      Output buffer.
 * @param out_buf_size Capacity of @p out_buf in bytes.
 * @return Characters written (≤ @p out_buf_size - 1), or 0 to discard.
 */
typedef size_t (*aegis_context_compress_fn)(const char* content, size_t content_len, char* out_buf,
                                            size_t out_buf_size);

/* ── Builder ───────────────────────────────────────────────────────────────── */

/** Opaque builder handle. */
typedef struct aegis_context_builder aegis_context_builder_t;

/**
 * @brief Create a context builder.
 *
 * @param[out] out  Receives the builder. Ownership: transferred.
 * @return AEGIS_OK or AEGIS_ERR_NOMEM.
 */
aegis_status_t aegis_context_builder_create(aegis_context_builder_t** out);

/**
 * @brief Destroy a context builder and release all owned sections.
 *
 * Safe to call with NULL (no-op).
 *
 * @param builder Handle to destroy (ownership: consumed).
 */
void aegis_context_builder_destroy(aegis_context_builder_t* builder);

/**
 * @brief Add a section to the builder.
 *
 * The builder copies @p content internally. Callers may free or reuse
 * their own buffer immediately after this call.
 *
 * @param builder  Builder (borrowed).
 * @param content  Section text (borrowed; required, non-NULL).
 * @param source   Source category.
 * @param priority Importance weight (0-100; higher = more important).
 * @param token_estimate Approximate token count (0 = auto-estimate by chars/4).
 * @return AEGIS_OK or AEGIS_ERR_NOMEM.
 */
aegis_status_t aegis_context_builder_add_section(aegis_context_builder_t* builder,
                                                 const char* content, aegis_context_source_t source,
                                                 int priority, size_t token_estimate);

/**
 * @brief Set an optional compression callback.
 *
 * Called before a section is added if its content length exceeds
 * @p max_uncompressed_chars. Pass NULL to disable compression.
 *
 * @param builder                Builder (borrowed).
 * @param compress_fn            Compression callback (borrowed; may be NULL).
 * @param compress_user          User pointer passed to @p compress_fn (may be NULL).
 * @param max_uncompressed_chars Maximum content length (bytes) that triggers compression.
 */
void aegis_context_builder_set_compression(aegis_context_builder_t*  builder,
                                           aegis_context_compress_fn compress_fn,
                                           void* compress_user, size_t max_uncompressed_chars);

/**
 * @brief Set the token budget for context assembly.
 *
 * Sections are sorted by priority (descending); once cumulative token
 * estimate exceeds the budget, remaining sections are dropped.
 * A budget of 0 means unlimited.
 *
 * @param builder Builder (borrowed).
 * @param budget  Max total tokens (0 = unlimited).
 */
void aegis_context_builder_set_budget(aegis_context_builder_t* builder, size_t budget);

/**
 * @brief Build the context from all added sections.
 *
 * Assembles sections in priority order, applies truncation per budget,
 * and returns the final prompt string.
 *
 * @param builder  Builder (borrowed).
 * @param token    Cancellation token (borrowed; may be NULL).
 * @param[out] out Receives the built context. Ownership: transferred.
 * @return AEGIS_OK, AEGIS_ERR_NOMEM, or AEGIS_ERR_CANCELLED.
 */
aegis_status_t aegis_context_build(const aegis_context_builder_t*    builder,
                                   const aegis_cancellation_token_t* token, aegis_context_t** out);

/* ── Context accessors ─────────────────────────────────────────────────────── */

/** Total token estimate of the built context. */
size_t aegis_context_token_estimate(const aegis_context_t* ctx);

/** True if the context was truncated to fit the budget. */
bool aegis_context_is_truncated(const aegis_context_t* ctx);

/** The assembled prompt text (borrowed; valid until destroy). */
const char* aegis_context_content(const aegis_context_t* ctx);

/**
 * @brief Destroy a built context and free its content.
 *
 * Safe to call with NULL.
 *
 * @param ctx Context handle (ownership: consumed).
 */
void aegis_context_destroy(aegis_context_t* ctx);

#ifdef __cplusplus
}
#endif

#endif /* AEGIS_CONTEXT_H */
