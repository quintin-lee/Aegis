#ifndef AEGIS_MESSAGE_CONTENT_H
#define AEGIS_MESSAGE_CONTENT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file content.h
 * @brief Message content (text + optional reasoning).
 */

typedef struct aegis_message_content {
    char* text;      /**< Owned text (may be NULL) */
    char* reasoning; /**< Owned reasoning/thinking (may be NULL) */
} aegis_message_content_t;

#ifdef __cplusplus
}
#endif

#endif /* AEGIS_MESSAGE_CONTENT_H */
