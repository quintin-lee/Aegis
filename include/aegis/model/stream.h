#ifndef AEGIS_MODEL_STREAM_H
#define AEGIS_MODEL_STREAM_H

#include "aegis/types.h"
#include "aegis/message/usage.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file stream.h
 * @brief Streaming model events — provider ↔ agent loop decoupling.
 */

typedef enum aegis_model_stream_event_type {
    AEGIS_MODEL_STREAM_TEXT_DELTA      = 0,
    AEGIS_MODEL_STREAM_REASONING_DELTA = 1,
    AEGIS_MODEL_STREAM_TOOL_CALL_START = 2,
    AEGIS_MODEL_STREAM_TOOL_CALL_DELTA = 3,
    AEGIS_MODEL_STREAM_TOOL_CALL_END   = 4,
    AEGIS_MODEL_STREAM_USAGE           = 5,
    AEGIS_MODEL_STREAM_END             = 6,
    AEGIS_MODEL_STREAM_ERROR           = 7
} aegis_model_stream_event_type_t;

typedef struct aegis_model_stream_event {
    aegis_model_stream_event_type_t type;
    const void*                     data;      /**< Borrowed event payload (type-specific) */
    size_t                          len;       /**< Payload length */
    uint32_t                        index;     /**< Tool call index for TOOL_CALL_* */
    const char*                     tool_name; /**< For TOOL_CALL_START */
    const char*                     call_id;   /**< For TOOL_CALL_* */
} aegis_model_stream_event_t;

typedef aegis_status_t (*aegis_model_stream_callback_fn)(const aegis_model_stream_event_t* event,
                                                         void*                             user);

const char* aegis_model_stream_event_type_str(aegis_model_stream_event_type_t t);

#ifdef __cplusplus
}
#endif

#endif /* AEGIS_MODEL_STREAM_H */
