# Skill Progressive-Disclosure Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Close pi-agent gap #4 — expose the coding agent's loaded skill metadata in its system prompt and add a `use_skill` tool that loads one skill's full instructions on demand.

**Architecture:** Three small pieces. (1) A by-name lookup `aegis_skill_registry_find` added to the skill registry. (2) A new `src/coding/skill_tools.c` module mirroring `delegate_tools`: a heap-owned `use_skill` tool def carrying a `skill_tools_ctx_t` (borrowed skill registry), plus a `build_coding_system_prompt` helper that appends a skill-metadata block. (3) Wiring in `coding_agent.c`: a new `system_prompt_owned` field replacing the three static `CODING_AGENT_SYSTEM_PROMPT` refs, and register/free of the `use_skill` tool in create/destroy.

**Tech Stack:** C; existing `aegis_skill` + `aegis_tool` + `aegis_coding` modules. No new dependencies, no new CMake option.

---

## Spec

`docs/superpowers/specs/2026-09-14-skill-disclosure-design.md` (committed 03ac61c).

## Grounded API facts (verify against source before editing)

- `include/aegis/skill/registry.h`: `aegis_skill_registry_t` opaque; `aegis_skill_registry_create/destroy`, `aegis_skill_registry_add(reg, aegis_skill_t*)` (transfers ownership), `aegis_skill_registry_count(reg)->size_t`, `aegis_skill_registry_get(reg, idx)->const aegis_skill_t*` (borrowed). **No by-name lookup — to add.**
- `include/aegis/skill/skill.h`: `aegis_skill_t {char* name; char* description; char* instructions; char* path;}`; `aegis_skill_create(name, desc, instructions, out)` copies; `aegis_skill_destroy`.
- `src/skill/registry.c`: vector-backed; `struct aegis_skill_registry {aegis_vector_t* vec;}`; uses `aegis_vector_len/get`. The new `find` must use `count` + `get(i)` + `strcmp(get(i)->name, name)` — it has no direct access to `->vec` from the header, so implement it in `registry.c` (which sees the struct) using `aegis_skill_registry_count` + `aegis_skill_registry_get`.
- `src/skill/manifest.c`: SKILL.md first line = description (may be empty → NULL), remaining lines joined = instructions. Dir basename = skill name.
- `src/coding/delegate_tools.{c,h}`: the pattern to mirror — `aegis_tool_def_t` heap-allocated with `strdup`'d name/description + owned `aegis_tool_param_spec_t` array + `.execute` + `.user=ctx`; registered via `aegis_tool_registry_register`; freed with `(void*)` casts (the ABI fields are `const`).
- `include/aegis/tool/tool.h`: `aegis_tool_args_create(out)`, `aegis_tool_args_add_string(args,name,s)`, `aegis_tool_args_find(args,name,const aegis_tool_value_t** out)->bool`, `aegis_tool_result_set_string(result,s)`, `aegis_tool_execute(reg,name,args,token,out)`, `aegis_tool_registry_create/destroy/register`. `aegis_tool_value_t` for STRING: `.as.str.ptr`.
- `src/coding/coding_agent.c`:
  - L35 `#define CODING_AGENT_SYSTEM_PROMPT "You are a coding agent. Use tools to help the user."`
  - L50 field `aegis_skill_registry_t* skills;`
  - L56-57 fields `task_tool` / `subagent` (delegate pattern to follow).
  - L257-268 create() loads skills (best-effort); L277-278, L292-293 destroy them on owned-tools fail.
  - L306-317 subagent + delegate register block; L313 sets `subagent->system_prompt = CODING_AGENT_SYSTEM_PROMPT`.
  - L326 `lcfg.system_prompt = CODING_AGENT_SYSTEM_PROMPT;` (create loop cfg).
  - L341-343, L402-404 `aegis_coding_delegate_tools_free(a->task_tool, a->subagent)` in the two destroy paths.
  - L350-351, L380-381 `aegis_skill_registry_destroy(a->skills)` (owned-tools fail path + normal destroy).
  - L453, L557, L720 `.system_prompt = CODING_AGENT_SYSTEM_PROMPT,` in `replace_session` / `set_model_client` rebuilds.
- `src/coding/CMakeLists.txt` L2: `add_library(aegis_coding ... coding_agent.c delegate_tools.c)` → add `skill_tools.c`.
- `cmake/AegisTests.cmake`: `unit_delegate_tools` target links `aegis_coding aegis_agent aegis_session aegis_model aegis_tool aegis_common pthread` → mirror for `unit_skill_tools` (+ `aegis_skill`).
- `tests/integration/test_cli.c` L628: `assert_contains(out, "registered tools (13):", ...)` → the new `use_skill` tool bumps this to 14.

**Execution rule (user standing instruction, whole session):** NO subagents — execute this plan directly via executing-plans, in 3 chunks, verifying each with `cmake --build build --parallel 8` + `ctest`.

---

## Chunk 1: skill registry `find` + `skill_tools` module + `build_coding_system_prompt`

**Files:**
- Modify: `include/aegis/skill/registry.h`
- Modify: `src/skill/registry.c`
- Create: `include/aegis/coding/skill_tools.h`
- Create: `src/coding/skill_tools.c`
- Modify: `src/coding/CMakeLists.txt`

- [ ] **Step 1: Declare `aegis_skill_registry_find` in `include/aegis/skill/registry.h`.**

After the `aegis_skill_registry_get` line (L18), add:

```c
aegis_status_t aegis_skill_registry_find(const aegis_skill_registry_t* reg,
                                         const char* name,
                                         const aegis_skill_t** out);
```

- [ ] **Step 2: Implement it in `src/skill/registry.c`.**

Append (uses the public `count`/`get` so it works from within the .c):

```c
/**
 * @brief Find a skill by name (linear scan).
 *
 * @param[in]  reg  Registry (NULL → INVALID).
 * @param[in]  name Skill name to match (NULL → INVALID).
 * @param[out] out  Receives a borrowed pointer to the stored skill on match.
 * @return AEGIS_OK on match, AEGIS_ERR_NOT_FOUND if absent,
 *   AEGIS_ERR_INVALID for NULL args.
 */
aegis_status_t aegis_skill_registry_find(const aegis_skill_registry_t* reg,
                                         const char* name, const aegis_skill_t** out)
{
    if (!reg || !name || !out) {
        return AEGIS_ERR_INVALID;
    }
    *out = NULL;
    size_t n = aegis_skill_registry_count(reg);
    for (size_t i = 0; i < n; ++i) {
        const aegis_skill_t* s = aegis_skill_registry_get(reg, i);
        if (s && s->name && strcmp(s->name, name) == 0) {
            *out = s;
            return AEGIS_OK;
        }
    }
    return AEGIS_ERR_NOT_FOUND;
}
```

- [ ] **Step 3: Write `include/aegis/coding/skill_tools.h`.**

Mirror `delegate_tools.h`:

```c
/**
 * @file skill_tools.h
 * @brief The "use_skill" tool + coding-agent system-prompt builder.
 *
 * The tool definition is heap-owned by the caller (the coding agent) because
 * its execute fn needs the skill registry, which a const global cannot carry.
 */
#ifndef AEGIS_CODING_SKILL_TOOLS_H
#define AEGIS_CODING_SKILL_TOOLS_H

#include "aegis/skill/registry.h"
#include "aegis/tool/tool.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Context the use_skill execute fn reads; the registry is borrowed. */
typedef struct {
    aegis_skill_registry_t* skills; /**< Skill registry shared with the agent. */
} skill_tools_ctx_t;

/**
 * @brief Register a heap-allocated "use_skill" tool into @p reg.
 *
 * Fills @p out_def with a malloc'd aegis_tool_def_t (name/description/param
 * arrays heap-owned; the registry stores a shallow copy; .user is @p ctx).
 * The caller owns both the def and @p ctx.
 *
 * @return AEGIS_OK, or AEGIS_ERR_NOMEM / AEGIS_ERR_BUSY / AEGIS_ERR_INVALID.
 */
aegis_status_t aegis_coding_skill_tools_register(aegis_tool_registry_t* reg,
                                                 skill_tools_ctx_t*     ctx,
                                                 aegis_tool_def_t**     out_def);

/**
 * @brief Free a def + ctx from aegis_coding_skill_tools_register (NULL-safe).
 */
void aegis_coding_skill_tools_free(aegis_tool_def_t* def, skill_tools_ctx_t* ctx);

/**
 * @brief Build the coding-agent system prompt, embedding skill metadata.
 *
 * Returns a heap-allocated string: the base prompt
 * ("You are a coding agent. Use tools to help the user.") plus, when @p
 * skills is non-empty, a block:
 *
 *     Available skills (load full instructions with the "use_skill" tool):
 *     - <name>: <description>
 *
 * (one line per skill; an empty description renders as "- <name>:"). The
 * caller owns and frees the result. NULL @p skills → base prompt only.
 *
 * @return AEGIS_OK, or AEGIS_ERR_NOMEM / AEGIS_ERR_INVALID.
 */
aegis_status_t build_coding_system_prompt(aegis_skill_registry_t* skills, char** out);

#ifdef __cplusplus
}
#endif

#endif /* AEGIS_CODING_SKILL_TOOLS_H */
```

- [ ] **Step 4: Write `src/coding/skill_tools.c`.**

Mirror `delegate_tools.c`. Define `#define SKILL_TOOL_NAME "use_skill"`. Single param `name` (STRING, required). The execute fn:

```c
static aegis_status_t use_skill_tool_execute(void* user, const aegis_tool_args_t* args,
                                             const aegis_cancellation_token_t* token,
                                             aegis_tool_result_t* out);
```

Register (heap def, exactly like `aegis_coding_delegate_tools_register`): `strdup` name/desc; `calloc(1, ...)` one `aegis_tool_param_spec_t` `{.name=strdup("name"), .type=AEGIS_TOOL_VAL_STRING, .required=true, .description=strdup("Name of the skill to load full instructions for.")}`; def `.execute=use_skill_tool_execute`, `.user=ctx`; `aegis_tool_registry_register(reg, def)`; on failure call the free fn. Free fn mirrors the delegate one (`(void*)` casts on `def->schema.params[i].name/.description`, `def->schema.params`, `def->name`, `def->description`, then `free(def)`, `free(ctx)`).

Execute fn:

```c
static aegis_status_t use_skill_tool_execute(void* user, const aegis_tool_args_t* args,
                                             const aegis_cancellation_token_t* token,
                                             aegis_tool_result_t* out)
{
    (void)token;
    skill_tools_ctx_t* ctx = user;
    if (!ctx || !args || !out) {
        return AEGIS_ERR_INVALID;
    }
    const aegis_tool_value_t* nv = NULL;
    if (!aegis_tool_args_find(args, "name", &nv) || !nv ||
        nv->type != AEGIS_TOOL_VAL_STRING) {
        return AEGIS_ERR_INVALID;
    }
    const char* skill_name = nv->as.str.ptr;
    if (!skill_name) {
        return AEGIS_ERR_INVALID;
    }
    const aegis_skill_t* sk = NULL;
    aegis_status_t st = aegis_skill_registry_find(ctx->skills, skill_name, &sk);
    if (st != AEGIS_OK) {
        return st; /* NOT_FOUND or INVALID */
    }
    return aegis_tool_result_set_string(out, sk->instructions ? sk->instructions : "");
}
```

`build_coding_system_prompt`: grow-a-buffer helper (reuse the checked-grow pattern or `snprintf` into a dynamically-grown `char*`). Build:

```
"You are a coding agent. Use tools to help the user."
```
then, if `skills` non-NULL and `aegis_skill_registry_count(skills) > 0`, append:

```
\nAvailable skills (load full instructions with the "use_skill" tool):
```
and for each skill `i` a line `\n- <name>: <description>` (description may be NULL → empty). Return via `*out`; caller frees. NOMEM on any growth failure.

Includes: `aegis/coding/skill_tools.h`, `aegis/skill/registry.h`, `aegis/common/error.h`, `<stdlib.h>`, `<string.h>`, `<stdio.h>` (for `snprintf`). `#define _POSIX_C_SOURCE 200809L`.

- [ ] **Step 5: Add `skill_tools.c` to the `aegis_coding` target.**

`src/coding/CMakeLists.txt` L2:

```cmake
add_library(aegis_coding mutations.c coding_tools.c discovery_tools.c git_tools.c coding_agent.c delegate_tools.c skill_tools.c)
```

No link changes (`aegis_coding` already links `aegis_skill` + `aegis_tool`).

- [ ] **Step 6: Build — confirm it compiles.**

```bash
cmake -S . -B build && cmake --build build --target aegis_coding --parallel 8
```

Expected: clean. If `aegis_skill_registry_find` / `aegis_tool_args_find` signatures differ from the grounded notes above, fix the calls to match `include/aegis/skill/registry.h` and `include/aegis/tool/tool.h`.

- [ ] **Step 7: Commit the module.**

```bash
git add include/aegis/skill/registry.h src/skill/registry.c \
        include/aegis/coding/skill_tools.h src/coding/skill_tools.c \
        src/coding/CMakeLists.txt
git commit -m "feat(skill): add registry find + use_skill tool + system-prompt builder"
```

---

## Chunk 2: wire into the coding agent

**Files:**
- Modify: `src/coding/coding_agent.c`

- [ ] **Step 1: Add struct fields + include.**

In `src/coding/coding_agent.c`, add to the include block:

```c
#include "aegis/coding/skill_tools.h"
```

In `struct aegis_coding_agent` (after `subagent`, ~L57), add:

```c
    char*             system_prompt_owned; /**< Heap prompt incl. skill metadata. */
    aegis_tool_def_t* skill_tool;          /**< Heap "use_skill" tool def. */
    skill_tools_ctx_t* skill_ctx;         /**< Owned ctx blob backing skill_tool. */
```

- [ ] **Step 2: Build + register in `create()`.**

After the delegate register block (L306-317, which ends before `lcfg` is set at L326), insert:

```c
    st = build_coding_system_prompt(a->skills, &a->system_prompt_owned);
    if (st != AEGIS_OK) {
        goto fail;
    }
    a->skill_ctx = calloc(1, sizeof(*a->skill_ctx));
    if (!a->skill_ctx) {
        st = AEGIS_ERR_NOMEM;
        goto fail;
    }
    a->skill_ctx->skills = a->skills;
    st = aegis_coding_skill_tools_register(a->tools, a->skill_ctx, &a->skill_tool);
    if (st != AEGIS_OK) {
        free(a->skill_ctx);
        a->skill_ctx = NULL;
        goto fail;
    }
```

Replace the base prompt used at L313 (`a->subagent->system_prompt = CODING_AGENT_SYSTEM_PROMPT;`) with the owned prompt so the child inherits skill metadata too:

```c
    a->subagent->system_prompt = a->system_prompt_owned;
```

But note L313 runs *before* `system_prompt_owned` is built. Reorder: build `a->system_prompt_owned` FIRST (move `build_coding_system_prompt` above the subagent block, after skill load at L268 and before the subagent calloc), then set `a->subagent->system_prompt = a->system_prompt_owned;`.

- [ ] **Step 3: Replace the static prompt references with the owned prompt.**

- L326 `lcfg.system_prompt = CODING_AGENT_SYSTEM_PROMPT;` → `lcfg.system_prompt = a->system_prompt_owned;`
- L453, L557, L720 `.system_prompt = CODING_AGENT_SYSTEM_PROMPT,` → `.system_prompt = a->system_prompt_owned,`

(Confirm each site is reachable only after `create()` succeeded, i.e. `system_prompt_owned` is non-NULL.)

- [ ] **Step 4: Free in both destroy paths.**

In the owned-tools fail path (near L341-351, after `aegis_coding_delegate_tools_free` and before/around `aegis_skill_registry_destroy`), and in the normal `destroy()` (near L402-404):

```c
    aegis_coding_skill_tools_free(a->skill_tool, a->skill_ctx);
    free(a->system_prompt_owned);
    a->skill_tool = NULL;
    a->skill_ctx = NULL;
    a->system_prompt_owned = NULL;
```

Place it AFTER `aegis_coding_delegate_tools_free` and BEFORE `aegis_skill_registry_destroy(a->skills)` (the skill registry outlives the tool def; the tool def's ctx borrows it but the def is freed first). Also add the `fail:` label in `create()` to free these on the early-return path (mirror the delegate `fail:` teardown): before the existing `aegis_coding_delegate_tools_free(a->task_tool, a->subagent);` line, add `aegis_coding_skill_tools_free(a->skill_tool, a->skill_ctx); free(a->system_prompt_owned);` (NULL-safe: the free fn and `free(NULL)` are no-ops when unset).

- [ ] **Step 5: Build + run coding tests.**

```bash
cmake --build build --parallel 8 && cd build && ctest -R "coding" --output-on-failure
```

Expected: `system_coding_loop` and coding-agent tests pass (no-skill path returns the base prompt → unchanged behavior).

- [ ] **Step 6: Commit the wiring.**

```bash
git add src/coding/coding_agent.c
git commit -m "feat(coding): build skill-aware system prompt + register use_skill tool"
```

---

## Chunk 3: unit test + CLI count + final verify

**Files:**
- Create: `tests/unit/test_skill_tools.c`
- Modify: `tests/integration/test_cli.c` (L628 `13` → `14`)
- Modify: `cmake/AegisTests.cmake`

- [ ] **Step 1: Write `tests/unit/test_skill_tools.c` (plain C, no network).**

Build a skill registry with `aegis_skill_create` + `aegis_skill_registry_add` (no disk needed — avoids a temp SKILL.md dir):

```c
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
    aegis_skill_registry_t* reg = NULL;
    assert(aegis_skill_registry_create(&reg) == AEGIS_OK);
    aegis_skill_t* sk = NULL;
    assert(aegis_skill_create("demo", "Demo skill", "Full instructions for demo.", &sk) == AEGIS_OK);
    assert(aegis_skill_registry_add(reg, sk) == AEGIS_OK);

    /* ── find ── */
    const aegis_skill_t* found = NULL;
    assert(aegis_skill_registry_find(reg, "demo", &found) == AEGIS_OK);
    assert(found && found == sk);
    assert(strcmp(found->instructions, "Full instructions for demo.") == 0);
    aegis_status_t miss = aegis_skill_registry_find(reg, "nope", &found);
    assert(miss == AEGIS_ERR_NOT_FOUND);

    /* ── system-prompt builder: with skills ── */
    char* prompt = NULL;
    assert(build_coding_system_prompt(reg, &prompt) == AEGIS_OK);
    assert(strstr(prompt, "You are a coding agent") != NULL);
    assert(strstr(prompt, "use_skill") != NULL);
    assert(strstr(prompt, "- demo: Demo skill") != NULL);
    free(prompt);

    /* ── system-prompt builder: no skills (base prompt) ── */
    prompt = NULL;
    aegis_skill_registry_t* empty = NULL;
    assert(aegis_skill_registry_create(&empty) == AEGIS_OK);
    assert(build_coding_system_prompt(empty, &prompt) == AEGIS_OK);
    assert(strstr(prompt, "use_skill") == NULL); /* no skills -> no block */
    free(prompt);
    aegis_skill_registry_destroy(empty);

    /* ── use_skill tool: register + execute ── */
    skill_tools_ctx_t* ctx = calloc(1, sizeof(*ctx));
    ctx->skills = reg;
    aegis_tool_def_t* def = NULL;
    assert(aegis_coding_skill_tools_register(reg, ctx, &def) == AEGIS_OK);

    aegis_tool_args_t* args = NULL;
    assert(aegis_tool_args_create(&args) == AEGIS_OK);
    assert(aegis_tool_args_add_string(args, "name", "demo") == AEGIS_OK);
    aegis_cancellation_token_t* tok = NULL;
    assert(aegis_cancellation_token_create(&tok) == AEGIS_OK);
    aegis_tool_result_t result = {0};
    assert(aegis_tool_execute(reg, "use_skill", args, tok, &result) == AEGIS_OK);
    assert(result.value.type == AEGIS_TOOL_VAL_STRING);
    assert(strcmp(result.value.as.str.ptr, "Full instructions for demo.") == 0);
    aegis_tool_result_destroy(&result);
    aegis_tool_args_destroy(args);

    /* unknown skill → NOT_FOUND */
    aegis_tool_args_t* args2 = NULL;
    assert(aegis_tool_args_create(&args2) == AEGIS_OK);
    assert(aegis_tool_args_add_string(args2, "name", "nope") == AEGIS_OK);
    result = (aegis_tool_result_t){0};
    aegis_status_t rr = aegis_tool_execute(reg, "use_skill", args2, tok, &result);
    assert(rr == AEGIS_ERR_NOT_FOUND);
    aegis_tool_args_destroy(args2);

    aegis_cancellation_token_destroy(tok);
    aegis_coding_skill_tools_free(def, ctx);
    aegis_skill_registry_destroy(reg);
    printf("All skill tools tests PASS\n");
    return 0;
}
```

NOTE: verify `aegis_tool_result_destroy`, `aegis_tool_result.value.type`/`.as.str.ptr`, `aegis_tool_execute`, and `AEGIS_TOOL_VAL_STRING` against `include/aegis/tool/tool.h` before running; adjust field names if they differ. `build_coding_system_prompt` for the empty-registry case must NOT contain "use_skill" — that's why the block is only appended when `count > 0`.

- [ ] **Step 2: Register the test target in `cmake/AegisTests.cmake`.**

Near the `unit_delegate_tools` block, add (mirror its link line, plus `aegis_skill`):

```cmake
add_executable(unit_skill_tools tests/unit/test_skill_tools.c)
target_link_libraries(unit_skill_tools PRIVATE aegis_coding aegis_skill aegis_tool aegis_common aegis_agent aegis_session aegis_model pthread)
aegis_set_warnings(unit_skill_tools)
add_test(NAME unit_skill_tools COMMAND unit_skill_tools)
```

- [ ] **Step 3: Update the CLI tool-count assertion.**

`tests/integration/test_cli.c` L628: `assert_contains(out, "registered tools (13):", ...)` → `"registered tools (14):"` (the `use_skill` tool adds one).

- [ ] **Step 4: Build + run the new test.**

```bash
cmake -S . -B build && cmake --build build --parallel 8 && cd build && ctest -R unit_skill_tools --output-on-failure
```

Expected: PASS.

- [ ] **Step 5: Full regression.**

```bash
cd build && ctest --output-on-failure
```

Expected: all prior tests + `unit_skill_tools` pass; `system_coding_loop` / `unit_loop` / `integration_cli` still green.

- [ ] **Step 6: Format the new/changed C files (never the `.cmake`).**

```bash
clang-format -i src/skill/registry.c src/coding/skill_tools.c src/coding/coding_agent.c tests/unit/test_skill_tools.c
```

- [ ] **Step 7: Commit the test + count update.**

```bash
git add tests/unit/test_skill_tools.c tests/integration/test_cli.c cmake/AegisTests.cmake
git commit -m "test(skill): add use_skill + registry-find + prompt-builder tests"
```

---

## Verification Criteria (definition of done)

- [ ] `aegis_skill_registry_find` + `skill_tools.c` (`use_skill` tool + `build_coding_system_prompt`) + coding-agent wiring + unit test all committed.
- [ ] `unit_skill_tools` PASSES (find by name, prompt embeds metadata, use_skill returns instructions, unknown → NOT_FOUND).
- [ ] `system_coding_loop` + `unit_loop` + `integration_cli` still PASS (no regression; no-skill path returns base prompt; CLI count now 14).
- [ ] Full `ctest` green.
- [ ] `clang-format --dry-run` clean on the new/changed C files.

## Rollback

Each chunk is an independent commit; `git revert <sha>` to roll back. Chunk 2's wiring is guarded so `skill_tool`/`skill_ctx`/`system_prompt_owned` are NULL-safe in both destroy paths.
