# Anthropic Native Provider — Design

**Status:** Design complete, awaiting implementation plan
**Date:** 2026-09-14
**Scope:** Add a native Anthropic Messages API provider alongside the existing
OpenAI-compatible one. Full support: complete + stream + tool_calling +
reasoning (thinking blocks). No MCP, no LLM-compact, no sub-agents.

## 1. Architecture overview

Add a new provider library `aegis_llm_anthropic` mirroring the existing
`aegis_llm_openai`, and extract the provider-agnostic infrastructure that the
two share into a new `aegis_llm_shared` target.

Three CMake targets result:

| Target | Path | Role |
|---|---|---|
| `aegis_llm_shared` | `providers/llm/shared/` | Provider-agnostic infra (JSON builder, curl SSE skeleton, cancellation cooperation). No wire protocol. |
| `aegis_llm_openai` | `providers/llm/openai/` | OpenAI wire format. Now **relinks** to `aegis_llm_shared` instead of owning its own copies. |
| `aegis_llm_anthropic` | `providers/llm/anthropic/` | Anthropic wire format. Links `aegis_llm_shared`. |

**Shared (extracted from `structured_openai.c` into `aegis_llm_shared`):**

- `json_buf_t` + `checked_grow` / `append_raw` / `append_json_string` — the
  growable JSON string builder.
- `sse_progress` — libcurl `XFERINFOFUNCTION` cancellation cooperation.
- The SSE pending-buffer chunk-reassembly skeleton currently inside
  `on_sse_write` (the realloc + `memmove` bookkeeping), as a reusable helper.
- The libcurl easy-setup sequence (connect/timeout/SSL setopts) as a helper.

**Provider-specific (stays in each provider's own `.c`):**

- `append_message` / `append_tool_json` / `build_body` — wire body shape.
- `emit_record` — SSE record parsing (OpenAI `data:` vs Anthropic `event:`+`data:`).
- `parse_complete_response` — one-shot response parsing.
- `sse_state_t` — per-provider stream state.

OpenAI's `openai_llm.c` (legacy blob adapter) is **not** touched.

## 2. `providers/llm/anthropic/` layout

Files (mirror the openai directory):

- `structured_anthropic.c` / `.h` — ctx factory + wire layer.
- `structured_anthropic_test.h` — test seam, compiled under
  `AEGIS_ANTHROPIC_TEST_API`, exposes `aegis_anthropic_parse_complete_response`.
- `CMakeLists.txt` — target `aegis_llm_anthropic` (STATIC, links
  `aegis_llm_shared`, `${CURL_LIBRARIES}`, `aegis_common aegis_model
  aegis_message aegis_tool`).

Context:

```c
typedef struct {
    char* api_key;  /* x-api-key header */
    char* base_url; /* default https://api.anthropic.com */
    char* model;    /* default claude-sonnet-4-5 */
} aegis_anthropic_model_ctx_t;
```

`aegis_anthropic_model_create(api_key, base_url, model, out_ctx, backend)`:
- capabilities = `TEXT | TOOL_CALLING | STREAMING | REASONING`.
- Endpoint: `POST {base}/v1/messages` (base default
  `https://api.anthropic.com`).
- Headers: `x-api-key: <key>` (NOT Bearer), `anthropic-version: 2023-06-01`,
  `Content-Type: application/json`. Env fallback `ANTHROPIC_API_KEY`.

### `build_body` — wire mapping (OpenAI → Anthropic)

| OpenAI field | Anthropic equivalent |
|---|---|
| `role:"system"` message | Top-level `"system"` param; multiple system messages joined into an array of `{type:"text",text}` |
| `role:"tool"` + `tool_call_id` | `user` message content block `{type:"tool_result",tool_use_id,content}` |
| assistant `tool_calls[]` (function-nested) | assistant content-block array with `{type:"tool_use",id,name,input}` — `input` is an object, not a string |
| `tools[]` `{type:"function",function:{name,parameters}}` | `tools[]` `{name,description,input_schema:{type:object,properties,required}}` (flattened) |
| — | `max_tokens` is **required** |
| — | `thinking:{type:"enabled",budget_tokens:N}` when `req->thinking_budget>0`; requires `max_tokens > N` |

Content model: a user message is either a plain string or a content-block
array (text / tool_result / tool_use / thinking).

### SSE state machine (Anthropic: `event:` line + `data:` JSON)

```
message_start        → capture usage.input_tokens
content_block_start  → (index, content_block.type)
   text      → text_delta          → TEXT_DELTA
   thinking  → thinking_delta      → REASONING_DELTA; signature_delta ignored
   tool_use  → TOOL_CALL_START (id, name); input_json_delta accumulates partial JSON
content_block_stop   → tool_use block: parse accumulated args → TOOL_CALL_END
message_delta        → usage.output_tokens (cumulative overwrite)
message_stop         → emit USAGE + END
error event          → ERROR
```

Key difference from OpenAI: tool-call arguments are accumulated across
`input_json_delta` and parsed **once** at `content_block_stop`, rather than
emitted as incremental `TOOL_CALL_DELTA` events. (`ping` keepalive is
ignored.)

Usage accounting: `input_tokens` arrives in `message_start`;
`output_tokens` in `message_delta` (cumulative overwrite, not additive).
Input may carry `cache_read_input_tokens` /
`cache_creation_input_tokens` variants — total input = sum of all present.

HTTP status mapping is identical to OpenAI: `429` →
`AEGIS_ERR_MODEL_RATE_LIMIT`, `413` → `AEGIS_ERR_CONTEXT_OVERFLOW`, other
non-2xx → `AEGIS_ERR_PROVIDER`. Cancellation still takes precedence over
provider errors.

## 3. Coding agent, request, CMake, tests

### `coding_agent.c` — type erasure

- Replace `aegis_openai_model_ctx_t* openai_model` with
  `void* provider_ctx` + `void (*provider_destroy)(void*)`.
- At create time, bind the matching `aegis_*_model_destroy` per provider
  string; the single destroy path calls `provider_destroy(provider_ctx)`.
- Dispatch: `strcmp(provider, "llm-anthropic") == 0` →
  `build_anthropic_model()` (a new static fn, `#ifdef AEGIS_ANTHROPIC_PROVIDER`),
  parallel to the existing `build_openai_model`.
- Update the file's `@file` / create doc comments (currently
  "OpenAI backend when configured, mock otherwise").

### `request.h`

- Add `uint32_t thinking_budget;` to `aegis_model_request_t` (0 = disabled,
  backward compatible). Existing consumers leave it 0.

### CMake wiring

- New option `AEGIS_ANTHROPIC_PROVIDER` (default ON, requires libcurl).
- New targets `aegis_llm_shared` and `aegis_llm_anthropic`.
- `aegis_llm_openai` relinks to `aegis_llm_shared` (its static infra fns become
  exported from the shared target).
- Top-level: when `AEGIS_ANTHROPIC_PROVIDER` is ON, `aegis` links
  `aegis_llm_anthropic` and gets the `AEGIS_ANTHROPIC_PROVIDER` compile def.
- `cmake/AegisInstall.cmake`: add `aegis_llm_anthropic` + `aegis_llm_shared`.

### Testing (mirror the OpenAI suite)

- Unit: `tests/unit/test_structured_anthropic.c` — exercises the
  `aegis_anthropic_parse_complete_response` seam and SSE event parsing.
- System e2e: Anthropic variants of `sse_e2e` / `tool_loop_e2e` /
  `http_status_e2e`; add an `AEGIS_ANTHROPIC_PROVIDER` gate block in
  `cmake/AegisTests.cmake` next to the existing OpenAI block.
- Coding-agent provider-switch e2e: mock → `llm-anthropic`, verify dispatch.

## 4. Scope OUT

MCP protocol, LLM-based context compact, sub-agent/task delegation, and skill
progressive-disclosure are separate pi-agent gaps and are **not** part of
this work.
