# Structured OpenAI Coding Loop Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Connect the Coding Agent to a real OpenAI-compatible streaming provider and complete one reliable text/tool-call/tool-result/final-response loop without changing the legacy autonomous LLM pipeline.

**Architecture:** Keep `aegis_llm_*` and the existing autonomous planner unchanged. Add a structured provider adapter behind `aegis_model_*`; the adapter serializes message lists and tool schemas into OpenAI Chat Completions requests, parses SSE deltas into `aegis_model_stream_event_t`, and the agent loop assembles tool calls, validates JSON arguments, executes registered tools, appends tool results, and continues until a text-only assistant response or a bounded turn limit. Tests use a local mock HTTP/SSE server or deterministic provider fixture and never require external credentials.

**Tech Stack:** C11, libcurl, CMake/CTest, existing Aegis message/model/tool/security APIs, local POSIX test server or fixture.

---

## File Map

**Modify:**
- `include/aegis/model/model.h` — extend model client construction/configuration with a structured provider backend while preserving existing callers.
- `include/aegis/model/request.h` — expose request fields needed for model/provider serialization if missing.
- `include/aegis/model/stream.h` — ensure text, reasoning, tool-call start/delta/end, usage, end, and error events carry sufficient data.
- `src/model/model.c` — replace the production coding path's unconditional mock behavior with provider-backed dispatch; retain an explicit mock backend for existing tests.
- `src/agent/loop.c` — implement tool-call accumulation, argument conversion, real registry execution, tool-result messages, max-turn protection, and error cleanup.
- `src/coding/coding_agent.c` — construct/configure the model client with the selected provider and project/tool context.
- `apps/aegis/cli_interactive.c` — pass provider/model options where applicable and ensure output reflects streamed/final responses.
- `providers/llm/openai/openai_llm.c` — add reusable structured request serialization and robust SSE parsing, without removing legacy blob completion.
- `providers/llm/openai/openai_llm.h` — publish structured adapter factory/configuration declarations.
- `providers/llm/openai/CMakeLists.txt` — link the model/message/tool dependencies needed by the structured adapter.
- `CMakeLists.txt` or `cmake/AegisLibraries.cmake` — ensure the adapter target is linked into coding-agent consumers.
- `cmake/AegisTests.cmake` — register focused structured-provider and coding-loop integration tests.

**Create:**
- `tests/support/mock_openai_server.c` or equivalent existing test-support location — deterministic local HTTP/SSE fixture if no existing test server abstraction is suitable.
- `tests/unit/test_openai_structured.c` — serialization, SSE parsing, malformed input, cancellation, and HTTP error tests.
- `tests/system/test_coding_agent_tool_loop.c` — end-to-end read-tool then final-answer flow.

Do not introduce an external JSON dependency unless the project already contains one; first implement a bounded parser appropriate for the required OpenAI response subset, or reuse an existing internal parser if available.

---

## Chunk 1: Define the Structured Provider Contract

### Task 1: Inspect and align existing model/message/tool structures

- [ ] Confirm the exact fields in `aegis_model_request_t`, `aegis_model_response_t`, and stream events.
- [ ] Confirm how tool definitions expose parameter schemas and how message tool results are represented.
- [ ] Document the minimum OpenAI subset: system/user/assistant/tool roles, text content, tool calls, tool results, usage, finish reason.

### Task 2: Write failing contract tests

- [ ] Add tests that construct a structured request containing system text, user text, one tool definition, and a tool result.
- [ ] Assert the provider emits valid OpenAI-compatible JSON with escaped strings and tool schemas.
- [ ] Assert a fixture stream produces text delta and complete tool-call events.
- [ ] Run the focused tests and verify they fail because the structured adapter is absent/incomplete.

### Task 3: Implement the adapter contract

- [ ] Add provider-backed model client configuration while retaining the current mock constructor behavior for existing tests.
- [ ] Add an OpenAI structured adapter factory/context that owns endpoint/model configuration and borrows registry/tool/message data only for the call duration.
- [ ] Make all allocation and callback failure paths return explicit Aegis errors and leave output zeroed.
- [ ] Run the focused contract tests.

---

## Chunk 2: Implement OpenAI-Compatible Request Serialization

### Task 4: Write serializer edge-case tests

- [ ] Test quotes, backslashes, newlines, tabs, control bytes, UTF-8 bytes, empty content, NULL optional content, and long content.
- [ ] Test multiple messages and multiple tool definitions.
- [ ] Test that model IDs and URLs cannot silently truncate; return an error when configured limits are exceeded.

### Task 5: Implement bounded serialization

- [ ] Build a growable JSON buffer using checked size arithmetic.
- [ ] Serialize messages by role, preserving assistant tool calls and tool result call IDs.
- [ ] Serialize tool schemas from the existing Aegis registry without adding transport logic to the generic tool ABI.
- [ ] Set `stream: true` for streaming calls and include generation parameters only when valid.
- [ ] Return `AEGIS_ERR_NOMEM` or `AEGIS_ERR_INVALID` on every failure path.

### Task 6: Verify serialization

- [ ] Run the serializer tests under the normal build.
- [ ] Run the same tests under ASan/UBSan if available.
- [ ] Inspect generated payload fixtures to ensure secrets are not logged.

---

## Chunk 3: Implement Robust SSE Streaming

### Task 7: Write failing SSE tests

- [ ] Feed fragmented chunks where SSE lines and JSON objects split across curl callbacks.
- [ ] Test `data: {...}` records, blank-line delimiters, `[DONE]`, CRLF, comments, malformed JSON, error events, and oversized records.
- [ ] Test text delta, reasoning delta, tool-call start/delta/end, finish reason, and usage extraction.
- [ ] Test callback cancellation stops parsing and returns `AEGIS_ERR_CANCELLED`.

### Task 8: Implement the curl write callback and event parser

- [ ] Add a per-request SSE buffer owned by the call, not global state.
- [ ] Configure `CURLOPT_WRITEFUNCTION` and `CURLOPT_WRITEDATA` for the structured stream path.
- [ ] Parse complete SSE records only; retain incomplete bytes for the next callback.
- [ ] Translate provider records into `aegis_model_stream_event_t` and invoke the caller callback with borrowed event data valid only during the callback.
- [ ] Set connect timeout, total timeout, low-speed timeout, TLS verification, and bounded response sizes.
- [ ] Map HTTP 4xx/5xx and curl errors to structured Aegis statuses without printing API keys or full sensitive payloads.

### Task 9: Verify streaming

- [ ] Run provider tests against the local fixture.
- [ ] Confirm `[DONE]` produces a single end event and no spurious error.
- [ ] Confirm callback failure aborts the curl request and cleans all buffers.

---

## Chunk 4: Complete Tool-Calling in the Agent Loop

### Task 10: Write failing loop tests

- [ ] Configure a deterministic model fixture that first emits a `read` tool call and then emits a final text response after receiving its tool result.
- [ ] Assert the session contains user, assistant tool-call, tool result with matching call ID, and final assistant messages in order.
- [ ] Assert the real registered tool executes exactly once and the old mock-result string is never inserted.
- [ ] Test unknown tool, malformed arguments, schema mismatch, tool failure, cancellation, and maximum-turn exhaustion.

### Task 11: Implement tool-call assembly

- [ ] Extend `stream_accum_t` to track call index, call ID, tool name, and incrementally assembled JSON arguments.
- [ ] Handle start/delta/end events and clone finalized calls into the assistant message.
- [ ] Reject missing IDs/names or excessively large arguments with explicit errors.
- [ ] Ensure every partial allocation is freed on stream failure, cancellation, and callback failure.

### Task 12: Convert JSON arguments into `aegis_tool_args_t`

- [ ] Implement a bounded object parser for the supported tool value types: bool, integer, float, string, and bytes representation if required.
- [ ] Reject duplicate keys, unknown JSON types, trailing garbage, excessively deep/nested input, and oversized strings.
- [ ] Validate parsed arguments through the existing tool schema before execution.

### Task 13: Execute tools and append results

- [ ] Replace the mock result block in `src/agent/loop.c` with `aegis_tool_execute()` or `aegis_tool_submit()` according to the loop's synchronous contract.
- [ ] Apply the existing security policy before execution; do not silently create an allow-all policy in the coding path.
- [ ] Serialize successful and failed tool results as tool messages with the original call ID.
- [ ] Continue the model loop after tool results, with a configurable hard maximum number of model turns.
- [ ] Preserve the final assistant response and set terminal loop state consistently.

### Task 14: Verify the complete loop

- [ ] Run the focused E2E test with the local SSE fixture.
- [ ] Run all existing unit/system tests.
- [ ] Run ASan and TSan builds for loop/tool concurrency and cleanup paths.

---

## Chunk 5: Wire the Coding Agent and CLI

### Task 15: Add provider selection without breaking legacy autonomous mode

- [ ] Extend coding-agent configuration to carry provider name, model, API key, and base URL without exposing secrets in logs.
- [ ] Make interactive and print modes use the same configured model backend.
- [ ] Keep `aegis run --provider ...` legacy autonomous behavior unchanged.
- [ ] Return a clear error when a non-mock model is requested but the required provider is not compiled in or credentials are absent.

### Task 16: Fix output and lifecycle behavior

- [ ] Ensure streaming text is surfaced through the CLI callback or accumulated and printed exactly once, not duplicated.
- [ ] Ensure model, session, tool registry, provider context, and SSE buffers have an unambiguous destruction order.
- [ ] Ensure cancellation reaches both curl and tool execution.

### Task 17: Verify CLI behavior

- [ ] Add/update integration coverage for mock mode.
- [ ] Add a local-fixture invocation test for structured mode if the test harness supports injecting the endpoint.
- [ ] Verify stdout contains machine-readable output only in JSON mode and diagnostics remain on stderr.

---

## Chunk 6: Security and Regression Hardening

### Task 18: Remove unsafe assumptions exposed by the real loop

- [ ] Ensure coding tools receive per-agent context instead of the global `g_mq` mutation queue.
- [ ] Remove `system("mkdir -p ...")` from any path reachable by model tool calls; use checked `mkdir()` operations.
- [ ] Enforce project-root path containment for read/write/edit.
- [ ] Use process groups for bash cancellation and ensure all descriptors are drained/closed in valid order.
- [ ] Avoid logging API keys, authorization headers, or complete provider payloads.

### Task 19: Add regression tests

- [ ] Test path traversal and symlink escape rejection.
- [ ] Test cancellation during network streaming and during bash execution.
- [ ] Test provider unregister/destroy cannot race an in-flight dispatch; if the current registry contract cannot guarantee this, document and fix lifetime pinning before enabling concurrent use.
- [ ] Run the complete CTest suite and sanitizer variants.

---

## Acceptance Criteria

- [ ] A local OpenAI-compatible SSE fixture can drive the coding loop through one real tool call and one final answer.
- [ ] The registered `read` tool executes with parsed arguments; no mock tool result is generated.
- [ ] Assistant/tool message ordering and tool-call IDs survive session save/load.
- [ ] Streaming handles fragmented SSE records, `[DONE]`, malformed input, callback errors, cancellation, and HTTP errors.
- [ ] Existing autonomous tests and legacy provider tests continue to pass.
- [ ] No API key or authorization header is written to logs or test output.
- [ ] Normal, ASan, and TSan verification results are recorded before declaring completion.
