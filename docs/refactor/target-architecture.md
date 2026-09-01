# Aegis Target Architecture

> Goal: upgrade from `Goal→Plan→TaskGraph→Scheduler→Executor→Critic` workflow runtime to `Session→Agent Loop→LLM Turn→Tool Call→Context Update` universal agent runtime, with Pi-like Coding Agent as default, Autonomous as optional strategy.

## 1. System Layout

```
                         AEGIS
                           │
        ┌──────────────────┼──────────────────┐
        │                  │                  │
        ▼                  ▼                  ▼
    CLI / RPC          Embedded API       Applications
                           │
                           ▼
                    ┌──────────────┐
                    │ Agent Runtime│
                    └──────┬───────┘
                           │
             ┌─────────────┼─────────────┐
             ▼             ▼             ▼
          Session       Context        Strategy
             │             │             │
             │             │       ┌─────┴─────┐
             │             │       ▼           ▼
             │             │    Coding     Autonomous
             │             │
             └──────┬──────┘
                    ▼
               Agent Loop
                    │
             ┌──────┴──────┐
             ▼             ▼
          Model          Tool Runtime
             │             │
       ┌─────┴─────┐   ┌───┴────────────┐
       ▼           ▼   ▼                ▼
   Provider     Stream Security       Executor
                                   │
                            ┌──────┴──────┐
                            ▼             ▼
                           Task         Process
```

## 2. Directory (target)

```
include/aegis/
  common/{allocator,atomic,buffer,cancellation,error,hashmap,list,queue,string,time,vector,uuid}.h
  runtime/{runtime,config,lifecycle,handle}.h
  agent/{agent,loop,state,strategy,turn}.h
  session/{session,message,history,branch,persistence,compaction}.h  # session is source of truth
  message/{message,content,role,tool_call,tool_result,usage}.h       # first-class
  context/{context,builder,budget,compaction,project,source}.h       # produces message_list
  model/{model,request,response,stream,capability}.h
  provider/{provider,registry,llm,model,stream}.h
  tool/{tool,registry,invocation,args,result,schema,policy}.h
  coding/{coding_agent,file_tools,shell_tools,project,instructions,mutations}.h
  skill/{skill,registry,loader,manifest}.h
  extension/{extension,registry,lifecycle}.h
  memory/{memory,working,episodic,semantic,procedural}.h
  task/, scheduler/, executor/, planner/, replanner/, critic/, reflection/, security/, storage/, checkpoint/, event/, observability/, plugin/
src/
  runtime/, agent/, session/, message/, context/, model/, provider/, tool/, coding/, skill/, extension/, memory/, task/, scheduler/, executor/, planner/, replanner/, critic/, reflection/, security/, storage/, checkpoint/, event/, observability/, plugin/, common/, internal/
apps/aegis/{main,cli_*.c, helpers}
```

## 3. Core Abstractions

### 3.1 Message Model (opaque)
```c
typedef enum { SYSTEM, USER, ASSISTANT, TOOL, EVENT, SUMMARY } aegis_message_role_t;
typedef struct aegis_message { id, role, timestamp, content, tool_calls[], tool_call_id, metadata, parent_id } aegis_message_t;
typedef struct aegis_message_list { count, at, append, prepend, clone } aegis_message_list_t;
```
Ops: `create/destroy/clone/append/count/at/role/content/tool_calls`. Content is `text|reasoning|tool_call`.

### 3.2 Tool Call Model (opaque)
```c
typedef struct aegis_tool_call { call_id, tool_name, arguments(JSON), index, metadata } aegis_tool_call_t;
typedef struct aegis_tool_result { call_id, status, content, error, is_partial } aegis_tool_result_t;
```
Runtime: `LLM ToolCall → registry lookup → schema validate → security gate → executor → result` — no `task.name` alias.

### 3.3 Model ABI (structured, streaming)
```c
typedef struct aegis_model_request {
  const char *model; const aegis_message_list_t *messages;
  const aegis_tool_registry_t *tools; uint32_t max_tokens; float temperature; bool stream;
} aegis_model_request_t;
typedef enum { TEXT_DELTA, REASONING_DELTA, TOOL_CALL_START/DELTA/END, USAGE, END, ERROR } aegis_model_stream_event_type_t;
typedef aegis_status_t (*aegis_model_stream_callback_fn)(const aegis_model_stream_event_t*,void*);
```
Provider implements `stream(request, callback, user)` lock-free; agent loop consumes events, not provider logic.

### 3.4 Session
```c
typedef struct aegis_session {
  session_id, created_at, updated_at, project_root, working_directory,
  messages, branch, parent, metadata
} aegis_session_t;
```
Ops: `create/destroy/append_message/count/at/save/load/fork/branch/switch`. Persistence: append-only JSONL (one event per line, versioned, replayable) not single huge JSON.

### 3.5 Context Engine
Produces `aegis_message_list_t` (not `char*`). Layers: `system prompt → project instructions → user/assistant history → tool calls/results → working memory → skills → tool definitions → summary`. Budget: `token budget, reserve output, priority, drop strategy` with deterministic truncation and `compaction` (summarize boundary → summary message).

### 3.6 Agent Loop & State
New states: `IDLE,RUNNING,WAITING_MODEL,WAITING_TOOL,COMPACTING,PAUSED,CANCELLING,COMPLETED,FAILED,CANCELLED`. Loop:
```
append user → while(true){ check cancel; load project; build context; budget; invoke model stream; append assistant; if !tool_calls break; for each call: security→tool_runtime→append result; }
```
Strategy ABI: `init/before_turn/after_model/after_tool/should_continue/shutdown` — `Coding (reactive)` default, `Autonomous` = Goal→Plan→Graph→Scheduler→Executor→Evaluate→Reflect→Replan as optional.

### 3.7 Tool Runtime & Coding Tools
Registry + schema + security + executor. Builtins `read(path,offset,limit)` (normalize, symlink, size, binary), `write` (atomic, mkdir -p), `edit` (exact unique match), `bash` (fork/exec, pipe, poll, waitpid, timeout/cancel). File mutation queue per path serializes `write→edit→edit`.

### 3.8 Security / Cancellation / Timeout
`security_gate(tool,cap,path)` mandatory (no bypass). Cancellation token tree `Agent→Turn→Tool`. Timeouts `agent/turn/tool/model` with real process kill.

### 3.9 Event & Observability
Events `SESSION_*, USER_MESSAGE, MODEL_*, TOOL_*, COMPACTION_*, AGENT_STATE_CHANGED, SESSION_SAVED` via `event_bus` (no dangling ptr). Metrics `llm_requests/latency/tokens, tool_calls/latency, context_tokens, compactions, turns`.

## 4. Dependency Rules
```
common
 ↑ 
runtime
 ↑
message/context/session/model/tool
 ↑
agent
 ↑
coding
 ↑
CLI/RPC
```
`agent` never depends on `CLI`; `session` never depends on `tool`; `tool` never depends on `session`; `message→session→message` via forward decl/opaque. No cycles.

## 5. Library Split
`libaegis_common`, `libaegis_runtime`, `libaegis_agent`, `libaegis_coding`, `libaegis_provider`, `libaegis_tool`, `libaegis_workflow` (planner etc.). `libaegis_agent` has no provider/tool/storage dep.

## 6. Checkpoint vs Session
`Session` = conversation/history/replay (JSONL). `Checkpoint` = runtime recovery for long jobs (atomic CRC, version + iteration). Session for interactive, checkpoint for autonomous.

## 7. Error & Thread Model
`aegis_status_t` + future `error_detail_t` and `CONTEXT_OVERFLOW, TOOL_VALIDATION, MODEL_RATE_LIMIT`. No callback under `agent.lock`/`executor` lock. Model/tool/event callbacks are lock-free via message pipeline.
