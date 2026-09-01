#ifndef AEGIS_CODING_TOOLS_H
#define AEGIS_CODING_TOOLS_H

#include "aegis/tool/tool.h"
#include "aegis/coding/mutations.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file coding_tools.h
 * @brief Builtin coding tools: read, write, edit, bash.
 */

// Tool definitions (borrowed, static lifetime)
extern const aegis_tool_def_t aegis_coding_tool_read;
extern const aegis_tool_def_t aegis_coding_tool_write;
extern const aegis_tool_def_t aegis_coding_tool_edit;
extern const aegis_tool_def_t aegis_coding_tool_bash;

// Helper to register all coding tools into a registry
aegis_status_t aegis_coding_tools_register_all(aegis_tool_registry_t*  reg,
                                               aegis_mutation_queue_t* mq);

#ifdef __cplusplus
}
#endif

#endif /* AEGIS_CODING_TOOLS_H */
