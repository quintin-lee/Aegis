#ifndef AEGIS_STATUS_H
#define AEGIS_STATUS_H

#include "aegis/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file status.h
 * @brief Status code to human-readable string conversion.
 */

/**
 * @brief Return a human-readable string for a status code.
 *
 * Return value points to a static string; do not free.
 *
 * @param status Error status to stringify.
 * @return Static string description.
 */
const char* aegis_status_str(aegis_status_t status);

#ifdef __cplusplus
}
#endif

#endif /* AEGIS_STATUS_H */
