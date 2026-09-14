#ifndef AEGIS_MODEL_REQUEST_H
#define AEGIS_MODEL_REQUEST_H

#include "aegis/message/message.h"
#include "aegis/tool/tool.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file request.h
 * @brief Structured model request — replaces blob prompt.
 */

typedef struct aegis_model_request {
    const char*                  model;    /**< Borrowed model name */
    const aegis_message_list_t*  messages; /**< Borrowed message list */
    const aegis_tool_registry_t* tools;    /**< Borrowed tool registry (may be NULL) */
    uint32_t                     max_tokens;
    float                        temperature;
    bool                         stream;
    const char*                  tool_choice; /**< Borrowed, e.g. "auto" */
    void*                        metadata;    /**< Borrowed user metadata */
    uint32_t
        thinking_budget; /**< 0 = disabled; else Anthropic thinking tokens (must be < max_tokens) */
} aegis_model_request_t;

#ifdef __cplusplus
}
#endif

#endif /* AEGIS_MODEL_REQUEST_H */
