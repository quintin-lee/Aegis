# Anthropic Native Provider Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a native Anthropic Messages API provider (complete + stream + tool_calling + reasoning) alongside the existing OpenAI one, sharing provider-agnostic HTTP/JSON infrastructure.

**Architecture:** New `aegis_llm_shared` target extracts the JSON builder, curl SSE skeleton, and cancellation cooperation from `structured_openai.c`; OpenAI relinks to it. New `aegis_llm_anthropic` target implements the Anthropic wire format. `coding_agent.c` gains type-erased provider dispatch. `request.h` gains a `thinking_budget` field.

**Tech Stack:** C11, CMake, libcurl, CTest.

**Spec:** `docs/superpowers/specs/2026-09-14-anthropic-provider-design.md`

---

## Conventions (read once, apply everywhere)

- Build: `cmake --build build --parallel 8` (reconfigure after CMake changes: `cmake -S . -B build`).
- Test: `cd build && ctest --output-on-failure --timeout 120`.
- Format: `clang-format -i <file>` on any C/CMake file you add/edit.
- Commit style: Conventional Commits, lowercase imperative, scope optional. Match history (`git log --oneline -5`).
- Every public function in a new `.c` gets a `/** @brief ... @param[in/out] ... @return ... */` docstring, English, matching the repo's existing Doxygen style (see `structured_openai.c` top).
- New provider files use `#ifndef` include guards (repo convention), `extern "C"` guards, `_POSIX_C_SOURCE 200809L`.
- Do NOT touch `providers/llm/openai/openai_llm.c` (legacy blob adapter).

---

## Chunk 1: Shared infrastructure target

**Files:**
- Create: `providers/llm/shared/llm_shared.h`
- Create: `providers/llm/shared/json_builder.c`
- Create: `providers/llm/shared/sse_http.c`
- Create: `providers/llm/shared/CMakeLists.txt`
- Modify: `providers/llm/openai/structured_openai.c` (delete the infra fns now in shared, use shared)
- Modify: `providers/llm/openai/structured_openai.h` (add `#include "llm_shared.h"` if it exposes json helpers; else no change)
- Modify: `providers/llm/openai/CMakeLists.txt` (link `aegis_llm_shared`)
- Modify: `CMakeLists.txt` (add `add_subdirectory(providers/llm/shared)` before the openai block)

### Task 1.1: Create the shared JSON builder

The JSON builder is provider-agnostic. Move it out of `structured_openai.c` into
`providers/llm/shared/json_builder.c` and export it.

- [ ] **Step 1: Create `providers/llm/shared/llm_shared.h`**

```c
#ifndef AEGIS_LLM_SHARED_H
#define AEGIS_LLM_SHARED_H

#include "aegis/common/cancellation/cancellation.h"
#include "aegis/model/model.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file llm_shared.h
 * @brief Provider-agnostic infrastructure shared by LLM backends:
 * growable JSON builder, libcurl SSE/cancel helpers, response-size caps.
 * No wire protocol here.
 */

#define AEGIS_LLM_MAX_RESPONSE (16u * 1024u * 1024u)

/** Growable NUL-terminated buffer for JSON request bodies. */
typedef struct {
    char*  data;
    size_t len;
    size_t cap;
} llm_json_buf_t;

/** Grow @p b to fit @p extra more bytes. @return 1 ok, 0 OOM/overflow. */
int aegis_llm_json_grow(llm_json_buf_t* b, size_t extra);

/** Append @p n raw bytes from @p s. @return 1 ok, 0 failure. */
int aegis_llm_json_append_raw(llm_json_buf_t* b, const char* s, size_t n);

/** Append @p s as a JSON-quoted+escaped string. @return 1 ok, 0 failure. */
int aegis_llm_json_append_string(llm_json_buf_t* b, const char* s);

/** @brief Cooperative-cancel check for libcurl XFERINFO callbacks.
 * @return non-zero (abort) when @p s points to a cancelled token, else 0.
 * @p s is a pointer to a struct with a `const aegis_cancellation_token_t* token`.
 */
int aegis_llm_sse_progress(void* user, curl_off_t dltotal, curl_off_t dlnow,
                           curl_off_t ultotal, curl_off_t ulnow);

#ifdef __cplusplus
}
#endif

#endif /* AEGIS_LLM_SHARED_H */
```

Note: `llm_shared.h` needs `curl/curl.h` for `curl_off_t`. Add
`#include <curl/curl.h>` after the std includes.

- [ ] **Step 2: Create `providers/llm/shared/json_builder.c`**

Copy `checked_grow`, `append_raw`, `append_json_string` from
`structured_openai.c` (lines 58–148), rename them to the exported names above,
and drop `static`. Keep bodies identical (including the overflow math).

- [ ] **Step 3: Create `providers/llm/shared/CMakeLists.txt`**

```cmake
# ── providers/llm/shared/CMakeLists.txt ─────────────────────────────────
add_library(aegis_llm_shared STATIC
    json_builder.c
    sse_http.c
)
target_include_directories(aegis_llm_shared PUBLIC ${PROJECT_SOURCE_DIR}/include
                                                  ${PROJECT_SOURCE_DIR}/providers/llm/shared)
target_link_libraries(aegis_llm_shared PUBLIC aegis_common aegis_model
                                              PRIVATE ${CURL_LIBRARIES} ${CURL_INCLUDE_DIRS})
aegis_set_warnings(aegis_llm_shared)
```

- [ ] **Step 4: Create `providers/llm/shared/sse_http.c`**

Move `sse_progress` (structured_openai.c lines 572–584) here as
`aegis_llm_sse_progress`. Its `user` is cast to a struct containing a
`const aegis_cancellation_token_t* token;` member — keep that contract and
document it in the header. (See: it is called as
`CURLOPT_XFERINFOFUNCTION` with `&state` where `state.token` holds the token.)

- [ ] **Step 5: Wire `CMakeLists.txt`**

After the `# ── Optional providers ──` comment block (line 68), and BEFORE the
OpenAI block, add:

```cmake
if(AEGIS_OPENAI_PROVIDER OR AEGIS_ANTHROPIC_PROVIDER)
    find_package(CURL REQUIRED)
endif()
add_subdirectory(providers/llm/shared)
```

(Do not double-`find_package(CURL)` — guard it. The existing openai block at
line 69 keeps its own `find_package(CURL REQUIRED)` guarded by
`AEGIS_OPENAI_PROVIDER`; to avoid duplication, remove that line and let the
new guarded block above handle both. Net: replace lines 68–75 with the guarded
`add_subdirectory(providers/llm/shared)` plus the openai subdirectory logic.)

- [ ] **Step 6: Build (will fail until OpenAI rewired — expected)**

Run: `cmake -S . -B build && cmake --build build --parallel 8`
Expected: configure succeeds; build may show the shared lib compiling.

Commit at the end of the chunk, not per-task.

### Task 1.2: Relink OpenAI to the shared target

- [ ] **Step 1: Edit `providers/llm/openai/structured_openai.c`**

Delete `json_buf_t` struct def, `checked_grow`, `append_raw`,
`append_json_string`, and `sse_progress` (now in shared). Add
`#include "llm_shared.h"`. Replace call sites:
- `json_buf_t` → `llm_json_buf_t`
- `checked_grow(b, x)` → `aegis_llm_json_grow(b, x)`
- `append_raw(b, s, n)` → `aegis_llm_json_append_raw(b, s, n)`
- `append_json_string(b, s)` → `aegis_llm_json_append_string(b, s)`
- `OPENAI_MAX_RESPONSE` → `AEGIS_LLM_MAX_RESPONSE`
- `sse_progress` → `aegis_llm_sse_progress`

Keep `json_buf_t` usages in `tool_json_context_t` (line 202, 213) updated to
`llm_json_buf_t`.

- [ ] **Step 2: Edit `providers/llm/openai/CMakeLists.txt`**

Add `aegis_llm_shared` to `target_link_libraries(aegis_llm_openai PUBLIC ...)`
and add `${PROJECT_SOURCE_DIR}/providers/llm/shared` to
`target_include_directories(... PRIVATE ...)`.

- [ ] **Step 3: Build + format + commit**

```bash
cmake -S . -B build && cmake --build build --parallel 8
clang-format -i providers/llm/openai/structured_openai.c providers/llm/shared/*.c providers/llm/shared/*.h
cd build && ctest --output-on-failure --timeout 120
git add -A && git commit -m "refactor: extract shared LLM JSON/SSE infra into aegis_llm_shared"
```

Expected: build clean, all existing tests still pass (OpenAI behavior
unchanged — only its backing moved to shared).

---

## Chunk 2: Anthropic provider

**Files:**
- Create: `providers/llm/anthropic/structured_anthropic.h`
- Create: `providers/llm/anthropic/structured_anthropic.c`
- Create: `providers/llm/anthropic/structured_anthropic_test.h`
- Create: `providers/llm/anthropic/CMakeLists.txt`

### Task 2.1: Anthropic header + ctx factory

- [ ] **Step 1: Create `providers/llm/anthropic/structured_anthropic.h`**

Mirror `structured_openai.h` (lines 1–29). Guard `AEGIS_STRUCTURED_ANTHROPIC_H`.
Declare:

```c
typedef struct aegis_anthropic_model_ctx aegis_anthropic_model_ctx_t;
aegis_status_t aegis_anthropic_model_create(const char* api_key, const char* base_url,
                                            const char* model, aegis_anthropic_model_ctx_t** out,
                                            aegis_model_backend_t* backend);
void           aegis_anthropic_model_destroy(aegis_anthropic_model_ctx_t* ctx);
```

- [ ] **Step 2: Create `structured_anthropic.c` ctx skeleton**

Copy the structure of `structured_openai.c`'s `aegis_openai_model_create` /
`aegis_openai_model_destroy` (lines 902–939). Defaults:
`ANTHROPIC_DEFAULT_URL = "https://api.anthropic.com"`,
`ANTHROPIC_DEFAULT_MODEL = "claude-sonnet-4-5"`.
Capabilities: `TEXT | TOOL_CALLING | STREAMING | REASONING`.
Env key fallback: `ANTHROPIC_API_KEY`.

- [ ] **Step 3: Build check (stub complete/stream)**

`complete`/`stream` may still be `NULL` temporarily; but
`aegis_model_client_create_with_backend` requires one of them, so for the
create test to succeed keep both wired to real fns (implement in next task).
Commit only after Task 2.2 + 2.3 so the target links.

### Task 2.2: Anthropic request body (`build_body`)

Implement Anthropic wire mapping (spec §2). This is the largest provider fn.

- [ ] **Step 1: Write `build_body(const aegis_model_request_t* req, const aegis_anthropic_model_ctx_t* ctx)`**

Rules (spec §2 table):
- Emit `{"model":"<model>"}`.
- **System extraction:** walk `req->messages`; every message with role
  `AEGIS_MESSAGE_SYSTEM` is pulled out into a top-level `"system"` field.
  If exactly one system message → `"system":"<text>"`. If several →
  `"system":[{"type":"text","text":"..."},...]`. If none → omit.
- **messages array** (remaining messages, in order):
  - `USER` with plain text → `"content":"<text>"` (string form).
  - `TOOL` (tool result) → must become a `user` message with a
    `{"type":"tool_result","tool_use_id":"<id>","content":"<content>"}` block.
    **Merge consecutive tool results into a single `user` message with a
    content-block array** (Anthropic requires tool_result to be in a user
    turn). If the tool result content is non-text, send `content` as the
    string.
  - `ASSISTANT` with `tool_call_count>0` → content-block array of
    `{"type":"tool_use","id":"<id>","name":"<name>","input":{...}}`. `input`
    must be a JSON **object**: parse the tool call's `arguments` string as JSON
    (if it is a JSON object). If the arguments string is not valid JSON or is
    empty, use `{}`. Prefix the array with a `{"type":"text","text":"..."}`
    block first when the assistant also has non-empty text content.
  - `USER`/`ASSISTANT` with only text → `"content":"<text>"`.
- **tools:** from `req->tools` via `aegis_tool_registry_visit`; each emits
  `{"name":"...","description":"...","input_schema":{"type":"object","properties":{...},"required":[...]}}`
  (flattened; `input_schema` not `function.parameters`). Reuse the param
  iteration logic from `append_tool_json` (structured_openai.c lines 207–283)
  but with the flattened Anthropic shape.
- **max_tokens:** required. Use `req->max_tokens` if set, else default
  `4096`. If `req->thinking_budget>0`, ensure `max_tokens > thinking_budget`
  (if not, bump `max_tokens = thinking_budget + 4096` locally).
- **thinking:** when `req->thinking_budget>0`, emit
  `"thinking":{"type":"enabled","budget_tokens":<N>}`.
- **stream:** when `req->stream`, emit `"stream":true`.

Return the heap JSON string (caller frees), or NULL on failure.

- [ ] **Step 2: Add a test seam**

Create `structured_anthropic_test.h` exposing (compiled under
`AEGIS_ANTHROPIC_TEST_API`) a pure function
`aegis_anthropic_build_body_for_test(const aegis_model_request_t*, const char* model)`
returning the malloc'd body string, so the unit test can assert on the exact
JSON without a network.

### Task 2.3: Anthropic SSE + complete parsing

- [ ] **Step 1: Implement `aegis_anthropic_sse_state_t` + record parser**

Anthropic SSE = `event:` line + `data:` JSON (differs from OpenAI's bare
`data:`). State machine (spec §2):
- Track `event_type` from the `event:` line, parse `data:` JSON per event.
- `message_start` → capture `message.usage.input_tokens`.
- `content_block_start` → note block type + `index` + for tool_use its `id`/`name`.
- `content_block_delta`:
  - `text_delta` → emit `TEXT_DELTA` (decode `text`).
  - `thinking_delta` → emit `REASONING_DELTA` (decode `thinking`).
  - `signature_delta` → ignore.
  - `input_json_delta` → append `partial_json` to a per-index args buffer.
- `content_block_stop` → for tool_use index, parse accumulated args (as a
  JSON object string, keep raw) → emit `TOOL_CALL_END`.
- `message_delta` → capture `usage.output_tokens` (overwrite cumulative).
- `message_stop` → emit `USAGE` (input/output/total) then `END`.
- `error` → emit `ERROR`.
- `ping` → ignore.
- Accumulate tool_call args and emit `TOOL_CALL_START` at
  `content_block_start` for tool_use (with `id` + `name`).

- [ ] **Step 2: Implement `structured_stream` / `structured_complete`**

Mirror `structured_stream` / `structured_complete` (structured_openai.c
lines 630–900) but:
- URL: `POST {base}/v1/messages`.
- Headers: `x-api-key: <key>`, `anthropic-version: 2023-06-01`,
  `Content-Type: application/json`, and for stream also
  `Accept: text/event-stream`.
- complete parsing: parse the `content` array; concatenate `text` blocks;
  `usage.input_tokens`/`usage.output_tokens`/`usage.total_tokens` (compute
  total = input+output if absent). Expose
  `aegis_anthropic_parse_complete_response` via the test seam.
- HTTP mapping: `429`→`MODEL_RATE_LIMIT`, `413`→`CONTEXT_OVERFLOW`,
  other non-2xx→`PROVIDER`. Cancellation precedence as OpenAI.

- [ ] **Step 3: Wire `create`/`destroy` and build the target**

`aegis_anthropic_model_create` sets `backend->complete/stream`, fills caps
from Task 2.1. Create `providers/llm/anthropic/CMakeLists.txt`:

```cmake
add_library(aegis_llm_anthropic STATIC
    structured_anthropic.c
)
set_source_files_properties(structured_anthropic.c PROPERTIES COMPILE_DEFINITIONS AEGIS_ANTHROPIC_TEST_API)
target_include_directories(aegis_llm_anthropic PUBLIC ${PROJECT_SOURCE_DIR}/include
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/providers/llm/anthropic>)
target_link_libraries(aegis_llm_anthropic PUBLIC aegis_common aegis_model aegis_message aegis_tool aegis_llm_shared
    PRIVATE ${CURL_LIBRARIES} ${CURL_INCLUDE_DIRS})
aegis_set_warnings(aegis_llm_anthropic)
```

- [ ] **Step 4: Build + commit**

```bash
cmake -S . -B build && cmake --build build --parallel 8
clang-format -i providers/llm/anthropic/*.c providers/llm/anthropic/*.h
git add -A && git commit -m "feat: add native Anthropic LLM provider (aegis_llm_anthropic)"
```

---

## Chunk 3: `coding_agent.c` type-erased dispatch + `request.h` field

**Files:**
- Modify: `include/aegis/model/request.h:19-28`
- Modify: `src/coding/coding_agent.c`
- Modify: `CMakeLists.txt` (anthropic option + link)
- Modify: `cmake/AegisOptions.cmake`

### Task 3.1: Add `thinking_budget` to the request

- [ ] **Step 1: Edit `include/aegis/model/request.h`**

Add after `max_tokens` (line 23):

```c
    uint32_t                     thinking_budget; /**< 0 = off; else budget_tokens for reasoning */
```

With doc comment: `< Rejected by providers without REASONING capability (treated as 0). >`
Update `@file` doc to mention the optional thinking budget.

### Task 3.2: Type-erase the provider context in `coding_agent.c`

- [ ] **Step 1: Replace the OpenAI-only ctx field**

In `struct aegis_coding_agent` (lines 30–50), replace:

```c
#ifdef AEGIS_OPENAI_PROVIDER
    aegis_openai_model_ctx_t* openai_model;
#endif
```

with:

```c
    void* provider_ctx;
    void (*provider_destroy)(void*);
```

(Always compiled, no `#ifdef` — keeps the struct layout stable across
configurations.)

- [ ] **Step 2: Add `build_anthropic_model` + provider builders**

Keep `build_openai_model` (guarded `#ifdef AEGIS_OPENAI_PROVIDER`). Add:

```c
#ifdef AEGIS_ANTHROPIC_PROVIDER
static aegis_status_t build_anthropic_model(aegis_coding_agent_t* a, const char* model_name,
                                            void** out_ctx, aegis_model_client_t** out_client);
#endif
```

Implement it mirroring `build_openai_model` (lines 78–94): create ctx via
`aegis_anthropic_model_create`, wrap with `aegis_model_client_create_with_backend`,
on client failure destroy the ctx and null `*out_ctx`.

Add a helper that returns the destroy fn per provider string:
`provider_ctx` is filled by `build_*_model` and `provider_destroy` is set to
`aegis_openai_model_destroy` / `aegis_anthropic_model_destroy` / `NULL` (mock).

- [ ] **Step 3: Rework `create` dispatch (line ~143–150)**

```c
if (a->provider && strcmp(a->provider, "llm-openai") == 0) {
#ifdef AEGIS_OPENAI_PROVIDER
    st = build_openai_model(a, model_name, &a->provider_ctx, &a->model);
    a->provider_destroy = aegis_openai_model_destroy;
#endif
} else if (a->provider && strcmp(a->provider, "llm-anthropic") == 0) {
#ifdef AEGIS_ANTHROPIC_PROVIDER
    st = build_anthropic_model(a, model_name, &a->provider_ctx, &a->model);
    a->provider_destroy = aegis_anthropic_model_destroy;
#endif
} else {
    st = aegis_model_client_create(model_name, &a->model);
    a->provider_destroy = NULL;
}
```

(Guard so a provider string with its option OFF falls through to mock with a
log — see error-handling note. If neither `#ifdef` is defined for that string,
fall back to `aegis_model_client_create` and set `provider_destroy=NULL`.)

- [ ] **Step 4: Rework `set_model` dispatch (line ~559–631)**

Same shape as create: branch on `a->provider`, build into `new_ctx`/`new_client`,
set `new_destroy`; on success swap `a->provider_ctx`/`a->provider_destroy`/`a->model`,
calling the OLD `provider_destroy` on the old ctx before `free(old_name)`.
Replace every `a->openai_model` / `aegis_openai_model_destroy(...)` reference
with the generic `a->provider_ctx` / `a->provider_destroy` pattern.

- [ ] **Step 5: Rework `destroy` (line ~265–283)**

Replace the `#ifdef AEGIS_OPENAI_PROVIDER` block:

```c
    if (a->model) {
        aegis_model_client_destroy(a->model);
    }
    if (a->provider_destroy && a->provider_ctx) {
        a->provider_destroy(a->provider_ctx);
    }
```

- [ ] **Step 6: Add includes**

```c
#ifdef AEGIS_ANTHROPIC_PROVIDER
#include "structured_anthropic.h"
#endif
```

- [ ] **Step 7: Update doc comments**

`@file` header: "mock model by default; OpenAI or Anthropic backend when
configured." Create doc: mention both providers. `set_model` doc: "OpenAI or
Anthropic backend when the stored provider names one, plain client otherwise."

### Task 3.3: CMake wiring for the Anthropic option

- [ ] **Step 1: `cmake/AegisOptions.cmake`** — add:

```cmake
option(AEGIS_ANTHROPIC_PROVIDER "Build native Anthropic LLM provider (requires libcurl)" ON)
```

- [ ] **Step 2: `CMakeLists.txt`** — in the providers block, after the OpenAI
subdirectory, add:

```cmake
if(AEGIS_ANTHROPIC_PROVIDER)
    add_subdirectory(providers/llm/anthropic)
    target_link_libraries(aegis PRIVATE aegis_llm_anthropic)
    target_compile_definitions(aegis PRIVATE AEGIS_ANTHROPIC_PROVIDER)
endif()
```

And update the shared-lib guard so it builds when either provider is on:
`if(AEGIS_OPENAI_PROVIDER OR AEGIS_ANTHROPIC_PROVIDER) add_subdirectory(providers/llm/shared) endif()`.

- [ ] **Step 3: `cmake/AegisInstall.cmake`** — add `aegis_llm_anthropic`
and `aegis_llm_shared` to the installed-target list (line ~24).

- [ ] **Step 4: Build + format + commit**

```bash
cmake -S . -B build && cmake --build build --parallel 8
clang-format -i src/coding/coding_agent.c include/aegis/model/request.h
cd build && ctest --output-on-failure --timeout 120
git add -A && git commit -m "feat: dispatch Anthropic provider in coding agent + thinking_budget request field"
```

---

## Chunk 4: Tests

**Files:**
- Create: `tests/unit/test_structured_anthropic.c`
- Create: `tests/system/test_anthropic_sse_e2e.c`
- Create: `tests/system/test_anthropic_tool_loop_e2e.c`
- Create: `tests/system/test_anthropic_http_status_e2e.c`
- Modify: `cmake/AegisTests.cmake`

### Task 4.1: Unit test for Anthropic parsing + body build

- [ ] **Step 1: Write `tests/unit/test_structured_anthropic.c`**

Mirror `tests/unit/test_structured_openai.c`:
- Assert `aegis_anthropic_model_create` returns OK, ctx + backend complete/stream non-NULL, caps include REASONING.
- Build a request (system + user + an assistant-with-tool_call + a tool result), call the `aegis_anthropic_build_body_for_test` seam, assert the JSON contains `"system":`, `"tool_use"`, `"tool_result"`, `"input_schema"`, and when `thinking_budget` set, `"thinking"`.
- Call `aegis_anthropic_parse_complete_response` on a canned
  `{"content":[{"type":"text","text":"hi"}],"usage":{"input_tokens":2,"output_tokens":3}}`
  and assert the response message content == "hi" and usage totals.

- [ ] **Step 2: Register in `cmake/AegisTests.cmake`**

Under a new `if(AEGIS_ANTHROPIC_PROVIDER)` block (after the openai block, ~line 322):

```cmake
add_executable(unit_structured_anthropic tests/unit/test_structured_anthropic.c)
target_include_directories(unit_structured_anthropic PRIVATE ${PROJECT_SOURCE_DIR}/providers/llm/anthropic)
target_link_libraries(unit_structured_anthropic PRIVATE aegis_llm_anthropic)
aegis_set_warnings(unit_structured_anthropic)
add_test(NAME unit_structured_anthropic COMMAND unit_structured_anthropic)
```

- [ ] **Step 3: Run**

`cmake -S . -B build && cmake --build build --parallel 8 && cd build && ctest -R unit_structured_anthropic --output-on-failure`
Expected: PASS.

### Task 4.2: System e2e tests

- [ ] **Step 1: Copy the three OpenAI e2e tests** to Anthropic variants
(`test_anthropic_sse_e2e.c`, `test_anthropic_tool_loop_e2e.c`,
`test_anthropic_http_status_e2e.c`), replacing the OpenAI includes/ctx with
Anthropic and asserting the Anthropic wire shape where they inspect JSON.
These are network-gated (skipped without a real endpoint), same as the OpenAI
ones.

- [ ] **Step 2: Register** the three in `AegisTests.cmake` under the
`AEGIS_ANTHROPIC_PROVIDER` gate (mirror lines 281–313), linking
`aegis_llm_anthropic` (+ `aegis_agent aegis_session aegis_model aegis_tool`
where the OpenAI counterparts do).

- [ ] **Step 3: Build + full ctest + commit**

```bash
cmake -S . -B build && cmake --build build --parallel 8
clang-format -i tests/unit/test_structured_anthropic.c tests/system/test_anthropic_*.c
cd build && ctest --output-on-failure --timeout 120
git add -A && git commit -m "test: add Anthropic provider unit + e2e tests"
```

---

## Chunk 5: Final verification

- [ ] **Step 1: Full clean build + all tests**

```bash
cmake -S . -B build && cmake --build build --parallel 8
cd build && ctest --output-on-failure --timeout 120
```
Expected: build 100%, all prior tests + new Anthropic tests pass.

- [ ] **Step 2: Verify both options OFF still builds**

```bash
cmake -S . -B build-noprov -DAEGIS_OPENAI_PROVIDER=OFF -DAEGIS_ANTHROPIC_PROVIDER=OFF
cmake --build build-noprov --parallel 8
```
Expected: coding agent falls back to mock; no provider libs linked. (Clean up
`build-noprov` after: `rm -rf build-noprov`.)

- [ ] **Step 3: Format check across all touched files**

```bash
for f in providers/llm/shared/*.{c,h} providers/llm/anthropic/*.{c,h} providers/llm/openai/structured_openai.c src/coding/coding_agent.c include/aegis/model/request.h; do clang-format --dry-run "$f" | grep warning; done
```
Expected: no warnings.

- [ ] **Step 4: Confirm diff is as scoped** — `git diff --stat` since `6b7be81`
should touch only: `providers/llm/shared/**`, `providers/llm/anthropic/**`,
`providers/llm/openai/structured_openai.c` + its CMakeLists, `src/coding/coding_agent.c`,
`include/aegis/model/request.h`, `CMakeLists.txt`, `cmake/AegisOptions.cmake`,
`cmake/AegisInstall.cmake`, `cmake/AegisTests.cmake`, and the new test files.

---

## Risk notes

- **OpenAI relink (Chunk 1)** is the riskiest change: it moves working code.
  Verify with a full ctest run before proceeding — OpenAI e2e/unit tests must
  stay green. If the relink destabilizes OpenAI, fall back: keep OpenAI's
  statics in place and have Anthropic own its own copies (drop `aegis_llm_shared`
  reuse for OpenAI; Anthropic still links shared).
- **tool_result merging** (Chunk 2.2): consecutive tool results must fold into
  one `user` message. If the session's message list interleaves user text
  between tool results, emit separate user messages rather than one giant
  array. Test 4.1's body assertion covers the single-user-tool-result case.
- **thinking_budget** defaults to 0 everywhere; only Anthropic reads it. No
  existing consumer sets it, so behavior is unchanged until opted in.
- `provider_ctx` is always-compiled (no `#ifdef`), so the struct layout is
  stable; a provider string with its option OFF falls back to mock — add a
  one-line log so silent fallback is visible.
