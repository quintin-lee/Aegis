/**
 * @file status.c
 * @brief Status-code-to-string mapping.
 *
 * Every aegis_status_t value has a corresponding static string.
 * The returned pointer is statically allocated and must not be freed.
 */
#include "aegis/status.h"

/**
 * @brief Map a status code to its canonical snake_case name.
 *
 * Every aegis_status_t value has a corresponding string; unknown values map
 * to "unknown".
 *
 * @param[in] status  Status code to name.
 *
 * @return Pointer to a static string; never NULL, must not be freed.
 */
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
    case AEGIS_ERR_MAX_ITERATIONS:
        return "max_iterations";
    case AEGIS_ERR_INVALID_STATE:
        return "invalid_state";
    case AEGIS_ERR_CONTEXT_OVERFLOW:
        return "context_overflow";
    case AEGIS_ERR_TOOL_VALIDATION:
        return "tool_validation";
    case AEGIS_ERR_MODEL_RATE_LIMIT:
        return "model_rate_limit";
    default:
        return "unknown";
    }
}
