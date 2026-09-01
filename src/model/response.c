#define _POSIX_C_SOURCE 200809L
#include "aegis/model/response.h"
#include <stdlib.h>

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
