#ifndef AEGIS_STATUS_H
#define AEGIS_STATUS_H

#include "aegis/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Return a human-readable string for a status code.
 *
 * Return value points to a static string; do not free.
 */
const char *aegis_status_str(aegis_status_t status);

#ifdef __cplusplus
}
#endif

#endif /* AEGIS_STATUS_H */
