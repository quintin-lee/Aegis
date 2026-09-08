#ifndef AEGIS_CODING_GIT_TOOLS_H
#define AEGIS_CODING_GIT_TOOLS_H

#include "aegis/tool/tool.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file git_tools.h
 * @brief Builtin git tools: status, diff, commit, log, branch.
 *
 * All tools shell out to git via fork/exec (no shell interpolation).
 * Paths are project-root confined. Commit messages and branch names
 * are validated against shell metacharacters.
 *
 * Security model:
 *   - git_status, git_diff, git_log: read-only (AEGIS_CAP_READ_FILE)
 *   - git_commit: creates commits (AEGIS_CAP_SHELL | AEGIS_CAP_WRITE_FILE)
 *   - git_branch: list (READ_FILE), create/switch (SHELL)
 *   - All path inputs validated via aegis_safe_relative_path()
 *   - Commit messages and branch names sanitized against injection
 */

extern const aegis_tool_def_t aegis_coding_tool_git_status;
extern const aegis_tool_def_t aegis_coding_tool_git_diff;
extern const aegis_tool_def_t aegis_coding_tool_git_commit;
extern const aegis_tool_def_t aegis_coding_tool_git_log;
extern const aegis_tool_def_t aegis_coding_tool_git_branch;

/**
 * @brief Register all git tools into the given registry.
 *
 * @param reg Tool registry (borrowed).
 * @return AEGIS_OK on success, or a negative error code.
 */
aegis_status_t aegis_coding_git_tools_register_all(aegis_tool_registry_t* reg);

#ifdef __cplusplus
}
#endif

#endif /* AEGIS_CODING_GIT_TOOLS_H */
