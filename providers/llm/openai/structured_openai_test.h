#ifndef AEGIS_STRUCTURED_OPENAI_TEST_H
#define AEGIS_STRUCTURED_OPENAI_TEST_H

#include "aegis/model/response.h"
#include "aegis/types.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

aegis_status_t aegis_openai_parse_complete_response(const char* json, size_t len,
                                                     aegis_model_response_t** out);

#ifdef __cplusplus
}
#endif

#endif
