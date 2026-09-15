/**
 * @file delegate_tools.h
 * @brief The "task" delegation tool: run a focused sub-goal on a child agent.
 *
 * The tool definition is heap-owned by the caller (typically the coding
 * agent) because its execute fn needs a model client and the parent tool
 * registry, which a const global cannot carry. The context blob holds both
 * (borrowed) plus the system prompt.
 */
#ifndef AEGIS_CODING_DELEGATE_TOOLS_H
#define AEGIS_CODING_DELEGATE_TOOLS_H

#include "aegis/model/model.h"
#include "aegis/tool/tool.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Context the task tool's execute fn reads; all pointers are borrowed.
 *
 * The caller (the coding agent) owns this blob and keeps it valid for the
 * lifetime of the registered tool.
 */
typedef struct {
    aegis_model_client_t* model;           /**< Model client shared with the parent. */
    aegis_tool_registry_t* parent_tools;   /**< Tools copied into the child (minus "task"). */
    const char*           system_prompt;   /**< Child system prompt (borrowed). */
} subagent_ctx_t;

/**
 * @brief Register a heap-allocated "task" tool into @p reg.
 *
 * Fills @p out_def with a malloc'd aegis_tool_def_t whose name/description/
 * param arrays are heap-owned (the registry stores a shallow copy) and whose
 * .user is @p ctx. The caller owns both the def and @p ctx and must release
 * them with @ref aegis_coding_delegate_tools_free.
 *
 * @param[in]  reg  Target tool registry (non-NULL).
 * @param[in]  ctx  Context blob the execute fn reads (borrowed).
 * @param[out] out_def Receives the owned tool def on success.
 * @return AEGIS_OK, or AEGIS_ERR_NOMEM / AEGIS_ERR_BUSY / AEGIS_ERR_INVALID.
 */
aegis_status_t aegis_coding_delegate_tools_register(aegis_tool_registry_t* reg,
                                                    subagent_ctx_t*        ctx,
                                                    aegis_tool_def_t**    out_def);

/**
 * @brief Free a def + ctx previously created by
 *        @ref aegis_coding_delegate_tools_register.
 *
 * NULL @p def and/or @p ctx are safe. The ctx is the blob itself (freed
 * here), distinct from the borrowed pointers it contains.
 */
void aegis_coding_delegate_tools_free(aegis_tool_def_t* def, subagent_ctx_t* ctx);

#ifdef __cplusplus
}
#endif

#endif /* AEGIS_CODING_DELEGATE_TOOLS_H */
