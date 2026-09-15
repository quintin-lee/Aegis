# MCP Client Design

**Feature:** Model Context Protocol (MCP) client — lets the coding agent discover and call tools exposed by external MCP servers over stdio.

**Status:** Design. See `docs/superpowers/specs/2026-09-14-mcp-client-design.md`.

## 1. Background & Goals

MCP (Model Context Protocol) is a JSON-RPC 2.0 protocol that lets an LLM agent
connect to an external "MCP server" process that exposes tools. This work adds
an **MCP client** to Aegis so the coding agent can:

1. Spawn an MCP server subprocess and complete the `initialize` handshake.
2. Discover the server's tools (`tools/list`).
3. Call those tools (`tools/call`) from the agent's tool loop.

Goals:

- Zero new external dependencies (pure `fork`/`execvp` + pipes + the existing
  JSON infrastructure).
- Discovered remote tools register **directly** into the coding agent's
  `aegis_tool_registry`, so the existing agent loop can invoke them with no
  protocol awareness.
- A general JSON value DOM that is reusable (extracted from the parser
  currently private to `src/agent/loop.c`).

Non-goals (out of scope for this feature):

- HTTP/SSE MCP transport (stdio subprocess only — covers the common case).
- MCP `resources` / `prompts` primitives (tools only).
- Server hot-reload, multi-server fan-in, server-side MCP.

## 2. Locked Decisions

| # | Decision |
|---|----------|
| 1 | **Transport:** stdio subprocess only. `fork`/`execvp` + `pipe` (mirror `git_tools.c`). No HTTP/SSE. |
| 2 | **Registration:** remote tools register directly into the coding agent's `aegis_tool_registry`; lifecycle tied to agent create/destroy. No standalone bridge object. |
| 3 | **JSON parsing:** extract a general JSON value DOM into a shared target; `loop.c`'s static parser becomes a thin adapter over it (loop.c must stay green). |
| 4 | **Tests:** unit tests for the JSON DOM; e2e with a small C mock MCP server subprocess; regression guard for the `loop.c` refactor. |

## 3. Module Layout

New directory `src/mcp/`, mirroring the provider pattern:

| File | Responsibility |
|------|----------------|
| `mcp_json.h` / `mcp_json.c` | General JSON value DOM + recursive-descent parser + accessors. Extracted from `loop.c` statics. |
| `mcp_stdio.h` / `mcp_stdio.c` | Internal transport: spawn the MCP server subprocess, send newline-delimited JSON-RPC requests, read responses, correlate by `id`. |
| `mcp_client.h` / `mcp_client.c` | Public API: client lifecycle, `tools/list` discovery, `tools/call`, and a helper to register discovered tools into a registry. |

### 3.1 JSON Value DOM (`mcp_json`)

```c
typedef enum {
    AEGIS_JSON_NULL, AEGIS_JSON_BOOL, AEGIS_JSON_INT, AEGIS_JSON_FLOAT,
    AEGIS_JSON_STRING, AEGIS_JSON_ARRAY, AEGIS_JSON_OBJECT,
} aegis_json_type_t;

typedef struct aegis_json_value aegis_json_value_t;
struct aegis_json_value {
    aegis_json_type_t type;
    union {
        bool   b;
        int64_t i;
        double f;
        char*  str;                       /* NUL-terminated */
        struct { aegis_json_value_t** items; size_t count; } arr;
        struct { char** keys; aegis_json_value_t** vals; size_t count; } obj;
    };
};
```

API:

```c
aegis_status_t aegis_json_parse(const char* json, aegis_json_value_t** out);
void           aegis_json_value_destroy(aegis_json_value_t* v);
aegis_json_value_t* aegis_json_object_get(const aegis_json_value_t* obj, const char* key);
aegis_json_value_t* aegis_json_array_at(const aegis_json_value_t* arr, size_t i);
size_t          aegis_json_array_len(const aegis_json_value_t* arr);
const char*     aegis_json_string(const aegis_json_value_t* v); /* NULL if not STRING */
int64_t         aegis_json_int(const aegis_json_value_t* v);    /* 0 if not INT */
bool            aegis_json_bool(const aegis_json_value_t* v);   /* false if not BOOL */
```

- `aegis_json_parse` owns the tree; caller frees with `aegis_json_value_destroy`
  (recursive). Malformed input returns `AEGIS_ERR_PROVIDER` with `*out == NULL`.
- Extracted from the static `json_skip_ws` / `json_parse_string` /
  `json_parse_args` in `src/agent/loop.c` (~L472–560). `loop.c`'s
  `json_parse_args` becomes an adapter: parse to the DOM object, then convert
  the object into an `aegis_tool_args_t`.

### 3.2 stdio Transport (`mcp_stdio`, internal)

```c
aegis_status_t aegis_mcp_stdio_spawn(aegis_mcp_stdio_t** out,
                                     const char* cmd, const char* const* argv);
void           aegis_mcp_stdio_close(aegis_mcp_stdio_t* s);
aegis_status_t aegis_mcp_stdio_request(aegis_mcp_stdio_t* s, uint64_t id,
                                       const char* method,
                                       const aegis_json_value_t* params,
                                       aegis_json_value_t** out_result);
```

- `spawn` forks/execs the server, wires pipes, sends the `initialize` request,
  then the `initialized` notification. `fork`/`execvp` + `pipe` mirror
  `git_tools.c`.
- `request` writes one JSON-RPC request line to child stdin, reads one
  response line from child stdout, parses it via the DOM, and returns the
  `.result` (or surfaces `.error` as `AEGIS_ERR_PROVIDER`). Responses are
  correlated by the JSON-RPC `id`.
- Child crash / EOF → subsequent requests return `AEGIS_ERR_PROVIDER`.

### 3.3 Public Client API (`mcp_client`)

```c
typedef struct aegis_mcp_client aegis_mcp_client_t;

typedef struct {
    char                name[128];
    char                description[256];
    aegis_json_value_t* input_schema;   /* owned; JSON-Schema DOM */
} aegis_mcp_tool_info_t;

aegis_status_t aegis_mcp_client_create(const char* server_cmd,
                                       const char* const* server_argv,
                                       aegis_mcp_client_t** out);
void           aegis_mcp_client_destroy(aegis_mcp_client_t* c);

/* out_tools/out_count owned by the client (freed on destroy). */
aegis_status_t aegis_mcp_list_tools(aegis_mcp_client_t* c, size_t* out_count,
                                    aegis_mcp_tool_info_t** out_tools);

/* tools/call: maps aegis_tool_args_t -> JSON arguments; result -> aegis_tool_result_t. */
aegis_status_t aegis_mcp_call_tool(aegis_mcp_client_t* c, const char* name,
                                   const aegis_tool_args_t* args,
                                   aegis_tool_result_t* out);

/* Wraps each discovered remote tool as aegis_tool_def_t and registers it. */
aegis_status_t aegis_mcp_register_tools(aegis_mcp_client_t* c, aegis_tool_registry_t* reg);
```

- `aegis_mcp_register_tools` builds an `aegis_tool_def_t` per remote tool whose
  `execute` fn sends `tools/call` and maps the JSON `result.content` into an
  `aegis_tool_result_t`. The execute `user` pointer is the client.
- **Result mapping:** each `result.content[]` item `{type:'text', text}` is
  concatenated into one string and set as the tool result payload.
  `result.isError == true` → return `AEGIS_ERR_PROVIDER` (but the error text
  is still placed in the result so the agent loop can surface it).

## 4. Protocol (JSON-RPC 2.0 over stdio, newline-delimited)

- One JSON-RPC message per line.
- Handshake: `initialize` (request, with `protocolVersion` + client
  capabilities) → server reply → `notifications/initialized` (no response).
- `tools/list` → `{ result: { tools: [ { name, description, inputSchema } ] } }`.
- `tools/call` → `{ result: { content: [ { type:'text', text } ], isError } }`.

## 5. Error Handling

| Condition | Behavior |
|-----------|----------|
| `fork`/`execvp` fails | `aegis_mcp_client_create` → `AEGIS_ERR_PROVIDER`; no tools registered. |
| `initialize` handshake fails/times out | create returns `AEGIS_ERR_PROVIDER`; `destroy` cleans up the child. |
| A single `tools/call` fails | `aegis_mcp_call_tool` returns that status; other registered tools are unaffected. |
| Child exits unexpectedly | Subsequent `aegis_mcp_stdio_request` → `AEGIS_ERR_PROVIDER`. |
| `aegis_mcp_client_destroy` | closes pipes, `waitpid`, frees discovered-tool DOM + stdio handle. |

Cancellation is honored by the caller's token at the read/write loop boundaries
(`aegis_mcp_call_tool` accepts the agent's `aegis_cancellation_token_t`).

## 6. CMake

- New option `AEGIS_MCP` (default `ON`), requires **no** libcurl — pure
  `fork`/`execvp` + existing `aegis_*` libs.
- The JSON DOM lives in a standalone target **`aegis_json`** (`src/mcp/`
  built as `aegis_json`). `aegis_agent` (loop.c), `aegis_mcp`, and
  `aegis_coding` all link `aegis_json`.
- `aegis_mcp` target: `mcp_json.c` (or links `aegis_json`), `mcp_stdio.c`,
  `mcp_client.c`; links `aegis_json aegis_tool aegis_common`.

## 7. Coding-Agent Wiring

- `aegis_coding_agent_config_t` gains an MCP server field (a `cmd` + `argv`
  pair, e.g. `const char* mcp_cmd; const char* const* mcp_argv;` — or a
  `char* const* mcp_cmd_argv`).
- On `aegis_coding_agent_create`: if MCP server is configured,
  `aegis_mcp_client_create` + `aegis_mcp_register_tools(client, a->tools)`.
- On `aegis_coding_agent_destroy`: `aegis_mcp_client_destroy`. The
  agent struct gains `aegis_mcp_client_t* mcp_client`.

## 8. Testing

1. **`tests/unit/test_mcp_json.c`** — DOM parse/accessors: scalars, nested
   arrays/objects, string escaping, corrupt input → `AEGIS_ERR_PROVIDER`.
2. **`tests/unit/test_mcp_client.c`** — mock MCP server: a small C program
   (`tests/fixtures/mock_mcp_server.c`, compiled to an executable) that reads
   stdin JSON-RPC and emits canned `initialize` / `tools/list` (1–2 tools) /
   `tools/call` responses. Test verifies `aegis_mcp_list_tools` discovery and
   `aegis_mcp_call_tool` result mapping.
3. **Regression** — the `loop.c` refactor must keep `unit_loop` and
   `system_coding_loop` green.

## 9. Scope OUT

HTTP/SSE transport, MCP resources/prompts, server hot-reload, server-side MCP,
multi-server fan-in, streaming tool output.
