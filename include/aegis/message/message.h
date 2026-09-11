#ifndef AEGIS_MESSAGE_H
#define AEGIS_MESSAGE_H

#include "aegis/message/role.h"
#include "aegis/message/content.h"
#include "aegis/message/tool_call.h"
#include "aegis/message/tool_result.h"
#include "aegis/message/usage.h"
#include "aegis/types.h"

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file message.h
 * @brief First-class Message model — opaque handle.
 *
 * Message is the unified container for LLM input/output, tool calls and results.
 * All strings are owned (copied on set/clone); getters return borrowed
 * pointers valid until the next mutation or destroy.
 */

typedef struct aegis_message      aegis_message_t;
typedef struct aegis_message_list aegis_message_list_t;

/* ── Message lifecycle ───────────────────────────────────────────────── */

/** Create message with role; NULL out → INVALID. */
aegis_status_t aegis_message_create(aegis_message_role_t role, aegis_message_t** out);
/** Destroy and free owned strings. Safe with NULL. */
void           aegis_message_destroy(aegis_message_t* msg);
/** Deep-copy a message. NULL src/out → INVALID. */
aegis_status_t aegis_message_clone(const aegis_message_t* src, aegis_message_t** out);

/* ── Message accessors ───────────────────────────────────────────────── */

/** Borrowed id, or NULL when unset. */
const char*              aegis_message_id(const aegis_message_t* msg);
/** Message role. */
aegis_message_role_t     aegis_message_role(const aegis_message_t* msg);
/** Creation timestamp (ms since epoch). */
uint64_t                 aegis_message_timestamp(const aegis_message_t* msg);
/** Borrowed text content, or NULL when unset. */
const char*              aegis_message_content(const aegis_message_t* msg);
/** Borrowed reasoning text, or NULL when unset. */
const char*              aegis_message_reasoning(const aegis_message_t* msg);
/** Borrowed tool-call id this message answers, or NULL. */
const char*              aegis_message_tool_call_id(const aegis_message_t* msg);
/** Borrowed parent message id, or NULL. */
const char*              aegis_message_parent_id(const aegis_message_t* msg);
/** Number of attached tool calls. */
size_t                   aegis_message_tool_call_count(const aegis_message_t* msg);
/** Borrowed tool call at idx. */
const aegis_tool_call_t* aegis_message_tool_call_at(const aegis_message_t* msg, size_t idx);

/* ── Message mutators ────────────────────────────────────────────────── */

/** Copy id (NULL clears). NULL msg → INVALID. */
aegis_status_t aegis_message_set_id(aegis_message_t* msg, const char* id);
/** Copy text content (NULL clears). NULL msg → INVALID. */
aegis_status_t aegis_message_set_content(aegis_message_t* msg, const char* text);
/** Copy reasoning text (NULL clears). NULL msg → INVALID. */
aegis_status_t aegis_message_set_reasoning(aegis_message_t* msg, const char* reasoning);
/** Copy tool-call id (NULL clears). NULL msg → INVALID. */
aegis_status_t aegis_message_set_tool_call_id(aegis_message_t* msg, const char* tool_call_id);
/** Copy parent id (NULL clears). NULL msg → INVALID. */
aegis_status_t aegis_message_set_parent_id(aegis_message_t* msg, const char* parent_id);
/** Attach a copy of @p call. NULL msg/call → INVALID. */
aegis_status_t aegis_message_add_tool_call(aegis_message_t* msg, const aegis_tool_call_t* call);

/* ── Message list ────────────────────────────────────────────────────── */

/** Create empty list. NULL out → INVALID. */
aegis_status_t aegis_message_list_create(aegis_message_list_t** out);
/** Destroy list and contained messages. Safe with NULL. */
void           aegis_message_list_destroy(aegis_message_list_t* list);
/** Deep-copy a list. NULL src/out → INVALID. */
aegis_status_t aegis_message_list_clone(const aegis_message_list_t* src,
                                        aegis_message_list_t**      out);

/** Number of messages in the list. */
size_t                 aegis_message_list_count(const aegis_message_list_t* list);
/** Borrowed message at idx. */
const aegis_message_t* aegis_message_list_at(const aegis_message_list_t* list, size_t idx);
/** Append a copy of @p msg. NULL list/msg → INVALID. */
aegis_status_t aegis_message_list_append(aegis_message_list_t* list, const aegis_message_t* msg);
/** Prepend a copy of @p msg. NULL list/msg → INVALID. */
aegis_status_t aegis_message_list_prepend(aegis_message_list_t* list, const aegis_message_t* msg);

#ifdef __cplusplus
}
#endif

#endif /* AEGIS_MESSAGE_H */
