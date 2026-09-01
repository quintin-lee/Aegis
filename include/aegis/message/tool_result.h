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
 */

typedef struct aegis_message_tool_result aegis_message_tool_result_t;

aegis_status_t aegis_message_tool_result_create(aegis_message_tool_result_t** out);
void           aegis_message_tool_result_destroy(aegis_message_tool_result_t* res);
aegis_status_t aegis_message_tool_result_clone(const aegis_message_tool_result_t* src,
                                               aegis_message_tool_result_t**      out);

const char*    aegis_message_tool_result_call_id(const aegis_message_tool_result_t* res);
const char*    aegis_message_tool_result_content(const aegis_message_tool_result_t* res);
const char*    aegis_message_tool_result_error(const aegis_message_tool_result_t* res);
aegis_status_t aegis_message_tool_result_status(const aegis_message_tool_result_t* res);
bool           aegis_message_tool_result_is_partial(const aegis_message_tool_result_t* res);

aegis_status_t aegis_message_tool_result_set_call_id(aegis_message_tool_result_t* res,
                                                     const char*                  call_id);
aegis_status_t aegis_message_tool_result_set_content(aegis_message_tool_result_t* res,
                                                     const char*                  content);
aegis_status_t aegis_message_tool_result_set_error(aegis_message_tool_result_t* res,
                                                   const char*                  error);
aegis_status_t aegis_message_tool_result_set_status(aegis_message_tool_result_t* res,
                                                    aegis_status_t               status);
aegis_status_t aegis_message_tool_result_set_is_partial(aegis_message_tool_result_t* res,
                                                        bool                         partial);

#ifdef __cplusplus
}
#endif

#endif /* AEGIS_MESSAGE_TOOL_RESULT_H */
