#ifndef AEGIS_STRUCTURED_OPENAI_H
#define AEGIS_STRUCTURED_OPENAI_H

#include "aegis/model/model.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct aegis_openai_model_ctx aegis_openai_model_ctx_t;

aegis_status_t aegis_openai_model_create(const char* api_key, const char* base_url,
                                         const char* model, aegis_openai_model_ctx_t** out,
                                         aegis_model_backend_t* backend);
void aegis_openai_model_destroy(aegis_openai_model_ctx_t* ctx);

#ifdef __cplusplus
}
#endif

#endif
