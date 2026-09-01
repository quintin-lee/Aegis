#ifndef AEGIS_MODEL_RESPONSE_H
#define AEGIS_MODEL_RESPONSE_H

#include "aegis/message/message.h"
#include "aegis/message/usage.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file response.h
 * @brief Structured model response (complete, non-streaming).
 */

typedef struct aegis_model_response {
    aegis_message_t* message; /**< Owned assistant message (may contain tool_calls) */
    aegis_usage_t    usage;
    char*            raw; /**< Owned raw JSON if provider preserves it */
} aegis_model_response_t;

aegis_status_t aegis_model_response_create(aegis_model_response_t** out);
void           aegis_model_response_destroy(aegis_model_response_t* resp);

#ifdef __cplusplus
}
#endif

#endif /* AEGIS_MODEL_RESPONSE_H */
