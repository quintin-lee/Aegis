# MCP Client Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:executing-plans to implement this plan (NO subagents — user standing instruction). Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add an MCP (Model Context Protocol) client so the coding agent can spawn an external MCP server over stdio, discover its tools, and call them — registering the remote tools directly into the agent's `aegis_tool_registry`.

**Architecture:** Three new files under `src/mcp/`: a general JSON value DOM + parser (`mcp_json`, a standalone `aegis_json` target extracted from `loop.c`'s static parser so both `aegis_agent` and the MCP client reuse it), a stdio subprocess transport (`mcp_stdio`, fork/execvp + pipes mirroring `git_tools.c`), and the public client API (`mcp_client`). The coding agent gains an optional MCP server field; on create it spawns + registers the remote tools, on destroy it tears them down.

**Tech Stack:** C, no new external deps (fork/execvp/pipe/poll, existing `aegis_tool`/`aegis_common`).

**Build gate after every chunk:** `cmake --build build --parallel 8` clean + `ctest --output-on-failure` all green (baseline 73/73). clang-format **C files only, never `.cmake`**.

---

## Grounding (verified against source)

### loop.c static JSON parser (`src/agent/loop.c` L472–646)
- `json_skip_ws(const char** p, const char* end)` L472 — advance past `isspace`.
- `json_parse_string(const char** p, const char* end, char** out)` L480 — parse a JSON string into a malloc'd NUL-terminated `char*` (cap 32, doubling); escapes `\n \r \t \" \\ \/`; **no `\u`**. Returns 1 on success.
- `json_parse_args(const char* json, aegis_tool_args_t** out)` L531–646 — parse a **flat** top-level object into `aegis_tool_args_t` (string/bool/int/float leaves; **no nesting** — nested values fall into the number branch and fail). `json_parse_args` is the one that becomes a thin DOM adapter.

**Key implication:** the extracted `aegis_json_parse` must be a **full recursive-descent** parser (nested object + array + scalars), a superset of the current loop.c parser. The loop.c adapter keeps behavior identical by mapping DOM scalars to `aegis_tool_args_add_*` and **rejecting** ARRAY/OBJECT leaf values exactly as the old code did. `unit_loop` + `system_coding_loop` MUST stay green.

### git_tools.c subprocess idiom (`src/coding/git_tools.c` L163–300)
`pipe()` → `fork()` → child: `setpgid(0,0)`, `dup2(out[1], STDOUT_FILENO)`, `execvp(cmd, argv)`, `_exit(127)`; parent: `close(out[1])`, `O_NONBLOCK` + `poll()` read loop, `waitpid`, cancellation via `kill(-pid, SIGTERM)`. MCP needs **two** pipes (stdin + stdout) for bidirectional newline-delimited JSON-RPC, not a bulk capture.

### tool.h ABI (`include/aegis/tool/tool.h`)
- `aegis_tool_def_t {const char* name; const char* description; aegis_tool_schema_t schema; aegis_capability_t capabilities; aegis_tool_execute_fn execute; aegis_tool_cancel_fn cancel; void* user;}` — **name/description/schema.params are borrowed** (must outlive registration). So `aegis_mcp_register_tools` allocates per-tool name/description/param arrays owned by the client and frees them in `aegis_mcp_client_destroy`.
- `aegis_tool_result_set_string(result, s)` deep-copies.
- `aegis_tool_registry_register(reg, const aegis_tool_def_t*)` shallow-copies; fails on dup name / NULL execute / empty name.

### coding_agent config (`include/aegis/coding/coding_agent.h` L24–31)
`aegis_coding_agent_config_t {project_root, model, provider, api_key, base_url, aegis_tool_registry_t* tools /*borrowed*/}`. Struct `aegis_coding_agent` (coding_agent.c) already has `aegis_model_client_t* model`, `aegis_cancellation_token_t* token`, `aegis_tool_registry_t* tools`. Gains `aegis_mcp_client_t* mcp_client` + config `mcp_cmd`/`mcp_argv`.

### CMake layout
- `src/CMakeLists.txt`: add `add_subdirectory(mcp)` to the subdir list.
- `src/agent/CMakeLists.txt` (`aegis_agent`): add `aegis_json` to `target_link_libraries`.
- `src/coding/CMakeLists.txt` (`aegis_coding`): add a gated `AEGIS_MCP` block (link `aegis_mcp`, include dir, compile def) mirroring the provider blocks.
- New option `AEGIS_MCP` in `cmake/AegisOptions.cmake` (default ON, no libcurl).
- `cmake/AegisTests.cmake`: register `test_mcp_json`, `test_mcp_client`, and the `mock_mcp_server` fixture executable.

---

## Chunk A — `aegis_json` DOM target + loop.c refactor

**Files:**
- Create: `src/mcp/mcp_json.h`, `src/mcp/mcp_json.c`, `src/mcp/CMakeLists.txt`
- Modify: `src/agent/loop.c` (replace static parser with DOM adapter)
- Modify: `src/agent/CMakeLists.txt` (link `aegis_json`)
- Modify: `src/CMakeLists.txt` (`add_subdirectory(mcp)`)

- [ ] **Step A1: Create `src/mcp/mcp_json.h` — JSON value DOM API.**

```c
#ifndef AEGIS_MCP_JSON_H
#define AEGIS_MCP_JSON_H

#include "aegis/types.h"         /* aegis_status_t (verified: enum at include/aegis/types.h L43) */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** A generic JSON value (object / array / string / int / float / bool / null). */
typedef enum aegis_json_type {
    AEGIS_JSON_NULL,
    AEGIS_JSON_BOOL,
    AEGIS_JSON_INT,
    AEGIS_JSON_FLOAT,
    AEGIS_JSON_STRING,
    AEGIS_JSON_ARRAY,
    AEGIS_JSON_OBJECT,
} aegis_json_type_t;

typedef struct aegis_json_value aegis_json_value_t;
struct aegis_json_value {
    aegis_json_type_t type;
    union {
        bool  b;
        int64_t i;
        double f;
        char*  str;                              /* NUL-terminated, owned */
        struct { aegis_json_value_t** items; size_t count; } arr;
        struct { char** keys; aegis_json_value_t** vals; size_t count; } obj;
    };
};

/**
 * @brief Parse a full JSON document into an owned DOM tree.
 *
 * Accepts objects, arrays, strings, numbers, bool and null with arbitrary
 * nesting.
 *
 * @param[in]  json  NUL-terminated JSON text.
 * @param[out] out   Receives the owned tree on success; caller frees with
 *   @ref aegis_json_value_destroy. Initialized to NULL on failure.
 * @return AEGIS_OK on success, AEGIS_ERR_INVALID on malformed input,
 *   AEGIS_ERR_NOMEM on allocation failure.
 */
aegis_status_t aegis_json_parse(const char* json, aegis_json_value_t** out);

/** Recursively free a DOM tree. Safe with NULL. */
void aegis_json_value_destroy(aegis_json_value_t* v);

/** Object lookup by key (NULL if not found or @p obj is not an object). */
const aegis_json_value_t* aegis_json_object_get(const aegis_json_value_t* obj,
                                                const char* key);
/** Array element (NULL if out of range or @p arr is not an array). */
const aegis_json_value_t* aegis_json_array_at(const aegis_json_value_t* arr, size_t i);
/** Array length (0 unless @p arr is an array). */
size_t aegis_json_array_len(const aegis_json_value_t* arr);
/** String payload (NULL unless @p v is a string). */
const char* aegis_json_string(const aegis_json_value_t* v);
/** Integer payload (0 unless @p v is an int). */
int64_t aegis_json_int(const aegis_json_value_t* v);
/** Boolean payload (false unless @p v is a bool). */
bool aegis_json_bool(const aegis_json_value_t* v);

#ifdef __cplusplus
}
#endif

#endif /* AEGIS_MCP_JSON_H */
```

> `aegis_status_t` is defined in `include/aegis/types.h` (verified L43) — the include above is correct as written.

- [ ] **Step A2: Create `src/mcp/mcp_json.c` — recursive-descent parser + accessors + destroy.**

Port the escape/whitespace logic from `loop.c` (`json_skip_ws`, `json_parse_string`) into file-scope statics, then add `json_parse_value(const char** p, const char* end, aegis_json_value_t** out)` handling all seven value types with recursion. Numbers: `strtoll` first (fall back to `strtod` for a non-integral value — or just always parse via `strtod` and tag INT when the literal has no `.`/`e`/`E`, matching loop.c's int/float split). Malformed → return `AEGIS_ERR_INVALID` with `*out` NULL and no leak. `aegis_json_value_destroy` is recursive (object: free keys + each val + arrays; string: free; scalar: nothing). Accessors guard on `type`.

- [ ] **Step A3: Create `src/mcp/CMakeLists.txt` defining both `aegis_json` and (later) `aegis_mcp`.**

For Chunk A, create the `aegis_json` target now:

```cmake
add_library(aegis_json STATIC mcp_json.c)
target_include_directories(aegis_json PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_link_libraries(aegis_json PUBLIC aegis_common)
aegis_set_warnings(aegis_json)
```

(Chunk B appends the `aegis_mcp` target to the same file.)

- [ ] **Step A4: Wire `aegis_json` into the build.**

- `src/CMakeLists.txt`: add `add_subdirectory(mcp)` to the subdir list.
- `src/agent/CMakeLists.txt`: add `aegis_json` to `aegis_agent`'s `target_link_libraries`.

- [ ] **Step A5: Add a DOM unit test (TDD red).**

Create `tests/unit/test_mcp_json.c` (plain C, `assert` + `printf PASS` + `main()`, matching `test_session.c` style). Cases: parse a scalar object `{"a":1,"b":"x","c":[true,null,2.5]}`; assert object/array accessors; nested array-of-object; corrupt input `{"a":}` → `AEGIS_ERR_INVALID`; a string with escapes. Build it (expect link failure until `aegis_json` exists / or pass since A3 created it — run and confirm green).

- [ ] **Step A6: Refactor `loop.c` to use the DOM.**

Replace `json_parse_args`'s body with: `aegis_json_parse(json, &dom)` → walk `dom->obj` pairs → for each val: STRING→`aegis_tool_args_add_string`, INT→`add_int`, FLOAT→`add_float`, BOOL→`add_bool`, NULL→skip; **ARRAY/OBJECT leaf → return failure** (preserve old reject behavior). Delete the now-unused `json_skip_ws`/`json_parse_string` statics if no longer referenced. `loop.c` includes `aegis/mcp_json.h` via the `aegis_json` target's PUBLIC include dir.

- [ ] **Step A7: Build + regression.**

```bash
cmake --build build --parallel 8
cd build && ctest -R "unit_loop|system_coding_loop|unit_mcp_json" --output-on-failure
```

Expected: all green (the loop.c refactor must not change tool-args behavior).

- [ ] **Step A8: Commit.**

```bash
git add src/mcp/ src/agent/loop.c src/agent/CMakeLists.txt src/CMakeLists.txt tests/unit/test_mcp_json.c cmake/AegisTests.cmake
git commit -m "feat(json): add aegis_json DOM parser; refactor loop.c tool-args parse onto it"
```

> If `test_mcp_json` isn't registered yet in `AegisTests.cmake`, add its `add_executable`/`add_test`/asan-env block in this chunk (CMake file — hand-edit, **never** clang-format it).

---

## Chunk B — `mcp_stdio` transport + `mcp_client` public API

**Files:**
- Create: `src/mcp/mcp_stdio.h`, `src/mcp/mcp_stdio.c`, `src/mcp/mcp_client.h`, `src/mcp/mcp_client.c`
- Modify: `src/mcp/CMakeLists.txt` (add `aegis_mcp` target)

- [ ] **Step B1: `mcp_stdio.h` — internal transport.**

```c
typedef struct aegis_mcp_stdio aegis_mcp_stdio_t;

aegis_status_t aegis_mcp_stdio_spawn(aegis_mcp_stdio_t** out, const char* cmd,
                                     const char* const* argv);
void aegis_mcp_stdio_close(aegis_mcp_stdio_t* s);

/**
 * Send one newline-delimited JSON-RPC request, read the matching response.
 * @p params is a DOM value (object). Returns the .result DOM (owned) in
 * @p out_result; .error is surfaced as AEGIS_ERR_PROVIDER.
 */
aegis_status_t aegis_mcp_stdio_request(aegis_mcp_stdio_t* s, uint64_t id,
                                       const char* method,
                                       const aegis_json_value_t* params,
                                       aegis_json_value_t** out_result);
```

- [ ] **Step B2: `mcp_stdio.c` — fork/execvp + two pipes + newline framing.**

`spawn`: `pipe(write_fd)` (parent→child stdin) + `pipe(read_fd)` (child→parent stdout); `fork()`; child: `setpgid(0,0)`, `dup2` both pipes onto FD 0/1/2 (stderr→/dev/null or a capture), `execvp(cmd, argv)`, `_exit(127)`. Parent: close the child-end fds, `poll()`-read lines (accumulate into a growable buffer, split on `\n`) and `write` request lines (request body + `\n`). `request`: encode the JSON-RPC envelope `{jsonrpc:"2.0",id,method,params}` via the existing `aegis_json_builder_*` (`providers/llm/shared/llm_shared.h` — link that target, or re-implement a minimal envelope writer), read one line, `aegis_json_parse` it, return `.result` (or `.error` → `AEGIS_ERR_PROVIDER`). EOF/crash → `AEGIS_ERR_PROVIDER`. `close`: close both pipes, `waitpid`, kill the process group if still alive.

> Confirm `aegis_json_builder_*` visibility: link the `aegis_llm_shared` target (or copy the small builder into `mcp_stdio.c`) — decide in-implementation; simplest is to link `aegis_llm_shared` from `aegis_mcp` in CMake.

- [ ] **Step B3: `mcp_client.h` — public API.**

```c
typedef struct aegis_mcp_client aegis_mcp_client_t;

typedef struct {
    char                 name[128];
    char                 description[256];
    aegis_json_value_t*  input_schema;   /* owned DOM */
} aegis_mcp_tool_info_t;

aegis_status_t aegis_mcp_client_create(const char* server_cmd,
                                       const char* const* server_argv,
                                       aegis_mcp_client_t** out);
void aegis_mcp_client_destroy(aegis_mcp_client_t* c);

/* out_count/out_tools owned by the client (freed on destroy). */
aegis_status_t aegis_mcp_list_tools(aegis_mcp_client_t* c, size_t* out_count,
                                    aegis_mcp_tool_info_t** out_tools);
aegis_status_t aegis_mcp_call_tool(aegis_mcp_client_t* c, const char* name,
                                   const aegis_tool_args_t* args,
                                   aegis_tool_result_t* out);
aegis_status_t aegis_mcp_register_tools(aegis_mcp_client_t* c, aegis_tool_registry_t* reg);
```

- [ ] **Step B4: `mcp_client.c` — handshake + discovery + call + register.**

`create`: `aegis_mcp_stdio_spawn` → send `initialize` (id=1, params `{protocolVersion, clientInfo, capabilities:{}}`) → read result → send `notifications/initialized` (no id) → OK. `list_tools`: `tools/list` → walk `result.tools[]`, copy `name`/`description`, `input_schema` = the `inputSchema` DOM (cloned). `call_tool`: build `tools/call` params `{name, arguments:{...from aegis_tool_args_t→DOM object}}`; send; read `result`; concatenate every `content[i].text` into one string; `aegis_tool_result_set_string(out, text)`; if `result.isError` → still set the string then return `AEGIS_OK` (the agent surfaces it), per spec §3.3. `register_tools`: `list_tools`, then for each tool build an `aegis_tool_def_t` (execute fn = `mcp_tool_execute` which calls `aegis_mcp_call_tool`; `user` = the client; schema params parsed from `input_schema` `properties` + `required`), register into `reg`. Store the allocated per-tool name/desc/param arrays on the client so `destroy` can free them **after** the registry is gone (registry is destroyed by the agent before the client — verify ordering in coding_agent.c Chunk C).

- [ ] **Step B5: `aegis_mcp` CMake target.**

Append to `src/mcp/CMakeLists.txt`:

```cmake
add_library(aegis_mcp STATIC mcp_stdio.c mcp_client.c)
target_include_directories(aegis_mcp PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
    PRIVATE ${PROJECT_SOURCE_DIR}/src/mcp)
target_link_libraries(aegis_mcp PUBLIC aegis_json aegis_tool aegis_common)
aegis_set_warnings(aegis_mcp)
```

(Add `aegis_llm_shared` to the link line if `mcp_stdio.c` uses the JSON builder.)

- [ ] **Step B6: Build + smoke.**

```bash
cmake --build build --parallel 8
```

Expected: compiles clean. (No new ctest yet — the mock-server e2e lands in Chunk C.)

- [ ] **Step B7: Commit.**

```bash
git add src/mcp/mcp_stdio.h src/mcp/mcp_stdio.c src/mcp/mcp_client.h src/mcp/mcp_client.c src/mcp/CMakeLists.txt
git commit -m "feat(mcp): stdio JSON-RPC transport + public MCP client API"
```

---

## Chunk C — coding-agent wiring + CMake option + mock-server tests

**Files:**
- Modify: `cmake/AegisOptions.cmake` (add `AEGIS_MCP`)
- Modify: `include/aegis/coding/coding_agent.h` + `src/coding/coding_agent.c` (config field + create/destroy)
- Modify: `src/coding/CMakeLists.txt` (gated `AEGIS_MCP` block)
- Create: `tests/fixtures/mock_mcp_server.c`, `tests/unit/test_mcp_client.c`
- Modify: `cmake/AegisTests.cmake` (register `test_mcp_client` + `mock_mcp_server`)

- [ ] **Step C1: Add `AEGIS_MCP` option.**

`cmake/AegisOptions.cmake`: `option(AEGIS_MCP "Build the MCP client" ON)` (no dependency).

- [ ] **Step C2: coding-agent config + lifecycle.**

`coding_agent.h`: add to `aegis_coding_agent_config_t` `const char* mcp_cmd; const char* const* mcp_argv;`. In `coding_agent.c`: on `create`, `if (cfg->mcp_cmd) { aegis_mcp_client_create(cfg->mcp_cmd, cfg->mcp_argv, &a->mcp_client); aegis_mcp_register_tools(a->mcp_client, a->tools); }` (only when the agent owns its registry). On `destroy`: `if (a->mcp_client) aegis_mcp_client_destroy(a->mcp_client);` **after** `aegis_tool_registry_destroy` so the client's owned tool defs outlive the registry. Guard all of it under `#ifdef AEGIS_MCP` if the coding target gates on it.

- [ ] **Step C3: `src/coding/CMakeLists.txt` gated block.**

Mirror the provider blocks:

```cmake
if(AEGIS_MCP)
    target_compile_definitions(aegis_coding PRIVATE AEGIS_MCP)
    target_link_libraries(aegis_coding PUBLIC aegis_mcp)
    target_include_directories(aegis_coding PRIVATE ${PROJECT_SOURCE_DIR}/src/mcp)
    set_property(TARGET aegis_coding PROPERTY EXPORT_LINK_INTERFACE_LIBRARIES aegis_mcp)
endif()
```

- [ ] **Step C4: Mock MCP server fixture.**

`tests/fixtures/mock_mcp_server.c`: a tiny stdin-driven program. Loop reading one JSON line; on `initialize` reply `{"jsonrpc":"2.0","id":1,"result":{"protocolVersion":"2024-11-05","capabilities":{}}}`; on `tools/list` reply with one tool `{"name":"echo","description":"echo","inputSchema":{"type":"object","properties":{"x":{"type":"string"}}}}`; on `tools/call` reply `{"jsonrpc":"2.0","id":N,"result":{"content":[{"type":"text","text":"echo:<args>"}]}}`. Emit `\n` after each reply.

- [ ] **Step C5: `tests/unit/test_mcp_client.c` (plain C).**

Spawn the mock server via `aegis_mcp_client_create` (argv = the compiled `mock_mcp_server` path passed as an env var or CMake-defined). Assert `aegis_mcp_list_tools` returns 1 tool named "echo"; `aegis_mcp_call_tool(c, "echo", args{x:"hi"}, &res)` → result string contains "hi". Register into a fresh `aegis_tool_registry_t`, confirm `aegis_tool_registry_find` locates "echo", call through the registry. Destroy everything. `printf("PASS")`.

- [ ] **Step C6: Register tests in `AegisTests.cmake` (hand-edit, never clang-format).**

Add a `mock_mcp_server` `add_executable` (link nothing / just `aegis_common` if needed) and a `test_mcp_client` target linking `aegis_mcp aegis_tool aegis_common` + `add_test`. Gate both under `if(AEGIS_MCP)`.

- [ ] **Step C7: Build + full ctest.**

```bash
cmake --build build --parallel 8
cd build && ctest --output-on-failure
```

Expected: all prior tests green + new `test_mcp_json`, `test_mcp_client` green.

- [ ] **Step C8: Commit.**

```bash
git add cmake/AegisOptions.cmake include/aegis/coding/coding_agent.h src/coding/coding_agent.c src/coding/CMakeLists.txt tests/fixtures/mock_mcp_server.c tests/unit/test_mcp_client.c cmake/AegisTests.cmake
git commit -m "feat(coding): MCP client wiring + CMake option + mock-server tests"
```

---

## Chunk D — final verify

- [ ] **Step D1: Full build + ctest.**

```bash
cmake --build build --parallel 8 && cd build && ctest --output-on-failure
```

- [ ] **Step D2: clang-format the changed C files ONLY (never `.cmake`).**

```bash
clang-format -i src/mcp/mcp_json.c src/mcp/mcp_stdio.c src/mcp/mcp_client.c \
  src/agent/loop.c src/coding/coding_agent.c \
  tests/unit/test_mcp_json.c tests/unit/test_mcp_client.c tests/fixtures/mock_mcp_server.c
# then re-build + ctest to confirm format didn't break anything
```

- [ ] **Step D3: Diff audit + final commit of any format churn.**

```bash
git diff --stat
git add -A && git commit -m "style: clang-format MCP client files"   # only if D2 produced a diff
```

---

## Verification criteria (definition of done)

- [ ] `aegis_json` DOM target built; `loop.c` uses it; `unit_loop` + `system_coding_loop` green.
- [ ] `aegis_mcp` client: spawn/handshake/list/call/register all work against the mock server.
- [ ] Coding agent spawns + registers MCP tools on create (when configured), tears down on destroy.
- [ ] `AEGIS_MCP` option exists; `test_mcp_json` + `test_mcp_client` pass.
- [ ] Full build 100%, ctest all green (73 + new tests), clang-format clean on C files.
- [ ] 3 commits: (A) JSON DOM + loop.c, (B) stdio + client, (C) wiring + tests.

## Rollback

Each chunk is an independent commit — revert with `git revert <sha>`. Chunk A is the foundational refactor (loop.c on the DOM); B and C depend on A.
