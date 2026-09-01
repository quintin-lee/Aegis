# Agent Loop

## State Machine

```mermaid
stateDiagram-v2
    [*] --> IDLE
    IDLE --> RUNNING: user_input
    RUNNING --> WAITING_MODEL: build_context
    WAITING_MODEL --> WAITING_TOOL: tool_calls
    WAITING_MODEL --> COMPLETED: no_tool_calls
    WAITING_TOOL --> WAITING_MODEL: tool_results
    WAITING_TOOL --> CANCELLING: cancel
    WAITING_MODEL --> CANCELLING: cancel
    WAITING_MODEL --> COMPACTING: overflow
    COMPACTING --> WAITING_MODEL: retry
    RUNNING --> FAILED: error
    CANCELLING --> CANCELLED
    COMPLETED --> [*]
    FAILED --> [*]
    CANCELLED --> [*]
```

## Sequence

```mermaid
sequenceDiagram
    participant U as User
    participant S as Session
    participant C as Context
    participant M as Model (stream)
    participant T as Tool Runtime
    U->>S: append USER message
    loop Turn
        S->>C: build_message_list (priority/budget)
        C->>M: stream(request, cb)
        M-->>S: TEXT_DELTA* / TOOL_CALL_START
        M->>S: append ASSISTANT (tool_calls?)
        alt no tool_calls
            S-->>U: final
        else tool_calls
            S->>T: for each call: registry→validate→security→executor→result
            T-->>S: append TOOL messages
        end
    end
```

## Implementation

`aegis_agent_loop_t {session, model, tools, system_prompt, token, state, lock}`

- `build_context_messages()` — session messages + system prompt (no `char*` concatenation)
- `aegis_model_stream` — mock chunked `TEXT_DELTA`, real provider will emit `TOOL_CALL_*`
- Cancellation: `token` checked before each phase, `WAITING_* → CANCELLING`
- No callback under `lock`: `set_state` is lock-protected, `stream_cb` is lock-free.

## Errors

`CANCELLED → CANCELLED`, `TIMEOUT → FAILED`, `CONTEXT_OVERFLOW → COMPACTING → retry`.
