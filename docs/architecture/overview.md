# Aegis Architecture Overview

> Version: 2.0 (Message/Session/Streaming) · 2026-08-28 · `libaegis_common` + `libaegis_core` → `libaegis_agent` + `libaegis_coding` (future split)

## 1. Principles

- **Agent Loop > Planner** — default is reactive `Session→Turn→Tool`, `AutonomousStrategy` is optional
- **Session > Checkpoint** — `Session` is conversation source of truth (JSONL), `Checkpoint` is workflow recovery
- **Message > Prompt** — `aegis_message_list_t` is first-class, not `char*`
- **ToolCall > TaskName** — `aegis_tool_call_t`/`aegis_message_tool_result_t` distinct
- **Stream > Blocking** — `aegis_model_stream` callback decoupled from loop/UI

## 2. Layers

```
common (allocator, hashmap, vector, mutex, cancellation, uuid, thread)
  ↑
runtime (lifecycle)
  ↑
message / session / model / tool  (no cycles, opaque handles)
  ↑
context (builder → message_list, budget)
  ↑
agent (loop, state, strategy)
  ↑
coding (read/write/edit/bash + mutations + coding_agent)
  ↑
CLI / RPC / Embedded (apps/aegis)
```

`agent` never depends on `CLI`; `session` never depends on `tool`; `tool` never depends on `session`.

## 3. Runtime

- `aegis_runtime_t` thread pool, `aegis_agent_loop_t` reactive loop: `User → Context → Model(stream) → Tool* → Session`
- States `IDLE,RUNNING,WAITING_MODEL,WAITING_TOOL,COMPACTING,PAUSED,CANCELLING,COMPLETED/FAILED/CANCELLED` with `pthread_mutex` guard and `no callback under lock`.

## 4. Ownership

- `*_create` → heap-owned, `*_destroy` consumes, `clone` deep-copies, `list_append` clones, `registry_find` copies-out, `executor` borrows `task` from `graph`.

## 5. Storage

- `Session` JSONL append-only, `Checkpoint` atomic `tmp→rename` + CRC, `storage/sqlite` for K/V.

## 6. Roadmap

`libaegis_core` → split to `common, runtime, agent, coding, provider, tool, workflow` per `cmake/` modules.
