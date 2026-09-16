/**
 * @file test_skill_tools.c
 * @brief Unit tests for skill progressive-disclosure: registry find,
 *        the "use_skill" tool, and the skill-aware system-prompt builder.
 */
#define _POSIX_C_SOURCE 200809L
#include "aegis/coding/skill_tools.h"
#include "aegis/skill/registry.h"
#include "aegis/skill/skill.h"
#include "aegis/tool/tool.h"
#include "aegis/common/cancellation/cancellation.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void)
{
    /* Build a registry holding one demo skill. */
    aegis_skill_registry_t* reg = NULL;
    assert(aegis_skill_registry_create(&reg) == AEGIS_OK);
    aegis_skill_t* demo = NULL;
    assert(aegis_skill_create("demo", "Demo skill", "Full instructions for demo.", &demo) ==
           AEGIS_OK);
    assert(aegis_skill_registry_add(reg, demo) == AEGIS_OK);

    /* ── aegis_skill_registry_find ─────────────────────────────────── */
    const aegis_skill_t* found = NULL;
    assert(aegis_skill_registry_find(reg, "demo", &found) == AEGIS_OK);
    assert(found != NULL && strcmp(found->name, "demo") == 0);
    assert(aegis_skill_registry_find(reg, "nope", &found) == AEGIS_ERR_NOT_FOUND);

    /* ── build_coding_system_prompt ────────────────────────────────── */
    char* prompt = NULL;
    assert(build_coding_system_prompt(reg, &prompt) == AEGIS_OK);
    assert(prompt != NULL);
    assert(strstr(prompt, "- demo: Demo skill") != NULL);
    assert(strstr(prompt, "use_skill") != NULL);
    free(prompt);
    prompt = NULL;

    /* Empty registry: base prompt only, no disclosure block. */
    aegis_skill_registry_t* empty = NULL;
    assert(aegis_skill_registry_create(&empty) == AEGIS_OK);
    assert(build_coding_system_prompt(empty, &prompt) == AEGIS_OK);
    assert(prompt != NULL && strstr(prompt, "use_skill") == NULL);
    free(prompt);
    aegis_skill_registry_destroy(empty);
    prompt = NULL;

    /* ── use_skill tool ────────────────────────────────────────────── */
    aegis_tool_registry_t* tool_reg = NULL;
    assert(aegis_tool_registry_create(&tool_reg) == AEGIS_OK);
    skill_tools_ctx_t ctx        = {.skills = reg};
    aegis_tool_def_t* skill_tool = NULL;
    assert(aegis_coding_skill_tools_register(tool_reg, &ctx, &skill_tool) == AEGIS_OK);

    aegis_tool_args_t* args = NULL;
    assert(aegis_tool_args_create(&args) == AEGIS_OK);
    assert(aegis_tool_args_add_string(args, "name", "demo") == AEGIS_OK);
    aegis_cancellation_token_t* token = NULL;
    assert(aegis_cancellation_token_create(&token) == AEGIS_OK);

    aegis_tool_result_t result = {0};
    assert(aegis_tool_execute(tool_reg, "use_skill", args, token, &result) == AEGIS_OK);
    assert(result.value.type == AEGIS_TOOL_VAL_STRING);
    assert(strcmp(result.value.as.str.ptr, "Full instructions for demo.") == 0);
    aegis_tool_result_destroy(&result);

    /* Unknown skill name -> NOT_FOUND. */
    aegis_tool_args_destroy(args);
    args = NULL;
    assert(aegis_tool_args_create(&args) == AEGIS_OK);
    assert(aegis_tool_args_add_string(args, "name", "nope") == AEGIS_OK);
    result = (aegis_tool_result_t){0};
    assert(aegis_tool_execute(tool_reg, "use_skill", args, token, &result) == AEGIS_ERR_NOT_FOUND);

    aegis_tool_result_destroy(&result);
    aegis_tool_args_destroy(args);
    aegis_cancellation_token_destroy(token);
    aegis_coding_skill_tools_free(skill_tool, NULL);
    aegis_tool_registry_destroy(tool_reg);
    aegis_skill_registry_destroy(reg);
    printf("All skill tools tests PASS\n");
    return 0;
}
