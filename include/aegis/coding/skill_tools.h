/**
 * @file skill_tools.h
 * @brief The "use_skill" delegation tool + skill-aware system-prompt builder.
 *
 * Mirrors delegate_tools.h: a heap-allocated tool def whose execute fn reads a
 * context blob (the skill registry, borrowed) to load one skill's full
 * instructions on demand. build_coding_system_prompt embeds skill metadata so
 * the model knows which skills are available and how to load them.
 */
#ifndef AEGIS_CODING_SKILL_TOOLS_H
#define AEGIS_CODING_SKILL_TOOLS_H

#include "aegis/skill/registry.h"
#include "aegis/tool/tool.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Opaque context the use_skill execute fn reads; the registry is borrowed. */
typedef struct {
    aegis_skill_registry_t* skills; /**< Skill registry shared with the agent. */
} skill_tools_ctx_t;

/**
 * @brief Register a heap-allocated "use_skill" tool into @p reg.
 *
 * Fills @p out_def with a malloc'd aegis_tool_def_t whose name/description and
 * param array are heap-owned (the registry stores a shallow copy) and whose
 * .user is @p ctx. The caller owns both and releases them with
 * @ref aegis_coding_skill_tools_free.
 *
 * @param[in]  reg  Target tool registry (non-NULL).
 * @param[in]  ctx  Context blob the execute fn reads (borrowed registry).
 * @param[out] out_def Receives the owned tool def on success.
 * @return AEGIS_OK, or AEGIS_ERR_NOMEM / AEGIS_ERR_BUSY / AEGIS_ERR_INVALID.
 */
aegis_status_t aegis_coding_skill_tools_register(aegis_tool_registry_t* reg,
                                                 skill_tools_ctx_t* ctx, aegis_tool_def_t** out_def);

/**
 * @brief Free a def + ctx created by @ref aegis_coding_skill_tools_register.
 *
 * NULL @p def and/or @p ctx are safe. Frees the def's owned strings/params,
 * then the ctx blob.
 */
void aegis_coding_skill_tools_free(aegis_tool_def_t* def, skill_tools_ctx_t* ctx);

/**
 * @brief Build a skill-aware coding-agent system prompt.
 *
 * Produces a heap-allocated string: the base coding-agent prompt plus, when
 * @p skills is non-empty, a block listing each available skill as
 * "- <name>: <description>" with a pointer to the "use_skill" tool. The caller
 * owns and frees the returned string.
 *
 * @param[in]  skills Loaded skill registry (NULL → base prompt only).
 * @param[out] out    Receives the malloc'd prompt on success.
 * @return AEGIS_OK, or AEGIS_ERR_NOMEM.
 */
aegis_status_t build_coding_system_prompt(aegis_skill_registry_t* skills, char** out);

#ifdef __cplusplus
}
#endif

#endif /* AEGIS_CODING_SKILL_TOOLS_H */
