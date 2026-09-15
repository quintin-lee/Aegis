# Sub-Agent / Task Delegation Design

**Date:** 2026-09-14
**Status:** Approved
**Closes pi-agent gap #5** (sub-agent/task delegation)

## 1. Goal

Let the coding agent delegate a focused sub-goal to a *child* agent that runs to
completion in an isolated session, then returns the child's final answer as a tool
result. This enables hierarchical planning ("do X, then Y") without the parent's
context window being flooded by every step of X.

## 2. The `task` tool

A new tool named **`task`** is registered into the coding agent's tool registry.

**Schema:**

| Param | Type | Required | Description |
|---|---|---|---|
| `goal` | string | yes | The concrete sub-goal for the child to accomplish. |
| `description` | string | no | Optional longer context for the child. |

The tool is a **heap-allocated `aegis_tool_def_t`** owned by the coding agent
(the registry stores a shallow copy, so the def's `.name`/`.description`/
`.schema.params` strings are heap-owned and freed on agent destroy). Its `.user`
points at a `subagent_ctx_t` blob (see §4).

**Module:** `src/coding/delegate_tools.c` + `src/coding/delegate_tools.h`.

## 3. Execution model (synchronous single child)

`task` runs **one** child agent **synchronously** to completion and returns its
final answer. No async/parallel delegation (YAGNI).

Execute flow (inside `task`'s `execute` fn):

1. Read `subagent_ctx_t` from `user`.
2. `aegis_session_create()` a **fresh child session** (isolated — the child sees
   none of the parent's history).
3. Build the **child tool registry** (§5 recursion guard): copy every parent tool
   except `task`.
4. Build child loop config:
   ```c
   aegis_agent_loop_config_t cfg = {
       .session           = child_session,
       .model             = ctx->model,          // shared, borrowed
       .tools             = child_tools,          // owned by execute fn
       .system_prompt     = ctx->system_prompt,
       .token             = /* fresh token */,
       .strategy          = NULL,                // reactive turn loop
       .max_strategy_turns= 10,                 // anti-runaway cap
   };
   aegis_agent_loop_create(&cfg, &child_loop);
   ```
5. `aegis_agent_loop_run_turn(child_loop, goal)` — synchronous; pumps until the
   child model stops requesting tools (or the 10-turn cap / cancellation).
6. Read the child session's **last assistant message** content:
   `aegis_session_message_at(child_session, count-1)` → `aegis_message_content()`.
7. `aegis_tool_result_set_string(out, that_content)`.
8. Destroy child loop, child session, child tool registry, child token.
9. Return `AEGIS_OK`, or propagate the `run_turn` failure status.

## 4. `subagent_ctx_t` blob

```c
typedef struct subagent_ctx {
    aegis_model_client_t*    model;        // borrowed (agent's model)
    aegis_tool_registry_t*   parent_tools; // borrowed (agent's registry)
    const char*              system_prompt;// borrowed (CODING_AGENT_SYSTEM_PROMPT)
} subagent_ctx_t;
```

Allocated in `aegis_coding_agent_create()` (heap, owned by the agent) and freed in
`aegis_coding_agent_destroy()`. The `task` tool def's `.user` points at it.

Public API in `delegate_tools.h`:

```c
/** Register the `task` tool into @p reg, using @p ctx (borrowed). */
aegis_status_t aegis_coding_delegate_tools_register(aegis_tool_registry_t* reg,
                                                    subagent_ctx_t* ctx);
/** Free the heap-owned `task` tool def + @p ctx. */
void aegis_coding_delegate_tools_free(aegis_tool_def_t* task_def, subagent_ctx_t* ctx);
```

(The `aegis_tool_def_t` is filled in place; the caller stores the pointer on the
agent struct and passes it back to `_free`.)

## 5. Recursion guard

The child's tool registry is a **separate registry** that re-registers every
parent tool **except `task`** itself:

```c
// visitor: re-register def unless def->name == "task"
aegis_tool_registry_visit(ctx->parent_tools, copy_tools_visitor, &child_tools);
```

This means the child cannot call `task` again — no infinite recursion. The child
has the same capability as the parent minus the delegator. Shallow copy is safe
because the tool defs' `user` pointers are shared. `child_tools` is owned by the
`task` execute fn and destroyed after the child loop. No allowlist param (YAGNI).

## 6. Coding-agent wiring

`src/coding/coding_agent.c` + `include/aegis/coding/coding_agent.h`:

- Struct gains **two unconditional fields**:
  - `aegis_tool_def_t* task_tool;` (heap-owned `task` def; NULL when tools are
    borrowed by the caller)
  - `subagent_ctx_t* subagent;` (owned ctx blob; freed in destroy)
- `create()`: after `aegis_coding_tools_register_all(a->tools, a->mq)`:
  ```c
  a->subagent = calloc(1, sizeof(*a->subagent));
  a->subagent->model        = a->model;
  a->subagent->parent_tools = a->tools;
  a->subagent->system_prompt= CODING_AGENT_SYSTEM_PROMPT;
  st = aegis_coding_delegate_tools_register(a->tools, a->subagent, &a->task_tool);
  // on failure: unwind (free task_tool + subagent before destroying registry)
  ```
- `destroy()`: call `aegis_coding_delegate_tools_free(a->task_tool, a->subagent)`
  **before** `aegis_tool_registry_destroy(a->tools)` (only when `owns_tools`);
  when the caller supplies a borrowed registry the agent still owns the def/ctx
  and frees them. Ordering: after `aegis_mcp_client_destroy` (if any), before the
  tool-registry destroy.

## 7. Testing

New plain-C test `tests/unit/test_delegate_tools.c` (assert + `printf` PASS +
`main`, matching repo style). Uses the **mock model**
(`aegis_model_client_create("mock")`) which deterministically returns
`"mock response to: <last user content>"` with no network, so the child loop
runs predictably.

Cases:
1. **Basic call:** parent registry with one dummy tool + `task`. Execute `task`
   with `goal="do X"` → assert the result string equals the child's final answer
   (`"mock response to: do X"`, since the child's user input is the goal).
2. **Recursion guard:** the call completes without infinite recursion (if the
   child could see `task`, the mock loop would run past the 10-turn cap and fail).
3. **Session isolation:** the child uses a fresh `aegis_session_create` with no
   parent history.
4. **Turn cap:** 10-turn default; the mock answers in one turn, so the cap is
   anti-runaway insurance.

CMake registration (`cmake/AegisTests.cmake`):

```cmake
add_executable(unit_delegate_tools tests/unit/test_delegate_tools.c)
target_link_libraries(unit_delegate_tools PRIVATE aegis_coding aegis_agent
                   aegis_session aegis_model aegis_tool aegis_common pthread)
aegis_set_warnings(unit_delegate_tools)
add_test(NAME unit_delegate_tools COMMAND unit_delegate_tools)
```

Regression: `unit_loop` / `system_coding_loop` must stay green.

## 8. Scope OUT

- Async / parallel / multiple concurrent children (YAGNI).
- Tool allowlist per call (child = full parent minus `task`).
- Streaming child events back to the parent (return only the final answer).
- Structured/typed return (plain string only).

## 9. Conventions

- English Doxygen on public API; `#ifndef` include guards; `aegis_` prefix;
  `aegis_status_t` returns; calloc ownership; create/destroy lifecycle.
- Plain-C tests via `cmake/AegisTests.cmake` + asan env property.
- `clang-format` on C files only (never `.cmake`).
