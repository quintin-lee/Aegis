/**
 * @file context.c
 * @brief Build structured LLM context from multiple sources.
 *
 * Assembly algorithm:
 *   1. Collect all sections from the builder.
 *   2. Sort by priority descending (deterministic).
 *   3. Accumulate tokens; stop when budget exceeded.
 *   4. Join selected sections with separator into final prompt.
 *
 * No LLM provider is referenced — the output is a plain string.
 */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#define _POSIX_C_SOURCE 200809L
#include "aegis/context/context.h"
#include "aegis/message/message.h"
#include "aegis/status.h"
#include "aegis/common/cancellation/cancellation.h"

#include "context_internal.h"
#include "lifecycle.h"

#include "aegis/common/vector.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Helpers ───────────────────────────────────────────────────────────────── */

/**
 * @brief Estimate the token count of a text span.
 *
 * Uses the rough English heuristic of one token per four characters.
 *
 * @param[in] text  Text to measure; NULL yields 0.
 * @param[in] len   Length of @p text in bytes.
 * @return Estimated token count, or 0 for NULL/empty input.
 */
static size_t estimate_tokens(const char* text, size_t len)
{
    if (!text || len == 0) {
        return 0;
    }
    /* Rough English estimate: 1 token ≈ 4 characters. */
    return (len + 3) / 4;
}

/**
 * @brief Compare two builder sections by priority, descending.
 *
 * qsort-style comparator over pointers to section pointers. NULL entries
 * compare equal. Equal priorities compare equal so that a stable sort
 * preserves insertion order among them.
 *
 * @param[in] a  Pointer to the left section pointer.
 * @param[in] b  Pointer to the right section pointer.
 * @return Negative if @p a sorts first, positive if @p b sorts first, 0 if tied.
 */
static int cmp_section_by_priority_desc(const void* a, const void* b)
{
    const aegis_context_item_t* const* x = (const aegis_context_item_t* const*)a;
    const aegis_context_item_t* const* y = (const aegis_context_item_t* const*)b;
    if (!*x || !*y) {
        return 0;
    }
    if ((*x)->priority > (*y)->priority) {
        return -1;
    }
    if ((*x)->priority < (*y)->priority) {
        return 1;
    }
    /* Same priority: preserve insertion order (stable sort). */
    return 0;
}

/* ── Builder ───────────────────────────────────────────────────────────────── */

/**
 * @brief Create an empty context builder.
 *
 * The builder starts with no sections, a zero (unlimited) token budget, and
 * no compression hook installed.
 *
 * @param[out] out  Receives the new builder on success; set only on success.
 * @return AEGIS_OK on success, AEGIS_ERR_NOMEM on allocation failure.
 *
 * Ownership: the caller owns the returned builder and must release it with
 * aegis_context_builder_destroy().
 */
aegis_status_t aegis_context_builder_create(aegis_context_builder_t** out)
{
    AEGIS_CHECK_OUT(out);
    aegis_context_builder_t* b = calloc(1, sizeof(*b));
    if (!b) {
        return AEGIS_ERR_NOMEM;
    }
    int rc = aegis_vector_create(&b->sections, sizeof(aegis_context_item_t*));
    if (rc != 0) {
        free(b);
        return AEGIS_ERR_NOMEM;
    }
    b->token_budget     = 0;
    b->compress_fn      = NULL;
    b->compress_user    = NULL;
    b->max_compress_len = 0;
    *out                = b;
    return AEGIS_OK;
}

/**
 * @brief Destroy a context builder and all sections added to it.
 *
 * Releases every section's duplicated content together with the section
 * vector and the builder itself. NULL is accepted and ignored.
 *
 * @param[in] builder  Builder to destroy, or NULL.
 */
void aegis_context_builder_destroy(aegis_context_builder_t* builder)
{
    if (!builder) {
        return;
    }
    if (builder->sections) {
        size_t n = aegis_vector_len(builder->sections);
        for (size_t i = 0; i < n; i++) {
            aegis_context_item_t* item = NULL;
            aegis_vector_get(builder->sections, i, &item);
            if (item) {
                free(item->content);
                free(item);
            }
        }
        aegis_vector_destroy(builder->sections);
    }
    free(builder);
}

/**
 * @brief Append a content section to the builder.
 *
 * The content string is duplicated, so the caller retains ownership of
 * @p content. A zero @p token_estimate requests automatic estimation via
 * the 1-token-per-4-characters heuristic. NULL, empty, and zero-length
 * content is rejected.
 *
 * @param[in] builder         Builder to extend.
 * @param[in] content         NUL-terminated section text (copied).
 * @param[in] source          Provenance tag selecting the message role later.
 * @param[in] priority        Higher values are packed first under a budget.
 * @param[in] token_estimate  Precomputed cost, or 0 to auto-estimate.
 * @return AEGIS_OK on success, AEGIS_ERR_INVALID for bad arguments,
 *         AEGIS_ERR_NOMEM on allocation failure.
 */
aegis_status_t aegis_context_builder_add_section(aegis_context_builder_t* builder,
                                                 const char* content, aegis_context_source_t source,
                                                 int priority, size_t token_estimate)
{
    if (!builder || !content || content[0] == '\0') {
        return AEGIS_ERR_INVALID;
    }
    aegis_context_item_t* item = calloc(1, sizeof(*item));
    if (!item) {
        return AEGIS_ERR_NOMEM;
    }
    item->content        = strdup(content);
    item->source         = source;
    item->priority       = priority;
    item->token_estimate = token_estimate;
    if (!item->content) {
        free(item);
        return AEGIS_ERR_NOMEM;
    }
    if (item->token_estimate == 0) {
        item->token_estimate = estimate_tokens(item->content, strlen(item->content));
    }
    int rc = aegis_vector_push(builder->sections, &item);
    if (rc != 0) {
        free(item->content);
        free(item);
        return AEGIS_ERR_NOMEM;
    }
    return AEGIS_OK;
}

/**
 * @brief Install an optional section-compression hook on the builder.
 *
 * Sections longer than @p max_uncompressed_chars are passed through
 * @p compress_fn at build time; a hook returning 0 discards the section.
 * NULL builder is ignored.
 *
 * @param[in] builder                 Builder to configure.
 * @param[in] compress_fn             Compression callback, or NULL to disable.
 * @param[in] compress_user           Opaque pointer forwarded to the callback.
 * @param[in] max_uncompressed_chars  Length threshold triggering compression.
 */
void aegis_context_builder_set_compression(aegis_context_builder_t*  builder,
                                           aegis_context_compress_fn compress_fn,
                                           void* compress_user, size_t max_uncompressed_chars)
{
    if (!builder) {
        return;
    }
    builder->compress_fn      = compress_fn;
    builder->compress_user    = compress_user;
    builder->max_compress_len = max_uncompressed_chars;
}

/**
 * @brief Set the token budget applied at build time.
 *
 * A budget of 0 means unlimited: every section is included. Otherwise the
 * build packs sections in priority order until the next section would exceed
 * the budget. NULL builder is ignored.
 *
 * @param[in] builder  Builder to configure.
 * @param[in] budget   Maximum estimated tokens, or 0 for unlimited.
 */
void aegis_context_builder_set_budget(aegis_context_builder_t* builder, size_t budget)
{
    if (!builder) {
        return;
    }
    builder->token_budget = budget;
}

/* ── Build ─────────────────────────────────────────────────────────────────── */

/**
 * @brief Assemble the builder's sections into a single prompt string.
 *
 * Sections are packed in priority order until the token budget (if any)
 * would be exceeded; oversized sections pass through the compression hook
 * when one is installed. The sections are joined with a "\n\n---\n\n"
 * separator. A pre-cancelled @p token aborts with AEGIS_ERR_CANCELLED.
 * An empty builder yields an empty (non-NULL) prompt.
 *
 * @param[in]  builder  Builder holding the sections to assemble.
 * @param[in]  token    Optional cancellation token, may be NULL.
 * @param[out] out      Receives the new context on success; set only on success.
 * @return AEGIS_OK on success, AEGIS_ERR_INVALID for bad arguments,
 *         AEGIS_ERR_CANCELLED if cancelled, AEGIS_ERR_NOMEM on allocation failure.
 *
 * Ownership: the caller owns the returned context and must release it with
 * aegis_context_destroy().
 */
aegis_status_t aegis_context_build(const aegis_context_builder_t*    builder,
                                   const aegis_cancellation_token_t* token, aegis_context_t** out)
{
    if (!builder || !out) {
        return AEGIS_ERR_INVALID;
    }
    *out = NULL;

    if (token && aegis_cancellation_token_is_cancelled(token)) {
        return AEGIS_ERR_CANCELLED;
    }

    aegis_context_t* ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        return AEGIS_ERR_NOMEM;
    }

    size_t n = aegis_vector_len(builder->sections);
    if (n == 0) {
        ctx->content        = strdup("");
        ctx->token_estimate = 0;
        ctx->truncated      = false;
        if (!ctx->content) {
            free(ctx);
            return AEGIS_ERR_NOMEM;
        }
        *out = ctx;
        return AEGIS_OK;
    }

    /* Copy all sections into a sortable array. */
    aegis_context_item_t** items = malloc(sizeof(aegis_context_item_t*) * n);
    if (!items) {
        free(ctx);
        return AEGIS_ERR_NOMEM;
    }
    for (size_t i = 0; i < n; i++) {
        aegis_context_item_t* item = NULL;
        aegis_vector_get(builder->sections, i, &item);
        items[i] = item;
    }

    /* Sort by priority descending (bubble sort — n is small). */
    for (size_t i = 0; i < n; i++) {
        for (size_t j = i + 1; j < n; j++) {
            if (cmp_section_by_priority_desc(&items[i], &items[j]) > 0) {
                aegis_context_item_t* tmp = items[i];
                items[i]                  = items[j];
                items[j]                  = tmp;
            }
        }
    }

    /* First pass: determine which sections fit within budget. */
    size_t cumulative = 0;
    int*   included   = calloc(n, sizeof(int));
    if (!included) {
        free(items);
        free(ctx);
        return AEGIS_ERR_NOMEM;
    }

    for (size_t i = 0; i < n; i++) {
        aegis_context_item_t* item = items[i];
        if (!item) {
            continue;
        }
        size_t seg_tokens = item->token_estimate;
        /* Apply compression estimate if needed. */
        if (builder->compress_fn && item->content &&
            strlen(item->content) > builder->max_compress_len) {
            seg_tokens = builder->max_compress_len / 4; /* rough estimate */
        }
        if (builder->token_budget > 0 && cumulative + seg_tokens > builder->token_budget) {
            break; /* Truncated. */
        }
        included[i] = 1;
        cumulative += seg_tokens;
    }
    bool truncated = (cumulative > 0 && builder->token_budget > 0);

    /* Second pass: compute total length for allocated buffer. */
    size_t total_len = 0;
    for (size_t i = 0; i < n; i++) {
        if (!included[i]) {
            continue;
        }
        aegis_context_item_t* item = items[i];
        if (!item || !item->content) {
            continue;
        }
        size_t seg_len = strlen(item->content);
        total_len += seg_len + 7; /* "\n\n---\n\n" separator */
    }

    /* Build final prompt string. */
    char* prompt = malloc(total_len + 1);
    if (!prompt) {
        free(included);
        free(items);
        free(ctx);
        return AEGIS_ERR_NOMEM;
    }
    prompt[0]  = '\0';
    size_t pos = 0;
    cumulative = 0;

    for (size_t i = 0; i < n; i++) {
        if (!included[i]) {
            continue;
        }
        aegis_context_item_t* item = items[i];
        if (!item || !item->content) {
            continue;
        }

        const char* segment = item->content;
        size_t      seg_len = strlen(segment);

        /* Apply compression if enabled. */
        char compress_buf[2048];
        if (builder->compress_fn && seg_len > builder->max_compress_len) {
            size_t compressed =
                builder->compress_fn(segment, seg_len, compress_buf, sizeof(compress_buf));
            if (compressed > 0) {
                compress_buf[compressed] = '\0';
                segment                  = compress_buf;
                seg_len                  = compressed;
            } else {
                continue; /* Section discarded by compressor. */
            }
        }

        size_t seg_tokens = estimate_tokens(segment, seg_len);
        if (builder->token_budget > 0 && cumulative + seg_tokens > builder->token_budget) {
            break;
        }
        cumulative += seg_tokens;

        if (pos > 0) {
            size_t sep_len = snprintf(prompt + pos, total_len - pos, "\n\n---\n\n");
            pos += sep_len;
        }
        size_t append_len = snprintf(prompt + pos, total_len - pos, "%s", segment);
        pos += append_len;
    }
    prompt[pos] = '\0';

    free(included);
    free(items);

    ctx->content        = prompt;
    ctx->token_estimate = cumulative;
    ctx->truncated      = truncated;
    *out                = ctx;
    return AEGIS_OK;
}

/**
 * @brief Assemble the builder's sections into a chat message list.
 *
 * Unlike aegis_context_build(), message order follows the conversation
 * protocol rather than priority: system/memory/tool-definition sections are
 * emitted first, then the remaining sections in insertion order. Each section
 * maps to one message whose role derives from its source tag (observation
 * sections become tool messages, all other non-system sections become user
 * messages). Budget enforcement and cancellation checks mirror the prompt
 * build; a mid-build cancellation destroys the partial list. An empty
 * builder yields an empty list.
 *
 * @param[in]  builder  Builder holding the sections to assemble.
 * @param[in]  token    Optional cancellation token, may be NULL.
 * @param[out] out      Receives the new message list on success; set only on success.
 * @return AEGIS_OK on success, AEGIS_ERR_INVALID for bad arguments,
 *         AEGIS_ERR_CANCELLED if cancelled, AEGIS_ERR_NOMEM on allocation failure
 *         (or any error propagated from message-list operations).
 *
 * Ownership: the caller owns the returned list and must release it with
 * aegis_message_list_destroy(), even when it is empty.
 */
aegis_status_t aegis_context_build_messages(const aegis_context_builder_t*    builder,
                                            const aegis_cancellation_token_t* token,
                                            aegis_message_list_t**            out)
{
    if (!builder || !out) {
        return AEGIS_ERR_INVALID;
    }
    *out = NULL;
    if (token && aegis_cancellation_token_is_cancelled(token)) {
        return AEGIS_ERR_CANCELLED;
    }

    aegis_message_list_t* list = NULL;
    aegis_status_t        st   = aegis_message_list_create(&list);
    if (st != AEGIS_OK) {
        return st;
    }

    size_t n = aegis_vector_len(builder->sections);
    if (n == 0) {
        *out = list;
        return AEGIS_OK;
    }

    aegis_context_item_t** items = malloc(sizeof(*items) * n);
    if (!items) {
        aegis_message_list_destroy(list);
        return AEGIS_ERR_NOMEM;
    }
    for (size_t i = 0; i < n; i++) {
        aegis_context_item_t* it = NULL;
        aegis_vector_get(builder->sections, i, &it);
        items[i] = it;
    }
    /* Message order is part of the conversation protocol. Do not sort the
     * history by priority: emit system sections first, then preserve the
     * original insertion order for all conversational messages. */
    size_t cumulative = 0;
    for (int pass = 0; pass < 2; ++pass) {
        for (size_t i = 0; i < n; i++) {
            if (token && aegis_cancellation_token_is_cancelled(token)) {
                free(items);
                aegis_message_list_destroy(list);
                return AEGIS_ERR_CANCELLED;
            }
            aegis_context_item_t* item = items[i];
            if (!item || !item->content) {
                continue;
            }
            bool is_system = item->source == AEGIS_CONTEXT_SYSTEM;
            if ((pass == 0) != is_system) {
                continue;
            }
            size_t seg_tokens = item->token_estimate
                                    ? item->token_estimate
                                    : estimate_tokens(item->content, strlen(item->content));
            if (builder->token_budget > 0 && cumulative + seg_tokens > builder->token_budget) {
                break;  // preserve priority order; later sections cannot displace this one
            }
            cumulative += seg_tokens;

            aegis_message_role_t role;
            switch (item->source) {
            case AEGIS_CONTEXT_SYSTEM:
            case AEGIS_CONTEXT_TOOL_DEFS:
            case AEGIS_CONTEXT_MEMORY:
                role = AEGIS_MESSAGE_SYSTEM;
                break;
            case AEGIS_CONTEXT_OBSERVATION:
                role = AEGIS_MESSAGE_TOOL;
                break;
            default:
                role = AEGIS_MESSAGE_USER;
                break;
            }
            aegis_message_t* msg = NULL;
            st                   = aegis_message_create(role, &msg);
            if (st != AEGIS_OK) {
                free(items);
                aegis_message_list_destroy(list);
                return st;
            }
            aegis_message_set_content(msg, item->content);
            st = aegis_message_list_append(list, msg);
            aegis_message_destroy(msg);
            if (st != AEGIS_OK) {
                free(items);
                aegis_message_list_destroy(list);
                return st;
            }
        }
    }
    free(items);
    *out = list;
    return AEGIS_OK;
}

/**
 * @brief Return the estimated token count recorded on a built context.
 *
 * @param[in] ctx  Built context, or NULL.
 * @return The recorded estimate, or 0 for NULL.
 */
size_t aegis_context_token_estimate(const aegis_context_t* ctx)
{
    return ctx ? ctx->token_estimate : 0;
}

/**
 * @brief Report whether the context build dropped sections over budget.
 *
 * @param[in] ctx  Built context, or NULL.
 * @return True if sections were omitted due to the token budget, false otherwise
 *         (NULL yields false).
 */
bool aegis_context_is_truncated(const aegis_context_t* ctx)
{
    return ctx ? ctx->truncated : false;
}

/**
 * @brief Borrow the assembled prompt string of a built context.
 *
 * @param[in] ctx  Built context, or NULL.
 * @return Pointer to the NUL-terminated prompt, or "" for NULL.
 *         The pointer remains valid until the context is destroyed.
 */
const char* aegis_context_content(const aegis_context_t* ctx)
{
    return ctx ? ctx->content : "";
}

/**
 * @brief Destroy a built context and its prompt string.
 *
 * NULL is accepted and ignored.
 *
 * @param[in] ctx  Context to destroy, or NULL.
 */
void aegis_context_destroy(aegis_context_t* ctx)
{
    if (!ctx) {
        return;
    }
    free(ctx->content);
    free(ctx);
}

#pragma GCC diagnostic pop
