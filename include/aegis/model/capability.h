#ifndef AEGIS_MODEL_CAPABILITY_H
#define AEGIS_MODEL_CAPABILITY_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file capability.h
 * @brief Model capability flags.
 */

typedef enum aegis_model_capability {
    AEGIS_MODEL_CAP_TEXT         = (1u << 0),
    AEGIS_MODEL_CAP_TOOL_CALLING = (1u << 1),
    AEGIS_MODEL_CAP_STREAMING    = (1u << 2),
    AEGIS_MODEL_CAP_REASONING    = (1u << 3),
    AEGIS_MODEL_CAP_VISION       = (1u << 4),
} aegis_model_capability_t;

#ifdef __cplusplus
}
#endif

#endif /* AEGIS_MODEL_CAPABILITY_H */
