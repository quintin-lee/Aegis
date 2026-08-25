#include "aegis/status.h"

const char* aegis_status_str(aegis_status_t status)
{
    switch (status) {
    case AEGIS_OK:
        return "ok";
    case AEGIS_ERR_INTERNAL:
        return "internal_error";
    case AEGIS_ERR_NOMEM:
        return "out_of_memory";
    case AEGIS_ERR_INVALID:
        return "invalid_argument";
    case AEGIS_ERR_NOT_FOUND:
        return "not_found";
    case AEGIS_ERR_BUSY:
        return "busy";
    case AEGIS_ERR_TIMEOUT:
        return "timeout";
    case AEGIS_ERR_CANCELLED:
        return "cancelled";
    case AEGIS_ERR_PERM:
        return "permission_denied";
    case AEGIS_ERR_PROVIDER:
        return "provider_error";
    case AEGIS_ERR_TOOL:
        return "tool_error";
    default:
        return "unknown";
    }
}
