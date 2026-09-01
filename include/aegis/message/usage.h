#ifndef AEGIS_MESSAGE_USAGE_H
#define AEGIS_MESSAGE_USAGE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file usage.h
 * @brief Token usage for a model turn.
 */

typedef struct aegis_usage {
    uint32_t input_tokens;
    uint32_t output_tokens;
    uint32_t total_tokens;
} aegis_usage_t;

#ifdef __cplusplus
}
#endif

#endif /* AEGIS_MESSAGE_USAGE_H */
