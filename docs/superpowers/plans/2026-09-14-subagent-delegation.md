# Sub-Agent Task Delegation Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a `task` tool to the coding agent that delegates a focused sub-goal to a synchronous child agent loop with an isolated session, returning the child's final answer.

**Architecture:** New module `src/coding/delegate_tools.c` (+ internal header `include/aegis/coding/delegate_tools.h`) exposes a heap-allocated `task` tool whose `.user` is a `subagent_ctx_t` blob (model + parent tools + system prompt, all borrowed). Its execute fn builds a fresh child session, a child tool registry that copies all parent tools EXCEPT `task` (recursion guard), and a child reactive loop; runs it to completion; returns the child's last assistant message. The coding agent owns the ctx blob + tool def and frees them on destroy.

**Tech Stack:** C, existing `aegis_agent` / `aegis_tool` / `aegis_session` / `aegis_model` modules. No new dependencies. No new CMake option (delegation is core).

---

## Spec

`docs/superpowers/specs/2026-09-14-subagent-delegation-design.md` (committed 2cd8bb6)

## File Map

| File | Responsibility |
|---|---|
| `include/aegis/coding/delegate_tools.h` | Declare `subagent_ctx_t` + `aegis_coding_delegate_tools_register` / `aegis_coding_delegate_tools_free` |
| `src/coding/delegate_tools.c` | Implement the `task` tool (heap def + execute fn + child loop) |
| `src/coding/CMakeLists.txt` | Add `delegate_tools.c` to `aegis_coding` sources |
| `src/coding/coding_agent.c` | Struct fields + create() register + destroy() free |
| `tests/unit/test_delegate_tools.c` | Plain-C test (mock model, no network) |
| `cmake/AegisTests.cmake` | Register `unit_delegate_tools` target |

**Constraints:**
- English Doxygen on the public API; `#ifndef` guards; `aegis_` prefix; `aegis_status_t` returns; `calloc` ownership; `create`/`destroy` lifecycle.
- The tool def + ctx are HEAP-owned by the coding agent (NOT a const global, because the execute fn needs `model`/`parent_tools` which const globals cannot carry).
- Child tool registry = parent tools MINUS `task` (recursion guard).
- NO new CMake feature option; `aegis_coding` already links `aegis_agent`/`aegis_tool`/`aegis_session`/`aegis_common` which provide everything.
- Do NOT use subagents for execution (user standing instruction); execute directly via executing-plans.
- Verify each chunk with `cmake --build build --parallel 8` + `ctest`; clang-format ONLY C files, NEVER `.cmake`.

---

## Chunk 1: `task` tool module (`delegate_tools.c/.h`)

**Files:**
- Create: `include/aegis/coding/delegate_tools.h`
- Create: `src/coding/delegate_tools.c`
- Modify: `src/coding/CMakeLists.txt`

- [ ] **Step 1: Write `include/aegis/coding/delegate_tools.h`.**

```c
#ifndef AEGIS_CODING_DELEGATE_TOOLS_H
#define AEGIS_CODING_DELEGATE_TOOLS_H

#include "aegis/tool/tool.h"
#include "aegis/model/model.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file delegate_tools.h
 * @brief The "task" delegation tool: run a focused sub-goal on a child agent.
 *
 * The tool definition is heap-owned by the caller (typically the coding
 * agent) because its execute fn needs a model client and the parent tool
 * registry, which a const global cannot carry. The context blob holds both
 * (borrowed) plus the system prompt.
 */

/** Opaque context the execute fn reads; all pointers are borrowed. */
typedef struct {
    aegis_model_client_t* model;        /**< Model client shared with the parent. */
    aegis_tool_registry_t* parent_tools; /**< Tools copied into the child (minus "task"). */
    const char*           system_prompt; /**< Child system prompt. */
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
```

- [ ] **Step 2: Write `src/coding/delegate_tools.c`.**

```c
/**
 * @file delegate_tools.c
 * @brief The "task" tool: delegate a sub-goal to a synchronous child agent.
 */
#define _POSIX_C_SOURCE 200809L
#include "aegis/coding/delegate_tools.h"
#include "aegis/agent/loop.h"
#include "aegis/session/session.h"
#include "aegis/common/cancellation/cancellation.h"
#include "aegis/common/error.h"
#include <stdlib.h>
#include <string.h>

#define TASK_TOOL_NAME    "task"
#define TASK_MAX_TURNS    10

/* Forward decl of the execute fn (static, wired into the def). */
static aegis_status_t task_tool_execute(void* user, const aegis_tool_args_t* args,
                                        const aegis_cancellation_token_t* token,
                                        aegis_tool_result_t* out);

/* Visitor: re-register every parent tool EXCEPT "task" into the child registry. */
struct copy_ctx {
    aegis_tool_registry_t* child;
};

static aegis_status_t copy_tool_visitor(const aegis_tool_def_t* def, void* user)
{
    struct copy_ctx* cc = user;
    if (def && strcmp(def->name, TASK_TOOL_NAME) != 0) {
        return aegis_tool_registry_register(cc->child, def);
    }
    return AEGIS_OK; /* skip "task" (recursion guard) */
}

aegis_status_t aegis_coding_delegate_tools_register(aegis_tool_registry_t* reg,
                                                    subagent_ctx_t*        ctx,
                                                    aegis_tool_def_t**    out_def)
{
    if (!reg || !out_def) {
        return AEGIS_ERR_INVALID;
    }
    *out_def = NULL;

    /* Owned name/description/params arrays (registry is shallow). */
    char*                 name = strdup(TASK_TOOL_NAME);
    char*                 desc = strdup("Delegate a focused sub-goal to a child agent. "
                                        "The child has the same tools (except task) "
                                        "and an isolated context. Returns its final answer.");
    if (!name || !desc) {
        free(name);
        free(desc);
        return AEGIS_ERR_NOMEM;
    }
    aegis_tool_param_spec_t* params = calloc(2, sizeof(*params));
    if (!params) {
        free(name);
        free(desc);
        return AEGIS_ERR_NOMEM;
    }
    char* goal_name = strdup("goal");
    char* goal_desc = strdup("The specific sub-goal for the child to complete.");
    char* desc_name = strdup("description");
    char* desc_desc = strdup("Optional human context for the sub-goal.");
    if (!goal_name || !goal_desc || !desc_name || !desc_desc) {
        free(goal_name); free(goal_desc); free(desc_name); free(desc_desc);
        free(params);
        free(name);
        free(desc);
        return AEGIS_ERR_NOMEM;
    }
    params[0] = (aegis_tool_param_spec_t){
        .name        = goal_name,
        .type        = AEGIS_TOOL_VAL_STRING,
        .required    = true,
        .description = goal_desc,
    };
    params[1] = (aegis_tool_param_spec_t){
        .name        = desc_name,
        .type        = AEGIS_TOOL_VAL_STRING,
        .required    = false,
        .description = desc_desc,
    };

    aegis_tool_def_t* def = calloc(1, sizeof(*def));
    if (!def) {
        free(goal_name); free(goal_desc); free(desc_name); free(desc_desc);
        free(params);
        free(name);
        free(desc);
        return AEGIS_ERR_NOMEM;
    }
    def->name        = name;
    def->description = desc;
    def->schema.params = params;
    def->schema.param_count = 2;
    def->execute   = task_tool_execute;
    def->user      = ctx;

    aegis_status_t st = aegis_tool_registry_register(reg, def);
    if (st != AEGIS_OK) {
        free(def->schema.params[0].name);
        free(def->schema.params[0].description);
        free(def->schema.params[1].name);
        free(def->schema.params[1].description);
        free(def->schema.params);
        free(def->name);
        free(def->description);
        free(def);
        return st;
    }
    *out_def = def;
    return AEGIS_OK;
}

void aegis_coding_delegate_tools_free(aegis_tool_def_t* def, subagent_ctx_t* ctx)
{
    if (def) {
        if (def->schema.params) {
            for (size_t i = 0; i < def->schema.param_count; ++i) {
                free(def->schema.params[i].name);
                free(def->schema.params[i].description);
            }
            free(def->schema.params);
        }
        free(def->name);
        free(def->description);
        free(def);
    }
    free(ctx);
}

static aegis_status_t task_tool_execute(void* user, const aegis_tool_args_t* args,
                                        const aegis_cancellation_token_t* token,
                                        aegis_tool_result_t* out)
{
    (void)token;
    subagent_ctx_t* ctx = user;
    if (!ctx || !args || !out) {
        return AEGIS_ERR_INVALID;
    }
    /* Read "goal" (required). */
    const aegis_tool_value_t* gv = NULL;
    if (!aegis_tool_args_find(args, "goal", &gv) || !gv ||
        gv->type != AEGIS_TOOL_VAL_STRING) {
        return AEGIS_ERR_INVALID;
    }
    const char* goal = gv->as.str.ptr;

    /* (2) Fresh isolated child session. */
    aegis_session_t* child_session = NULL;
    aegis_status_t   st = aegis_session_create("/tmp", &child_session);
    if (st != AEGIS_OK) {
        return st;
    }

    /* (3) Child tool registry = parent tools minus "task". */
    aegis_tool_registry_t* child_tools = NULL;
    st = aegis_tool_registry_create(&child_tools);
    if (st != AEGIS_OK) {
        aegis_session_destroy(child_session);
        return st;
    }
    if (ctx->parent_tools) {
        struct copy_ctx cc = {.child = child_tools};
        st = aegis_tool_registry_visit(ctx->parent_tools, copy_tool_visitor, &cc);
        if (st != AEGIS_OK) {
            aegis_tool_registry_destroy(child_tools);
            aegis_session_destroy(child_session);
            return st;
        }
    }

    /* (4) Child loop with a fresh token, reactive strategy, capped turns. */
    aegis_cancellation_token_t* child_token = NULL;
    st = aegis_cancellation_token_create(&child_token);
    if (st != AEGIS_OK) {
        aegis_tool_registry_destroy(child_tools);
        aegis_session_destroy(child_session);
        return st;
    }
    aegis_agent_loop_config_t ccfg;
    memset(&ccfg, 0, sizeof(ccfg));
    ccfg.session           = child_session;
    ccfg.model             = ctx->model;
    ccfg.tools             = child_tools;
    ccfg.system_prompt     = ctx->system_prompt;
    ccfg.token             = child_token;
    ccfg.strategy          = NULL; /* reactive */
    ccfg.max_strategy_turns = TASK_MAX_TURNS;

    aegis_agent_loop_t* child_loop = NULL;
    st = aegis_agent_loop_create(&ccfg, &child_loop);
    if (st != AEGIS_OK) {
        aegis_cancellation_token_destroy(child_token);
        aegis_tool_registry_destroy(child_tools);
        aegis_session_destroy(child_session);
        return st;
    }

    /* (5) Run the child synchronously. */
    aegis_status_t run_st = aegis_agent_loop_run_turn(child_loop, goal);

    /* (6) Read the child's last message content as the answer. */
    size_t n = aegis_session_message_count(child_session);
    const char* answer = "";
    if (n > 0) {
        const aegis_message_t* last = aegis_session_message_at(child_session, n - 1);
        if (last) {
            answer = aegis_message_content(last) ? aegis_message_content(last) : "";
        }
    }
    st = aegis_tool_result_set_string(out, answer);
    if (st != AEGIS_OK) {
        /* cleanup below, then report the alloc failure */
    }

    /* (7) Tear down the child. */
    aegis_agent_loop_destroy(child_loop);
    aegis_session_destroy(child_session);
    aegis_tool_registry_destroy(child_tools);
    aegis_cancellation_token_destroy(child_token);

    /* (8) Propagate the run failure, but only if the result was set. */
    if (st != AEGIS_OK) {
        return st;
    }
    return run_st;
}
```

Notes:
- `aegis_tool_registry_visit` returns the first non-OK from the visitor; `copy_tool_visitor` returns `AEGIS_OK` for skipped `task` and the `aegis_tool_registry_register` status otherwise, so a registration failure aborts and propagates.
- `task_tool_execute` sets the result string *before* tearing down, so a successful run always has its answer in `out` even if `run_st` is non-OK; the caller (agent loop) inspects `run_st`.

- [ ] **Step 3: Add `delegate_tools.c` to the `aegis_coding` target.**

Edit `src/coding/CMakeLists.txt` line 2:

```cmake
add_library(aegis_coding mutations.c coding_tools.c discovery_tools.c git_tools.c coding_agent.c)
```

becomes:

```cmake
add_library(aegis_coding mutations.c coding_tools.c discovery_tools.c git_tools.c coding_agent.c delegate_tools.c)
```

`aegis_coding` already links `aegis_agent aegis_tool aegis_session aegis_common aegis_skill` (line 5); `aegis_model` comes transitively via `aegis_agent`. No link changes needed.

- [ ] **Step 4: Build — confirm it compiles.**

```bash
cmake -S . -B build && cmake --build build --target aegis_coding --parallel 8
```

Expected: clean. If `aegis_tool_registry_visit` / `aegis_tool_args_find` signatures differ, fix the calls to match `include/aegis/tool/tool.h`.

- [ ] **Step 5: Commit the module.**

```bash
git add include/aegis/coding/delegate_tools.h src/coding/delegate_tools.c src/coding/CMakeLists.txt
git commit -m "feat(coding): add 'task' delegation tool module (child agent, recursion guard)"
```

---

## Chunk 2: Wire into the coding agent

**Files:**
- Modify: `src/coding/coding_agent.c`

- [ ] **Step 1: Add struct fields + include.**

At the top of `src/coding/coding_agent.c` include block, add:

```c
#include "aegis/coding/delegate_tools.h"
```

In `struct aegis_coding_agent` (after the `owns_tools` field, ~line 54), add:

```c
    aegis_tool_def_t* task_tool;  /**< Heap "task" tool def; NULL when unset. */
    subagent_ctx_t*   subagent;   /**< Owned ctx blob backing task_tool. */
```

- [ ] **Step 2: Register in `create()`.**

After the MCP block (which ends ~line 301 with `#endif`) and BEFORE the `aegis_agent_loop_config_t lcfg;` line (~line 303), insert:

```c
    a->subagent = calloc(1, sizeof(*a->subagent));
    if (!a->subagent) {
        st = AEGIS_ERR_NOMEM;
        goto unwind_mcp;
    }
    a->subagent->model        = a->model;
    a->subagent->parent_tools = a->tools;
    a->subagent->system_prompt = CODING_AGENT_SYSTEM_PROMPT;
    st = aegis_coding_delegate_tools_register(a->tools, a->subagent, &a->task_tool);
    if (st != AEGIS_OK) {
        free(a->subagent);
        a->subagent = NULL;
        goto unwind_mcp;
    }

#ifdef AEGIS_MCP
    goto unwind_mcp;
unwind_mcp:
#endif
```

Cleaner without a label — restructure to a direct unwind matching the surrounding style. The two failure paths need the same teardown as the MCP failure above (skills/mq/tools-if-owned + model + session + free(a)). Write it inline:

```c
    a->subagent = calloc(1, sizeof(*a->subagent));
    if (!a->subagent) {
        st = AEGIS_ERR_NOMEM;
        goto fail;
    }
    a->subagent->model         = a->model;
    a->subagent->parent_tools  = a->tools;
    a->subagent->system_prompt = CODING_AGENT_SYSTEM_PROMPT;
    st = aegis_coding_delegate_tools_register(a->tools, a->subagent, &a->task_tool);
    if (st != AEGIS_OK) {
        free(a->subagent);
        a->subagent = NULL;
        goto fail;
    }

    /* fall through to loop creation */

fail:
#ifdef AEGIS_MCP
    if (a->mcp_client) {
        aegis_mcp_client_destroy(a->mcp_client);
    }
#endif
    if (a->owns_tools) {
        if (a->skills) {
            aegis_skill_registry_destroy(a->skills);
        }
        aegis_mutation_queue_destroy(a->mq);
        aegis_tool_registry_destroy(a->tools);
    }
    aegis_model_client_destroy(a->model);
    aegis_session_destroy(a->session);
    free(a);
    return st;
```

IMPORTANT: the `fail:` label must sit where the existing post-MCP error-handling already lives. To avoid duplicating teardown, refactor so the existing MCP-failure unwinds and this new block share one `fail:` label. The cleanest approach: move the MCP block's two inner unwinds to a shared `fail:` label and let the new subagent block `goto fail` too. Keep behavior identical (the MCP client is only freed if created).

- [ ] **Step 3: Free in `destroy()`.**

In `aegis_coding_agent_destroy` (~line 348), after the MCP-client destroy block (~line 373-377) and BEFORE `if (a->session)`, insert:

```c
    aegis_coding_delegate_tools_free(a->task_tool, a->subagent);
    a->task_tool = NULL;
    a->subagent  = NULL;
```

This frees the task def's owned strings/params + the ctx blob. Placed after the loop destroy (line 353-355) so no live loop references the tools; before the owned-tools-registry destroy so the (shallow) registry no longer points at the freed def.

- [ ] **Step 4: Build + run coding tests.**

```bash
cmake --build build --parallel 8 && cd build && ctest -R "coding" --output-on-failure
```

Expected: clean build, `system_coding_loop` (and any coding-agent tests) still pass.

- [ ] **Step 5: Commit the wiring.**

```bash
git add src/coding/coding_agent.c
git commit -m "feat(coding): wire 'task' tool into coding agent lifecycle"
```

---

## Chunk 3: Unit test

**Files:**
- Create: `tests/unit/test_delegate_tools.c`
- Modify: `cmake/AegisTests.cmake`

- [ ] **Step 1: Write `tests/unit/test_delegate_tools.c` (plain C, mock model).**

```c
/**
 * @file test_delegate_tools.c
 * @brief Unit test for the "task" delegation tool (mock model, no network).
 */
#define _POSIX_C_SOURCE 200809L
#include "aegis/coding/delegate_tools.h"
#include "aegis/agent/loop.h"
#include "aegis/tool/tool.h"
#include "aegis/model/model.h"
#include "aegis/common/cancellation/cancellation.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    /* Mock model: deterministic "mock response to: <last user content>". */
    aegis_model_client_t* model = NULL;
    assert(aegis_model_client_create("mock", &model) == AEGIS_OK);

    /* Parent registry with one dummy tool + the "task" tool. */
    aegis_tool_registry_t* reg = NULL;
    assert(aegis_tool_registry_create(&reg) == AEGIS_OK);

    subagent_ctx_t* ctx = calloc(1, sizeof(*ctx));
    assert(ctx);
    ctx->model         = model;
    ctx->parent_tools  = reg;
    ctx->system_prompt = "You are a focused sub-agent. Complete the goal.";

    aegis_tool_def_t* task = NULL;
    assert(aegis_coding_delegate_tools_register(reg, ctx, &task) == AEGIS_OK);

    /* Build args: goal = "do X". */
    aegis_tool_args_t* args = NULL;
    assert(aegis_tool_args_create(&args) == AEGIS_OK);
    assert(aegis_tool_args_add_string(args, "goal", "do X") == AEGIS_OK);

    aegis_cancellation_token_t* token = NULL;
    assert(aegis_cancellation_token_create(&token) == AEGIS_OK);

    aegis_tool_result_t result = {0};
    aegis_status_t st = aegis_tool_execute(reg, "task", args, token, &result);
    assert(st == AEGIS_OK);
    assert(result.value.type == AEGIS_TOOL_VAL_STRING);
    /* Child runs one reactive turn with user input "do X" on the mock model,
     * whose final assistant message is "mock response to: do X". */
    assert(strcmp(result.value.as.str.ptr, "mock response to: do X") == 0);

    aegis_tool_result_destroy(&result);
    aegis_tool_args_destroy(args);
    aegis_cancellation_token_destroy(token);
    aegis_coding_delegate_tools_free(task, ctx);
    aegis_tool_registry_destroy(reg);
    aegis_model_client_destroy(model);
    printf("All delegate tools tests PASS\n");
    return 0;
}
```

Notes:
- `aegis_tool_execute(reg, "task", args, token, &result)` resolves the tool from the registry and invokes `task_tool_execute` with `.user = ctx`.
- The mock model's canned response is `"mock response to: %s"` of the LAST user message content, which is the goal string `"do X"` — so the child's final assistant message content is exactly `"mock response to: do X"`. Verify the mock's exact wording in `src/model/model.c` `aegis_model_complete` and adjust the assertion if it differs.

- [ ] **Step 2: Register the test target in `cmake/AegisTests.cmake`.**

Add (near the other unit tests):

```cmake
add_executable(unit_delegate_tools tests/unit/test_delegate_tools.c)
target_link_libraries(unit_delegate_tools PRIVATE aegis_coding aegis_agent aegis_session aegis_model aegis_tool aegis_common pthread)
aegis_set_warnings(unit_delegate_tools)
add_test(NAME unit_delegate_tools COMMAND unit_delegate_tools)
```

- [ ] **Step 3: Build + run the new test.**

```bash
cmake -S . -B build && cmake --build build --parallel 8 && cd build && ctest -R unit_delegate_tools --output-on-failure
```

Expected: PASS.

- [ ] **Step 4: Run the full suite for regressions.**

```bash
cd build && ctest --output-on-failure
```

Expected: all prior tests + `unit_delegate_tools` pass.

- [ ] **Step 5: Format the new/changed C files (never the .cmake).**

```bash
clang-format -i src/coding/delegate_tools.c tests/unit/test_delegate_tools.c
```

- [ ] **Step 6: Commit the test.**

```bash
git add tests/unit/test_delegate_tools.c cmake/AegisTests.cmake src/coding/delegate_tools.c tests/unit/test_delegate_tools.c
git commit -m "test(coding): add unit test for the 'task' delegation tool"
```

---

## Verification Criteria (definition of done)

- [ ] `task` tool module + coding-agent wiring + unit test all committed.
- [ ] `unit_delegate_tools` PASSES (child loop runs on mock model, returns final answer).
- [ ] `system_coding_loop` + `unit_loop` still PASS (no regression).
- [ ] Full `ctest` green.
- [ ] `clang-format --dry-run` clean on the new/changed C files.
- [ ] Recursion guard verified: child registry lacks `task` (implicit via the passing test + code review of `copy_tool_visitor`).

## Rollback

Each chunk is an independent commit; `git revert <sha>` to roll back. Chunk 2's wiring is guarded so `task_tool`/`subagent` are NULL-safe in destroy.
