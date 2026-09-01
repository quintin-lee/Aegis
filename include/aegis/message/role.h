#ifndef AEGIS_MESSAGE_ROLE_H
#define AEGIS_MESSAGE_ROLE_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file role.h
 * @brief Message role discriminator.
 */

typedef enum aegis_message_role {
    AEGIS_MESSAGE_SYSTEM    = 0, /**< System instructions */
    AEGIS_MESSAGE_USER      = 1, /**< Human user */
    AEGIS_MESSAGE_ASSISTANT = 2, /**< Assistant / model */
    AEGIS_MESSAGE_TOOL      = 3, /**< Tool output (tool_result) */
    AEGIS_MESSAGE_EVENT     = 4, /**< Internal event */
    AEGIS_MESSAGE_SUMMARY   = 5  /**< Compaction summary */
} aegis_message_role_t;

const char* aegis_message_role_str(aegis_message_role_t role);

#ifdef __cplusplus
}
#endif

#endif /* AEGIS_MESSAGE_ROLE_H */
