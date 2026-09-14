/**
 * @file response.c
 * @brief Model response container: owned assistant message plus raw
 * provider payload. Create/destroy only; parsing lives in providers.
 */
#define _POSIX_C_SOURCE 200809L
#include "aegis/model/response.h"
#include <stdlib.h>

/**
 * @brief Allocate a zeroed model-response container.
 *
 * The container owns one assistant message (if set) and one raw provider
 * payload buffer; both are released by @ref aegis_model_response_destroy.
 *
 * @param[out] out Receives the new response; untouched on failure.
 * @return AEGIS_OK on success, AEGIS_ERR_INVALID for NULL @p out,
 *   AEGIS_ERR_NOMEM on allocation failure.
 */
aegis_status_t aegis_model_response_create(aegis_model_response_t** out)
{
    if (!out) {
        return AEGIS_ERR_INVALID;
    }
    aegis_model_response_t* r = (aegis_model_response_t*)calloc(1, sizeof(*r));
    if (!r) {
        return AEGIS_ERR_NOMEM;
    }
    *out = r;
    return AEGIS_OK;
}

/**
 * @brief Release a model-response container and all resources it owns.
 *
 * Destroys the owned assistant message (if any) and frees the raw
 * payload buffer. NULL is a no-op.
 *
 * @param[in] r Response to destroy, or NULL.
 */
void aegis_model_response_destroy(aegis_model_response_t* r)
{
    if (!r) {
        return;
    }
    if (r->message) {
        aegis_message_destroy(r->message);
    }
    free(r->raw);
    free(r);
}
