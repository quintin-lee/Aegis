#ifndef AEGIS_MESSAGE_TOOL_RESULT_H
#define AEGIS_MESSAGE_TOOL_RESULT_H

#include "aegis/types.h"
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file tool_result.h
 * @brief First-class Tool Result model for Message layer.
 * Distinct from tool/tool.h's aegis_tool_result_t (which holds tool execution value).
 *
 * All strings are owned (copied on set/clone); getters return borrowed
 * pointers valid until the next set or destroy.
 */

typedef struct aegis_message_tool_result aegis_message_tool_result_t;

/** Create a zeroed result (status defaults to AEGIS_OK). NULL out → INVALID. */
aegis_status_t aegis_message_tool_result_create(aegis_message_tool_result_t** out);
/** Destroy and free owned strings. Safe with NULL. */
void aegis_message_tool_result_destroy(aegis_message_tool_result_t* res);
/** Deep-copy all fields. NULL src/out → INVALID. */
aegis_status_t aegis_message_tool_result_clone(const aegis_message_tool_result_t* src,
                                               aegis_message_tool_result_t**      out);

/** Borrowed call id, or NULL when unset/absent. */
const char* aegis_message_tool_result_call_id(const aegis_message_tool_result_t* res);
/** Borrowed content, or NULL when unset/absent. */
const char* aegis_message_tool_result_content(const aegis_message_tool_result_t* res);
/** Borrowed error text, or NULL when unset/absent. */
const char* aegis_message_tool_result_error(const aegis_message_tool_result_t* res);
/** Stored status; AEGIS_ERR_INVALID when res is NULL. */
aegis_status_t aegis_message_tool_result_status(const aegis_message_tool_result_t* res);
/** Partial-delivery flag; false when res is NULL. */
bool aegis_message_tool_result_is_partial(const aegis_message_tool_result_t* res);

/** Copy call_id (NULL clears). NULL res → INVALID. */
aegis_status_t aegis_message_tool_result_set_call_id(aegis_message_tool_result_t* res,
                                                     const char*                  call_id);
/** Copy content (NULL clears). NULL res → INVALID. */
aegis_status_t aegis_message_tool_result_set_content(aegis_message_tool_result_t* res,
                                                     const char*                  content);
/** Copy error text (NULL clears). NULL res → INVALID. */
aegis_status_t aegis_message_tool_result_set_error(aegis_message_tool_result_t* res,
                                                   const char*                  error);
/** Store status. NULL res → INVALID. */
aegis_status_t aegis_message_tool_result_set_status(aegis_message_tool_result_t* res,
                                                    aegis_status_t               status);
/** Store partial flag. NULL res → INVALID. */
aegis_status_t aegis_message_tool_result_set_is_partial(aegis_message_tool_result_t* res,
                                                        bool                         partial);

#ifdef __cplusplus
}
#endif

#endif /* AEGIS_MESSAGE_TOOL_RESULT_H */
