# Aegis Architecture Audit (Phase 0)

> Date: 2026-08-28 · Branch: master · Version: 0.1.0 · Commit: dd264e1
> Scope: `include/aegis/*`, `src/*`, `apps/aegis`, `providers/*`, `strategies/*`, `tests/*`, `CMakeLists.txt`

## 1. Overview

Aegis is a C11 agent runtime (~52 public headers, 69 C files, 24 internal headers). Core model is **Goal → Planner → Plan → TaskGraph → Scheduler → Executor → Tool → Critic → Reflection → Replanner → Checkpoint** — an Autonomous Workflow Runtime, not a reactive `Session→Turn→Tool` Coding Agent. CLI (`apps/aegis`) is correctly isolated as Application layer (migrated from `include/aegis/cli_helpers.h` in prior hardening). Build is `libaegis_common` (foundation) + `libaegis_core` (all runtime) + `aegis`/`aegis_shell` executables.

## 2. Public ABI

### 2.1 Stable handles (opaque, heap-owned, `aegis_*_destroy` consumes)
`agent`, `runtime`, `task`, `task_graph`, `scheduler`, `executor`, `plan`, `planner`, `memory*`, `provider_registry`, `tool_registry`, `security_policy`, `checkpoint`, `event_bus`, `storage`, `context`, `cancellation_token`.

### 2.2 Error model
`aegis_status_t` in `types.h`: `-12..0` (`OK, INTERNAL, NOMEM, INVALID, NOT_FOUND, BUSY, TIMEOUT, CANCELLED, PERM, PROVIDER, TOOL, MAX_ITERATIONS, INVALID_STATE`). All APIs return `aegis_status_t`; no structured `aegis_error_detail_t`.

### 2.3 Provider / Tool dispatch
- `provider/llm.h`: `struct aegis_llm_request {prompt, prompt_len, max_tokens, temperature}` → `aegis_llm_response {data,len}` via `aegis_llm_dispatch`. **Transport-agnostic blob**; no message/tool_choice/stream.
- `tool/tool.h`: `aegis_tool_args_t` (owned strings/bytes), `aegis_tool_schema`, `aegis_tool_def_t {name, schema, cap, execute}`, `registry {register,find}`, `tool_submit(executor,registry,task,tool_name,args)` — bridge shows `Task→Executor→ToolRegistry→Tool`. Tool `execute` signature `(user,args,token,result)`.

### 2.4 Known ABI gaps
- No `Message`, `ToolCall`, `ToolResult` as first-class citizens (tool call is `task.name` hint)
- No `Session`, `Branch`, `Persistence`
- No streaming (`stream.h` absent)
- No `model/request/response` structured ABI; `temperature/max_tokens` are only hints on blob request
- Checkpoint `version` conflated (`checkpoint_sequence` vs `iteration`) — hardened in `dd264e1` to separate `iteration` field.

## 3. Dependency Graph (actual)

```
CLI/apps → Agent → {Planner, Scheduler, Context} → Executor → Tool → Provider/Memory/Storage/Security → Common
                      ↘ Critic/Reflection/Replanner ↗
                 Runtime, Event, Checkpoint, Plugin, Observability → Common
```

Validation: `tool.h` uses forward decl for `executor` (no `#include executor.h`) to break cycle; `checkpoint` depends only on public `plan/graph/task`; `cancellation` moved `executor→common/cancellation`. No `CLI→core` reverse dep (enforced). `common` has zero upward edges.

## 4. Module Inventory

| Layer | Handles | Thread safety |
|---|---|---|
| **common** | allocator, buffer, queue (ring pow2), hashmap (open addr tombstone), vector/list, string, uuid (RFC4122), mutex, atomic, time, cancellation (token tree), thread | mutex-guarded or lock-free atomics |
| **task** | `task.c` (id atomic, state machine READY→RUNNING→WAITING→SUCCESS/FAILED/CANCELLED), `graph.c` (DAG, cycle check), `dependency.c` | graph mutex |
| **scheduler** | `scheduler.c` attach/poll/next/notify_complete | mutex |
| **executor** | `executor.c` fixed workers, FIFO queue, retry+timeout+cancellation, `wait(id)` single-delivery | `executor→task` lock order, no callback under lock |
| **tool** | `tool.c, tool_call.c, tool_registry.c, tool_schema.c` | registry mutex, schema validation |
| **provider** | `provider.c, registry, llm.c, embedding.c` + `providers/llm/mock, embedding_hash, storage/sqlite` | registry RW lock |
| **planner** | `plan.c` (versioned steps, validate acyclic), `planner.c` (provider dispatch, strategy registry) | single-threaded builder |
| **strategy** | `strategy_registry.c` + `strategies/plan_execute` | — |
| **memory** | `memory.c` + `_working/_episodic/_semantic/_procedural` | per-store mutex |
| **critic/reflection/replanner** | thin evaluators (task-state counts) | — |
| **autonomous** | `autonomous_agent.c` facade + `src/autonomous/{loop,planning,execution,evaluation,reflection,replanning,checkpoint,recovery,state_machine,lifecycle}.c` | `agent.lock` guards `state,iteration,checkpoint_sequence`, publishes events after unlock (snapshot pattern) |
| **checkpoint** | `checkpoint.c` atomic write tmp→rename, CRC32, magic `AEGISCHK`, `version/iteration/plan_version/tasks` | single-threaded handle |
| **context** | `context.c` builder priority/budget, string assembly (not message list) | not thread-safe |
| **security** | `security.c` capability gate `policy,cap,path,network` | — |
| **event** | `event_bus.c` subscriber table 256, snapshot-during-dispatch | mutex + snapshot |
| **runtime** | `runtime.c` thread pool lifecycle CREATED→STARTING→STARTED→STOPPING→STOPPED | mutex |
| **agent** | `agent.c` simple CREATED→READY→RUNNING→PAUSED→CANCELLING→COMPLETED/FAILED | mutex |
| **autonomous agent** | `autonomous_state.h` 15 states, `state_machine.c` validated transitions | mutex |

## 5. Ownership & Lifecycle

- **Owned/Transferred**: `*_create` allocates heap, `*_destroy` consumes; `*_response.data`, `plan_text`, `goal` copied.
- **Borrowed**: registry `find` returns value copy; `task` in graph owned by graph, executor borrows; `checkpoint_populate` borrows `agent/plan/graph`.
- **Leaks fixed in hardening**: `autonomous_agent.c` now destroys `runtime`, `owned_token/policy`, `planner/scheduler/executor/critic` on fail; `checkpoint_save` holds lock for consistent `iteration/sequence/state`; `recovery` validates `max_retries`.
- **Remaining**: no `retain/release` refcount; session/message future must define `borrowed vs owned vs shared`.

## 6. Thread Model & Lock Order

- Default: single agent loop thread + executor workers (2 workers, 64 q). No global singletons.
- Global lock order enforced: `executor → task` (no reverse), `agent.lock` never held while invoking `provider/tool/event` callbacks (snapshot→unlock→publish).
- Cancellation is token-tree: `Agent Token → Turn Token → Tool Token` partially implemented (executor `token` propagation, but no explicit turn hierarchy).
- No deadlock in tests (TSan clean on `build-tsan` 52/52).

## 7. Error Propagation

- All APIs return `aegis_status_t`; provider errors surface as `AEGIS_ERR_PROVIDER`, tool errors as `AEGIS_ERR_TOOL`, but autonomous loop maps many failures to `FAILED` with `AEGIS_ERR_INTERNAL`. No `CONTEXT_OVERFLOW, PROTOCOL, TOOL_VALIDATION, MODEL_RATE_LIMIT` as spec desires.
- Checkpoint `read` returns `status enum` (`OK/MISSING/CORRUPTED/INCOMPLETE/VERSION_MISMATCH`) mapped to `AEGIS_ERR_NOT_FOUND/INVALID` in recovery — needs richer `error_detail`.

## 8. Autonomous Loop (current)

`autonomous_loop_run` (152 lines) orchestrates `plan→execute→checkpoint→evaluate→reflect→replan` for `max_iterations` (5 default). `execution` does `materialize→scheduler_attach→next→security_gate→tool_submit→wait` per task, `CHECKPOINTING` per task, then `EVALUATING` via critic (counts SUCCESS vs PARTIAL). This is **plan-centric**, not `Session→Turn→Tool` reactive. `autonomous` public API retained for compat, will become `AutonomousStrategy`.

## 9. Context (current)

`context_builder` assembles priority-sorted `char*` blob with token budget. No `message_list_t`, no `tool definitions` as messages, no `project instructions`, no `compaction`. Must be rebuilt to produce `aegis_message_list_t`.

## 10. Security / CLI

- `security_policy` mandatory when `tool_registry` present (allow-all fallback hardened). Gate checks `cap` per tool, but not yet path/network/credential granularity per tool_call.
- CLI in `apps/aegis/{main,cli_*.c}` with `cli_helpers.{h,c}` private (not public). Commands `init/run/status/cancel/inspect`, not yet interactive `aegis` REPL or `streaming UI`.

## 11. Persistence & Session

- `checkpoint` handles long-running workflow recovery; `storage/sqlite` handles generic K/V; no `session/history/JSONL/branch/fork` model exists.

## 12. Tests

- 52 tests: `unit/*` (scheduler, hashmap, executor_concurrent, task_race...), `system/*` (autonomous closed_loop, tool, security, cancellation, failure, recovery_e2e), `integration_cli`. All 52 pass normal/ASan/TSan.
- Coverage gaps (for refactor): `message, tool_call, session, branch, context compaction, streaming, read/edit/bash`.

## 13. Architecture Risks for Refactor

1. **Plan-centric bias**: default path forces `planner` — must make `AutonomousStrategy` optional.
2. **Blob LLM API**: `prompt→bytes` hides tool_choice/stream/reasoning → must add adapter, not delete.
3. **Task as tool proxy**: `task.name == tool.name` conflation → need `ToolCall` separation.
4. **Context as string**: `char*` assembly cannot produce `message_list` → rebuild context engine.
5. **Checkpoint vs Session confusion**: checkpoint already versioned atomic; session JSONL must be append-only replayable distinct.
6. **Lock ordering**: future `Session→Context→Model→Tool` pipeline must preserve `no callback under lock`.
7. **ABI split**: single `libaegis_core` too large → needs `common/runtime/agent/coding/provider/tool/workflow` split in cmake.

## 14. Positive Foundations to Keep

`common/*`, `cancellation`, `provider/tool registry` with RW lock, `executor` wait-not-callback model, `event_bus` snapshot, `checkpoint` atomic CRC, `memory` modular stores, `strategy_registry` pluggability, `security` mandatory gate, `apps/aegis` isolation.
