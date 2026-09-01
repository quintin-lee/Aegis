# Aegis Migration Plan (Incremental, Adapter-First)

> Principle: `New API → Adapter → Migration → Deprecation` — never big-bang delete. Each phase ends with `build + ctest 52/52 + ASan/UBSan (+TSan if concurrent) + zero warnings + ABI check`.

## Phase 0 — Audit (current, no code change)
- [x] `docs/refactor/architecture-audit.md`
- [x] `docs/refactor/target-architecture.md`
- [x] `docs/refactor/migration-plan.md` (this file)
- Gate: docs reviewed, dependency graph & ownership table approved.

## Phase 1 — Message + ToolCall (2 weeks)
**Create** `include/aegis/message/{message,content,role,tool_call,tool_result,usage}.h` + `src/message/*.c` opaque handles.

- API: `aegis_message_create/destroy/clone`, `aegis_message_list_append/count/at`, `aegis_tool_call_create/set_name/set_args/id`, `aegis_tool_result_create/set_content/error`
- Keep `provider/llm.h` blob `prompt→bytes` as compat adapter: `message_list → prompt string → provider → bytes → message`
- Tests: `tests/unit/test_message.c`, `test_tool_call.c` (count, clone, serialization fuzz)
- CMake: new `libaegis_message` or append to `libaegis_common` (decision: append to `common` to avoid premature split)
- Migration: no existing caller changes.

## Phase 2 — Model + Stream
**Create** `include/aegis/model/{request,response,stream,capability}.h` + `include/aegis/provider/stream.h`

- `aegis_model_request_t {model, messages, tools, temperature, max_tokens, stream}`, `aegis_model_stream_event_t`, `callback_fn`
- Decouple: `provider` invokes `callback` lock-free; `agent loop` consumes, `CLI` renders — three layers never intermix.
- Mock: `providers/model/mock_stream` that replays `TEXT_DELTA + TOOL_CALL_START/END`
- Adapter: old `aegis_llm_dispatch(prompt)` implemented as `message_list(single user) → request → stream callback collects → response`
- Tests: `test_model_stream.c` (delta reassembly, tool_call chunking, error propagation)

## Phase 3 — Session
**Create** `include/aegis/session/{session,history,branch,persistence,compaction}.h` + `src/session/*.c`

- `session {id, project_root, messages, branch, parent}`; `append_message`, `message_count/at`, `save/load` JSONL append-only, `fork/branch/switch`
- Persistence format: `{"v":1,"type":"session_start",...}` per line, crash-tolerant replay, forward compat
- No dep on `tool` or `context`; depends only on `message`, `common`
- Tests: `test_session_persistence.c` (crash replay, fork tree, version upgrade)

## Phase 4 — Context Engine
**Refactor** `include/aegis/context/*` + `src/context/*` to produce `message_list_t`

- Replace `char* prompt` builder with `context_build(session,memory,skills,tool_defs) → message_list`
- Add `budget {hard/soft, reserve_output}`, `project {AGENTS.md search global→project}` via `coding/project`
- Compaction: `large history → boundary → summarize (mock LLM) → summary message + recent tail`
- Adapter: old `context_build_string` kept as `message_list → concatenated string` for `planner` compat
- Tests: `test_context_budget.c`, `test_compaction.c`, `test_project_instructions.c`

## Phase 5 — Agent Loop (most critical)
**Create** `include/aegis/agent/{loop,state,strategy,turn}.h` + `src/agent/loop.c`

- States `IDLE,RUNNING,WAITING_MODEL,WAITING_TOOL,COMPACTING,PAUSED,CANCELLING,COMPLETED,FAILED,CANCELLED`
- Loop algorithm as spec16, token tree `Agent→Turn→Tool` propagated, context overflow → `retry after compaction`
- Strategy ABI `init/before_turn/after_model/after_tool/should_continue/shutdown`; default `Coding (reactive)`
- Adapter: `autonomous_agent` stays, not touched
- Tests: `test_agent_loop.c` (state transitions, cancel during model/tool, timeout)

## Phase 6 — Builtin Coding Tools
**Create** `src/coding/{read,write,edit,bash,mutations}.c` + `include/aegis/coding/*`

- `read` (normalize, symlink policy, size limit, binary detect), `write` (atomic rename, mkdir -p), `edit` (exact unique), `bash` (fork/exec pipe poll waitpid, timeout/cancel, cwd/env)
- `mutations.c` per-path queue
- Register via `tool_registry` with schemas, not hard-coded `if strcmp(tool)`
- Tests: `test_coding_tools.c` (concurrent edit serialization, large file, binary, timeout kill)

## Phase 7 — Coding Agent
**Create** `include/aegis/coding/coding_agent.h` + `src/coding/coding_agent.c`

- Composes `session + context + model + tool_registry(builtins) + security + project`
- `aegis_coding_agent_create/run/cancel` — minimal loop, no planner
- Tests: system happy path `User→read→edit→bash→final`, tool error → model recovery

## Phase 8 — CLI
**Refactor** `apps/aegis/*` to new loop

- `aegis` (interactive), `aegis --print`, `--resume`, `--session`, `--model`, `--json`, commands `/help,/model,/session,/fork,/compact`
- Streaming UI via `event_bus` → renderer, zero `printf` in core
- Tests: `integration_cli` updated + `test_cli_streaming.c`

## Phase 9 — Skills / Extensions
- `include/aegis/skill/*` registry/loader from `~/.aegis/skills` + project, manifest, prompt injection
- `include/aegis/extension/*` ABI version, lifecycle, capability decl (tool/provider/skill/command/strategy)
- Plugin retains but redefined; tests `test_skill_loader.c`

## Phase 10 — Autonomous Migration
- Wrap existing `planner/task_graph/scheduler/executor/critic/reflection/replanner/checkpoint` as `AutonomousStrategy` implementing `strategy ABI`
- `aegis_autonomous_agent_t` becomes thin adapter: `create → AutonomousStrategy + new agent runtime` → deprecated
- Tests: `system_autonomous` re-used, plus `test_autonomous_strategy.c` (goal→plan→execute→replan without affecting coding agent)

## Phase 11 — Tests & Stability
- `tests/{unit,component,integration,system,fuzz,stress}` hierarchy; fuzz `tool args, session JSONL, stream, plan DSL, checkpoint` with libFuzzer
- CI `ASan/UBSan/TSan` on each phase; `ctest -j` 52+ new tests green
- Docs: `docs/architecture/*.md`, `docs/adr/*`, `docs/migration/*`

## CMake & Library Split (parallel, Phase 1-2)
- Split `CMakeLists.txt` → `cmake/{AegisOptions,Warnings,Tests,Sanitizers,Install,Version}.cmake`
- Top-level only `project+options+subdirectories+install`; per-dir `CMakeLists.txt` for `libaegis_common, libaegis_runtime, libaegis_agent, libaegis_coding, libaegis_provider, libaegis_tool, libaegis_workflow`

## Compatibility Guarantees
- Old `aegis_llm_complete(prompt,response)` kept via adapter until Phase 10 deprecation notice (1 minor version).
- `autonomous_agent.h` public header stays until Phase 10 `deprecated` attribute, then removal.
- No provider/tool/storage hard-coded in `libaegis_agent`.

## Risks & Mitigations
- **Blow-up refactor**: mitigate via adapter layer + per-phase `build green` gate.
- **Lock order violation**: enforce `executor→task`, `agent.lock` never held in callbacks; TSan on every concurrent phase.
- **Ownership confusion**: annotate every new public header with `who allocates/owns/destroys + thread safety`.

## Phase Exit Checklist (every phase)
1. Build `cmake -S . -B build && cmake --build build`
2. `ctest --test-dir build -j` 52+ pass
3. `ctest` ASan/UBSan (LD_PRELOAD) + TSan (clang) if concurrent
4. ABI check `nm -D libaegis*` no CLI/provider leak
5. Ownership/thread docs updated
6. No circular deps (`include/aegis` graph `common←runtime←message/session←agent←coding`)
