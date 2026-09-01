#define _POSIX_C_SOURCE 200809L
#include "aegis/message/role.h"

const char* aegis_message_role_str(aegis_message_role_t role)
{
    switch (role) {
    case AEGIS_MESSAGE_SYSTEM:
        return "system";
    case AEGIS_MESSAGE_USER:
        return "user";
    case AEGIS_MESSAGE_ASSISTANT:
        return "assistant";
    case AEGIS_MESSAGE_TOOL:
        return "tool";
    case AEGIS_MESSAGE_EVENT:
        return "event";
    case AEGIS_MESSAGE_SUMMARY:
        return "summary";
    default:
        return "unknown";
    }
}
