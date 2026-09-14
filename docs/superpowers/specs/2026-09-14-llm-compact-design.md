# LLM-Summarization Compact Design

**Goal:** Replace pure-truncation context compaction with an LLM-summarized
compaction that preserves the semantic content of dropped messages, while
falling back to truncation when the model is unavailable or fails.

**Architecture:** A new `aegis_session_compact_with_summary()` API mirrors the
existing `aegis_session_compact()` but, when a model client is supplied,
summarizes the dropped (older) messages into a single assistant message that
is prepended to the retained list. Callers (coding-agent auto-compact, CLI
`/compact`) use it when a model client is present; otherwise they fall back to
the pure-truncation path.

**Tech Stack:** C, existing `aegis_model_client_t` complete path, existing
`aegis_session_t` message-list API.

---

## 1. Background

`aegis_session_compact(session, keep_messages)` in `src/session/session.c`
(≈line 250) is pure truncation: it keeps the newest `keep_messages`, adjusts
the start boundary to preserve tool-pair invariants (a leading TOOL message
pulls back to its owning assistant-with-tool_calls; an assistant with
tool_calls pulls forward the whole tool-result run), then replaces
`s->messages` with the retained sub-list. All older messages are dropped
with no trace.

Two callers use it:

- `src/coding/coding_agent.c` auto-compact after
  `aegis_agent_loop_context_dropped(loop) > 0` — passes
  `AEGIS_LOOP_CONTEXT_WINDOW` (newest N messages).
- `apps/aegis/cli_interactive.c` `/compact` command — passes `32`.

The gap (vs. pi-agent): dropped messages are summarizable context that the
LLM could condense into a short summary, preserving decisions, findings, and
file paths. Today that context is lost.

## 2. API

New function in `src/session/session.c` + declared in
`include/aegis/session/session.h` (which gains includes for
`aegis/model/model.h` and `aegis/common/cancellation/cancellation.h` for the
`aegis_model_client_t` / `aegis_cancellation_token_t` parameter types):

```c
/**
 * Compact the session, summarizing dropped messages via the model client.
 *
 * Same tool-pair-preserving truncation as aegis_session_compact(), plus:
 * when @p model is non-NULL and @p token is not cancelled, the dropped
 * messages are condensed into a single assistant message (no tool_calls)
 * prepended to the retained list. On model failure the function still
 * truncates (returning the retained list) and sets *out_summary = NULL.
 *
 * @param[in]  s        Session to compact.
 * @param[in]  keep     Number of newest messages to retain.
 * @param[in]  model    Model client for summarization, or NULL to skip.
 * @param[in]  token    Cancellation token, or NULL.
 * @param[out] out_summary Receives a pointer to the summary message (borrowed
 *             from the session's list; do NOT free), or NULL when no
 *             summary was produced (model NULL, call failed, or no dropped
 *             messages). Untouched on alloc failure.
 * @return AEGIS_OK on success (including fallback truncation),
 *   AEGIS_ERR_INVALID on NULL s, AEGIS_ERR_NOMEM on hard allocation failure.
 */
aegis_status_t aegis_session_compact_with_summary(
    aegis_session_t* s, size_t keep, aegis_model_client_t* model,
    const aegis_cancellation_token_t* token, aegis_message_t** out_summary);
```

Design decisions (locked):

1. **Summary role = ASSISTANT.** The summary is one plain assistant message
   (no tool_calls) so it is safe to prepend at the head of the retained list
   without violating tool-pair invariants.
2. **Model source = caller's client.** The function takes an
   `aegis_model_client_t*`; callers pass their existing client (coding agent
   uses `a->model`, CLI passes the session's model). No separate summarizer
   client.
3. **Failure = degrade to truncation.** If `model == NULL`, the call fails,
   or the token is pre-cancelled, the function still performs the truncation
   (retained list is set on the session) and returns `AEGIS_OK` with
   `*out_summary = NULL`. Only a hard NOMEM in building the retained list
   returns `AEGIS_ERR_NOMEM`.

## 3. Behavior

1. Compute `start`/`end` exactly as `aegis_session_compact` does
   (tool-pair-preserving truncation). Dropped = `[0, start)`, retained =
   `[start, end)`.
2. Build the retained sub-list (same as today).
3. If `model != NULL` and `start > 0` and `!token-cancelled`:
   - Assemble a one-shot `aegis_model_request_t`:
     - `messages` = [ system "Summarize the following conversation concisely,
       preserving decisions, findings, file paths, and next steps",
       then each dropped message in order ]
      - `stream = false`, `max_tokens = 0` (provider default), `model = NULL`
        (the client's own name, captured at creation, is used).
   - Call `aegis_model_complete(model, &req, token, &resp)`.
   - On success: take `resp->message`'s content, create a new
     `AEGIS_MESSAGE_ASSISTANT` message with that content, prepend it to the
     retained list. Set `*out_summary` to that message (borrowed). Destroy
     `resp`.
   - On any failure (non-OK, NOMEM, cancelled): leave `*out_summary = NULL`;
     the retained list is used as-is (pure truncation fallback).
4. Set `s->messages = retained`, `s->updated_at = now_ms()`.
5. Return status.

Notes:

- The summary prompt is fixed (English, concise). No token budget knob in
  this iteration (YAGNI) — `max_tokens` left at provider default.
- Dropped messages include tool results / tool calls; the LLM receives them
  as-is in the request's message list, so tool-pair content is summarized,
  not silently dropped.
- Because the summary is a plain assistant message with no tool_calls, it does
  not break the tool-pair invariant when prepended before a retained leading
  TOOL message.

## 4. Callers

- `src/coding/coding_agent.c` auto-compact: call
  `aegis_session_compact_with_summary(a->session, AEGIS_LOOP_CONTEXT_WINDOW,
  a->model, a->token, &summary)` instead of the plain compact. `a->token`
  exists per turn (created in `run()`).
- `apps/aegis/cli_interactive.c` `/compact`: call the new API with the
  session's model client and `32`. If the CLI has no model client, pass
  NULL → pure truncation (current behavior).

## 5. Testing

- `tests/unit/test_session_compact.c` (extend or new):
  - No model (NULL) + dropped messages → truncation happens, `out_summary`
    NULL, retained count == keep.
  - Mock model returns a canned summary → retained list has one extra
    assistant message at head with the summary text; `out_summary` points to
    it.
  - Mock model fails (e.g. canned ERROR) → truncation still happens,
    `out_summary` NULL, return OK.
  - Pre-cancelled token → truncation, no summary call, `out_summary` NULL.
  - keep >= count → no-op, unchanged.
- Existing `unit_session` tests must still pass (plain compact unchanged).

## 6. Scope OUT

- Token-budget knob for the summary (provider default only).
- Streaming the summary (one-shot complete only).
- A separate summarizer model client (reuse caller's client).
- MCP, sub-agents, skill progressive-disclosure (other pi-agent gaps).
