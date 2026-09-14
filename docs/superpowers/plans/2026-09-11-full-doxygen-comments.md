# Full-Source English Doxygen Comments Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add complete English Doxygen comments to every C source file in the repo without changing any behavior.

**Architecture:** Comment-only edits, one chunk per subsystem with disjoint file sets so chunks run in parallel. Public headers in `include/aegis/` already carry full Doxygen and are NOT rewritten (single known anomaly fix only). Each `.c` definition gets an implementation-side doc block; static helpers get full `@brief/@param/@return`; tricky branches get short `//` notes.

**Tech Stack:** C11, CMake build in `build/`, GTest via `ctest --test-dir build`, existing Doxygen style (`@file/@brief/@param/@return`, `/**<` trailing field docs, `/* ── Section ── */` separators).

---

## Global Comment Standard (applies to ALL chunks)

1. **File header:** every `.c`/`.h` keeps `/** @file <name> @brief <one line> ... */`. Only file missing it is `src/internal/memory_helpers.h` — Chunk 1 adds it.
2. **Exported function definitions** (`aegis_*` in `.c`): prepend `/** @brief ... @param... @return... */` describing implementation specifics (locking, allocation, ownership transfer, error paths). Mirror the header's contract, do not contradict it. Keep `@param[in/out]` direction tags consistent with the header.
3. **Static helpers** (`static ...` in `.c`): full `@brief/@param/@return` — this is the main gap (e.g. `is_terminal`, `emit_state_change` in `src/agent/agent.c`; `next_task_id` in `src/task/task.c`; `clone_item`/`free_item`/`cmp_item_by_priority_desc` in `src/internal/memory_helpers.h`).
4. **Structs/fields/macros/enums defined in `.c` or internal headers:** document each field with `/**< ... */` or a `//` note where a trailing doc is impractical.
5. **Complex logic:** 1–2 line `//` comments on non-obvious branches (retry backoff, state-transition guards, cleanup chains on error paths). No comment on self-evident code.
6. **Language:** English only. Style: match neighbors (section separators, `Ownership:`/`Thread-safe:` notes).
7. **Forbidden:** zero functional changes — no renames, no reordering, no logic edits, no reformatting unrelated lines. If a doc anomaly is found in a public header (beyond the one listed in Chunk 8), do NOT fix it; report it.

## Global Verification (every chunk)

```bash
cmake --build build -j$(nproc)   # expected: exit 0, no new warnings
git diff --stat                  # expected: only your chunk's files, insertions of comment lines only
```

Final acceptance (Chunk 9): full build + `ctest --test-dir build --output-on-failure` passes; `git diff` contains no non-comment line changes (spot-check with `git diff -U0 | grep '^[+-]' | grep -v '^[+-][+-]' | grep -vE '^[+-]\s*(\*|/|$)‘` — every added/removed line must be inside a comment).

---

## Chunk 1: Foundation (src/common + one internal header fix)

**Files:**
- Modify: `src/common/allocator.c`, `src/common/atomic.c`, `src/common/buffer.c`, `src/common/cancellation/cancellation.c`, `src/common/error.c`, `src/common/hashmap.c`, `src/common/list.c`, `src/common/mutex.c`, `src/common/queue.c`, `src/common/result.c`, `src/common/string.c`, `src/common/thread.c`, `src/common/time.c`, `src/common/vector.c`, `src/common/uuid.c`
- Modify: `src/internal/memory_helpers.h` (add missing `@file/@brief` header + docs for `clone_item`, `free_item`, `cmp_item_by_priority_desc`)
- Reference (read-only): corresponding headers under `include/aegis/common/` for contract wording.

- [ ] **Step 1: Read each `.c` file and its public header** to learn the contract and spot undocumented statics/fields.
- [ ] **Step 2: Add `@file`-level detail only where missing** (`memory_helpers.h`); leave existing `@file` blocks untouched.
- [ ] **Step 3: Add doc blocks to every undocumented function definition and static helper** per the Global Standard.
- [ ] **Step 4: Document undocumented struct fields / macros** with trailing `/**< ... */`.
- [ ] **Step 5: Build and check diff**

Run: `cmake --build build -j$(nproc)`
Expected: exit 0, no new warnings.

Run: `git diff --stat`
Expected: only Chunk 1 files listed.

## Chunk 2: Core lifecycle (src/agent + src/task + src/event)

**Files:**
- Modify: `src/agent/agent.c`, `src/agent/loop.c`, `src/agent/state.c`, `src/task/task.c`, `src/task/dependency.c`, `src/task/graph.c`, `src/event/event.c`, `src/event/event_bus.c`
- Reference (read-only): `include/aegis/agent/*.h`, `include/aegis/task/*.h`, `include/aegis/event/event.h`, `src/internal/task_internal.h`, `src/internal/event_internal.h`, `src/internal/lifecycle.h`

- [ ] **Step 1: Read each `.c` plus its header/internal header** (note the state-transition tables in `agent.c`/`task.c` — implementation docs should reference transition edges, not restate the whole table).
- [ ] **Step 2: Document all static helpers** (e.g. `is_terminal`, `emit_state_change`, `next_task_id`) with full `@brief/@param/@return`.
- [ ] **Step 3: Document every exported definition** with implementation-side notes (mutex scope, abort-on-active-state in destroy paths, atomic ID generation).
- [ ] **Step 4: Annotate non-obvious branches** (terminal-state guards, retry/wait edges) with short `//` comments.
- [ ] **Step 5: Build and check diff**

Run: `cmake --build build -j$(nproc)`
Expected: exit 0, no new warnings.

## Chunk 3: Runtime flow (context/session/planner/executor/scheduler/strategy)

**Files:**
- Modify: `src/context/context.c`, `src/session/session.c`, `src/planner/plan.c`, `src/planner/planner.c`, `src/executor/executor.c`, `src/scheduler/scheduler.c`, `src/strategy/autonomous_strategy.c`, `src/strategy/strategy_registry.c`
- Note: `src/context/context.c` + `src/session/session.c` + `src/planner/*` + `src/executor/*` + `src/scheduler/*` + `src/strategy/*` = 8 `.c` files in these dirs (verify with `find src/context src/session src/planner src/executor src/scheduler src/strategy -name '*.c'` and cover all of them).
- Reference (read-only): matching headers under `include/aegis/` + `src/internal/*_internal.h`.

- [ ] **Step 1: Read each `.c` and its contracts.**
- [ ] **Step 2: Document statics and exported definitions** per Global Standard (scheduling policy, claim/check-then-act guards, plan-validation rules deserve explicit notes).
- [ ] **Step 3: Annotate concurrency-critical sections** (claim loops, lock scopes) with `//` notes.
- [ ] **Step 4: Build and check diff**

Run: `cmake --build build -j$(nproc)`
Expected: exit 0.

## Chunk 4: Autonomous subsystem

**Files:**
- Modify all `.c`/internal `.h` under `src/autonomous/`: `autonomous_agent.c`, `autonomous_state.c`, `checkpoint.c`, `evaluation.c`, `execution.c`, `lifecycle.c`, `loop.c`, `memory_mock.c`, `planning.c`, `recovery.c`, `reflection.c`, `replanning.c`, `state_machine.c`, `src/shell.c`, `src/status.c` (verify exact list with `find src/autonomous -name '*.c' -o -name '*.h'`), plus `autonomous_agent_internal.h` if it lacks field docs.
- Reference (read-only): `include/aegis/autonomous_agent.h`, `include/aegis/autonomous_state.h`.

- [ ] **Step 1: Read all files in the subsystem** (loop/state-machine files first for vocabulary, then the rest).
- [ ] **Step 2: Document statics and exported definitions** per Global Standard (phase transitions, recovery policy, evaluation criteria).
- [ ] **Step 3: Build and check diff**

Run: `cmake --build build -j$(nproc)`
Expected: exit 0.

## Chunk 5: Coding subsystem (largest files)

**Files:**
- Modify: `src/coding/coding_agent.c`, `src/coding/coding_tools.c`, `src/coding/discovery_tools.c`, `src/coding/git_tools.c`, `src/coding/mutations.c`, `src/coding/path_safety.h`
- Reference (read-only): `include/aegis/coding/*.h`.

- [ ] **Step 1: Read each file with its header** (`coding_tools.c`/`discovery_tools.c` are large — read in slices, document as you go).
- [ ] **Step 2: Document statics and exported definitions** per Global Standard (path-safety validation rules, mutation semantics, tool-arg handling).
- [ ] **Step 3: Annotate safety-critical checks** (path traversal guards, shell-arg handling) with `//` notes.
- [ ] **Step 4: Build and check diff**

Run: `cmake --build build -j$(nproc)`
Expected: exit 0.

## Chunk 6: State, messages, observability

**Files:**
- Modify: `src/memory/memory.c`, `src/memory/memory_episodic.c`, `src/memory/memory_semantic.c`, `src/memory/memory_working.c`, `src/model/model.c`, `src/model/response.c`, `src/model/stream.c`, `src/message/message.c`, `src/message/role.c`, `src/message/tool_call.c`, `src/message/tool_result.c`, `src/observability/log.c`, `src/observability/metrics.c`, `src/observability/trace.c`, `src/reflection/reflection.c`, `src/replanner/replanner.c`, `src/critic/critic.c`, `src/checkpoint/checkpoint.c` (verify with `find` per dir).
- Reference (read-only): matching `include/aegis/` headers.

- [ ] **Step 1: Read each `.c` with its header.**
- [ ] **Step 2: Document statics and exported definitions** per Global Standard (retention/eviction policy in memory, streaming-chunk handling in model, checkpoint snapshot semantics).
- [ ] **Step 3: Build and check diff**

Run: `cmake --build build -j$(nproc)`
Expected: exit 0.

## Chunk 7: Providers, tools, storage, plugins, misc infra

**Files:**
- Modify: `src/provider/embedding.c`, `src/provider/llm.c`, `src/provider/provider.c`, `src/provider/provider_registry.c`, `src/tool/tool.c`, `src/tool/tool_call.c`, `src/tool/tool_registry.c`, `src/tool/tool_schema.c`, `src/storage/storage.c`, `src/storage/storage_store.c`, `src/plugin/plugin.c`, `src/security/security.c`, `src/skill/loader.c`, `src/skill/manifest.c`, `src/skill/registry.c`, `src/skill/skill.c`, `src/runtime/config.c`, `src/runtime/runtime.c` (verify with `find` per dir).
- Reference (read-only): matching `include/aegis/` headers.

- [ ] **Step 1: Read each `.c` with its header.**
- [ ] **Step 2: Document statics and exported definitions** per Global Standard (registry lookup semantics, schema validation, permission checks, config defaults).
- [ ] **Step 3: Build and check diff**

Run: `cmake --build build -j$(nproc)`
Expected: exit 0.

## Chunk 8: CLI apps + one public-header anomaly

**Files:**
- Modify: `apps/aegis/main.c`, `apps/aegis/cli_cancel.c`, `apps/aegis/cli_gate.c`, `apps/aegis/cli_helpers.c`, `apps/aegis/cli_helpers.h`, `apps/aegis/cli_init.c`, `apps/aegis/cli_input.c`, `apps/aegis/cli_inspect.c`, `apps/aegis/cli_interactive.c`, `apps/aegis/cli_render.c`, `apps/aegis/cli_run.c`, `apps/aegis/cli_status.c`, `apps/aegis/cli_repl.h`
- Modify (single fix): `include/aegis/agent/agent.h` lines 127–132 — the `@brief Resume agent execution` block has a stray `aegis_agent_event_bus(...)` declaration line pasted inside its doc comment; remove the stray line so the doc attaches to `aegis_agent_resume`.
- Reference (read-only): `apps/aegis/cli_repl.h` for command-handler contracts.

- [ ] **Step 1: Read all CLI files** (start with `main.c` + `cli_repl.h` for the command map, then each `cli_*.c`).
- [ ] **Step 2: Document every command handler and static helper** with `@brief/@param/@return` (flag parsing, dispatch, render paths).
- [ ] **Step 3: Apply the single `agent.h` doc fix** (delete only the stray line, nothing else).
- [ ] **Step 4: Build and check diff**

Run: `cmake --build build -j$(nproc)`
Expected: exit 0.

## Chunk 9: Final verification

- [ ] **Step 1: Full build**

Run: `cmake --build build -j$(nproc)`
Expected: exit 0, no warnings beyond the pre-existing baseline.

- [ ] **Step 2: Full test suite**

Run: `ctest --test-dir build --output-on-failure`
Expected: all tests pass; any failure must be shown with output and confirmed pre-existing (via `git stash` + rerun) rather than hidden.

- [ ] **Step 3: Comment-only audit**

Run: `git diff --stat` (expected: only `.c`/`.h` files) and spot-check `git diff -U0` to confirm no functional line changed.
