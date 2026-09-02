#ifndef AEGIS_CODING_DISCOVERY_TOOLS_H
#define AEGIS_CODING_DISCOVERY_TOOLS_H

#include "aegis/tool/tool.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file discovery_tools.h
 * @brief Builtin discovery tools: list, glob, grep.
 *
 * All tools are project-root confined (relative paths only, no ".."),
 * skip .git directories during recursive walks, honor cooperative
 * cancellation between files, and cap results explicitly.
 */

extern const aegis_tool_def_t aegis_coding_tool_list;
extern const aegis_tool_def_t aegis_coding_tool_glob;
extern const aegis_tool_def_t aegis_coding_tool_grep;

aegis_status_t aegis_coding_discovery_tools_register_all(aegis_tool_registry_t* reg);

#ifdef __cplusplus
}
#endif

#endif /* AEGIS_CODING_DISCOVERY_TOOLS_H */
