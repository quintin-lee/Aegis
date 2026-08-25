#ifndef AEGIS_AGENT_H
#define AEGIS_AGENT_H

#include "aegis/types.h"
#include "aegis/status.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Create a new agent with the given name.
 *
 * @param[out] out   Pointer receiving the created agent handle. Ownership: transferred.
 * @param[in]  name  Agent name (must be non-NULL, non-empty). Borrowed.
 * @return AEGIS_OK on success, or an error code.
 */
aegis_status_t aegis_agent_create(aegis_agent_t** out, const char* name);

/**
 * @brief Destroy an agent and release all owned resources.
 *
 * Safe to call with NULL (no-op).
 *
 * @param agent Handle to destroy. After return, pointer is invalid. Ownership: consumed.
 */
void aegis_agent_destroy(aegis_agent_t* agent);

#ifdef __cplusplus
}
#endif

#endif /* AEGIS_AGENT_H */
