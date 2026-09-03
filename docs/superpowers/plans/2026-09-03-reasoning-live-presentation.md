# Reasoning Live Presentation Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Wire model reasoning (thinking) end-to-end: OpenAI provider parses `reasoning_content`/`reasoning` SSE fields → agent loop accumulates & forwards `AEGIS_AGENT_EVENT_REASONING_DELTA` → CLI streams it dim-italic → session JSONL persists it.

**Architecture:** Thin vertical slice through existing layers. The stream event type `AEGIS_MODEL_STREAM_REASONING_DELTA` already exists (stream.h:20) but has no producer/consumer; message storage (`aegis_message_set_reasoning`) exists but is never written. Mock model emits reasoning so all test harnesses exercise the path.

**Tech Stack:** C11/POSIX, CMake+CTest, no new dependencies.

**Spec:** `docs/superpowers/specs/2026-09-03-reasoning-live-presentation-design.md`

---

## Chunk 1: Agent loop event forwarding

### Task 1: Failing test — REASONING_DELTA forwarded + attached to message

**Files:**
- Modify: `tests/unit/test_agent_events.c:66-103` (fixture backend), `:139-147` (assertions)

- [ ] **Step 1: Extend fixture backend turn-1 (text turn) to emit reasoning before text**

In `model_backend_stream`, else-branch (turn >= 1), before the TEXT_DELTA event:

```c
    } else {
        const char*                r     = "thinking hard";
        aegis_model_stream_event_t rev   = {
            .type = AEGIS_MODEL_STREAM_REASONING_DELTA, .data = r, .len = strlen(r),
        };
        assert(callback(&rev, callback_user) == AEGIS_OK);
        const char*                text  = "done";
        aegis_model_stream_event_t event = {
            .type = AEGIS_MODEL_STREAM_TEXT_DELTA, .data = text, .len = strlen(text),
        };
        assert(callback(&event, callback_user) == AEGIS_OK);
    }
```

- [ ] **Step 2: Update event-sequence assertions**

```c
    /* Event sequence: TOOL_START -> TOOL_END -> REASONING_DELTA -> TEXT_DELTA */
    assert(log.n == 4);
    ...
    assert(log.items[2].type == AEGIS_AGENT_EVENT_REASONING_DELTA);
    assert(strcmp(log.items[2].text, "thinking hard") == 0);
    assert(log.items[3].type == AEGIS_AGENT_EVENT_TEXT_DELTA);
    assert(strcmp(log.items[3].text, "done") == 0);
```

(The `ev_log_t` already stores `text` via `data/len` copy — verify field name when editing; it captures TEXT_DELTA text today, same path works for reasoning.)

- [ ] **Step 3: Assert reasoning attached to final session message**

After the sequence asserts:

```c
    const aegis_message_t* last_msg =
        aegis_session_message_at(session, aegis_session_message_count(session) - 1);
    assert(aegis_message_reasoning(last_msg) != NULL);
    assert(strcmp(aegis_message_reasoning(last_msg), "thinking hard") == 0);
```

- [ ] **Step 4: Build + run to verify failure**

Run: `cmake --build build -j 2>&1 | grep -E "error|warning" | head; ctest --test-dir build -R unit_agent_events --output-on-failure 2>&1 | tail -8`
Expected: compile error — `AEGIS_AGENT_EVENT_REASONING_DELTA` undeclared.

### Task 2: Implement loop forwarding + accumulation

**Files:**
- Modify: `include/aegis/agent/loop.h:30-34` (enum), `src/agent/loop.c:172-190` (accum struct), `:449-494` (stream_cb), `:560-570` (assembly), `:190-200` (destroy)

- [ ] **Step 1: Add enum value** in `loop.h` after `AEGIS_AGENT_EVENT_TOOL_END`:

```c
    AEGIS_AGENT_EVENT_REASONING_DELTA = 3, /**< data/len: borrowed reasoning fragment */
```

- [ ] **Step 2: Add reasoning buffer to `stream_accum_t`** (`char* reasoning; size_t rlen; size_t rcap;`) and free it in `stream_accum_destroy`.

- [ ] **Step 3: Factor a shared append helper and use it for text and reasoning** (above `stream_cb`):

```c
static int accum_append(char** buf, size_t* len, size_t* cap, const char* data, size_t n)
{
    if (*len + n + 1 > *cap) {
        size_t ncap = *cap ? *cap * 2 : 256;
        while (ncap < *len + n + 1) ncap *= 2;
        char* nb = realloc(*buf, ncap);
        if (!nb) return 0;
        *buf = nb; *cap = ncap;
    }
    memcpy(*buf + *len, data, n);
    *len += n;
    (*buf)[*len] = '\0';
    return 1;
}
```

Refactor the existing TEXT_DELTA accumulation block in `stream_cb` to call it (behavior identical), then add the REASONING case:

```c
    if (ev->type == AEGIS_MODEL_STREAM_REASONING_DELTA && ev->data && ev->len) {
        if (!accum_append(&acc->reasoning, &acc->rlen, &acc->rcap, ev->data, ev->len)) {
            return AEGIS_ERR_NOMEM;
        }
        if (acc->loop && acc->loop->on_event) {
            aegis_agent_event_t out_ev = {
                .type = AEGIS_AGENT_EVENT_REASONING_DELTA, .data = ev->data, .len = ev->len,
            };
            acc->loop->on_event(&out_ev, acc->loop->event_user);
        }
    }
```

- [ ] **Step 4: Attach reasoning at assembly** — right after the `aegis_message_set_content(am, ...)` success block (~line 566), before tool-call attachment:

```c
        if (acc.reasoning && aegis_message_set_reasoning(am, acc.reasoning) != AEGIS_OK) {
            aegis_message_destroy(am);
            stream_accum_destroy(&acc);
            set_state(l, AEGIS_AGENT_LOOP_FAILED);
            return AEGIS_ERR_NOMEM;
        }
```

- [ ] **Step 5: Build + run to verify pass**

Run: `cmake --build build -j 2>&1 | grep -E "error|warning" | head; ctest --test-dir build -R unit_agent_events --output-on-failure 2>&1 | tail -3`
Expected: PASS.

- [ ] **Step 6: Commit Chunk 1**

```bash
git add include/aegis/agent/loop.h src/agent/loop.c tests/unit/test_agent_events.c
git commit -m "feat: forward reasoning deltas as agent events and store on message"
```

---

## Chunk 2: OpenAI provider SSE parsing

### Task 3: Failing test — provider emits REASONING_DELTA for both field names

**Files:**
- Modify: `tests/system/test_openai_sse_e2e.c` (fixture body switch, events collector, main)

- [ ] **Step 1: Extend `fixture_t` with `int reasoning_field;` (0=none, 1=reasoning_content, 2=reasoning) and extend the body builder:**

```c
    } else if (fixture->reasoning_field == 1) {
        body = "data: {\"choices\":[{\"delta\":{\"reasoning_content\":\"thin\"}}]}\n\n"
               "data: {\"choices\":[{\"delta\":{\"reasoning_content\":\"king\"}}]}\n\n"
               "data: {\"choices\":[{\"delta\":{\"content\":\"Hi\"}}]}\n\n"
               "data: [DONE]\n\n";
    } else if (fixture->reasoning_field == 2) {
        body = "data: {\"choices\":[{\"delta\":{\"reasoning\":\"why\"}}]}\n\n"
               "data: {\"choices\":[{\"delta\":{\"content\":\"Hi\"}}]}\n\n"
               "data: [DONE]\n\n";
    }
```

- [ ] **Step 2: Extend `events_t` + `collect_event`** with `char reasoning[128]; size_t reasoning_len; int reasoning_deltas;` handling `AEGIS_MODEL_STREAM_REASONING_DELTA` (same memcpy pattern as text).

- [ ] **Step 3: Two scenarios in `main()`** (clone the multi-chunk fixture pattern): assert `reasoning_deltas == 2 && strcmp(reasoning, "thinking") == 0` for field 1, `reasoning_deltas == 1 && strcmp(reasoning, "why") == 0` for field 2, and text still arrives.

- [ ] **Step 4: Verify failure**

Run: `cmake --build build -j 2>&1 | grep error | head; ctest --test-dir build -R system_openai_sse_e2e --output-on-failure 2>&1 | tail -6`
Expected: assertion failure (`reasoning_deltas == 0`).

### Task 4: Implement provider parsing

**Files:**
- Modify: `providers/llm/openai/structured_openai.c` (`emit_record`, after the content block ~line 274)

- [ ] **Step 1: Add reasoning detection after the content-delta block.** Check the long key first so `"reasoning"` never matches inside `"reasoning_content"`:

```c
    const char* rkey = strstr(json, "\"reasoning_content\"");
    if (!rkey) rkey = strstr(json, "\"reasoning\"");
    if (rkey) {
        const char* rvalue = json_string_after(json, rkey == strstr(json, "\"reasoning_content\"")
                                                       ? "\"reasoning_content\""
                                                       : "\"reasoning\"");
```

Simpler correct form (avoid double strstr confusion):

```c
    const char* rcontent = json_string_after(json, "\"reasoning_content\"");
    if (!rcontent) rcontent = json_string_after(json, "\"reasoning\"");
```

Caveat: `json_string_after(json, "\"reasoning\"")` could still match the prefix inside `"reasoning_content"` **only if** the long key exists but `json_string_after` on it failed (e.g. value is `null`). That would then try to read `"reasoning_content"`'s value as the short key — same position, same failure (returns NULL). Verify this reasoning in code and add a comment.

- [ ] **Step 2: Emit the event** (mirror the content block):

```c
    if (rcontent) {
        char* decoded = malloc(len + 1);
        if (!decoded) { free(json); return 0; }
        size_t n = copy_json_string(rcontent, decoded, len + 1);
        aegis_model_stream_event_t ev = {.type = AEGIS_MODEL_STREAM_REASONING_DELTA, .data = decoded, .len = n};
        aegis_status_t rc = s->callback(&ev, s->callback_user);
        free(decoded);
        if (rc != AEGIS_OK) { free(json); return 0; }
    }
```

Note: emit reasoning **before** text within the same record if both appear — ordering matches token flow.

- [ ] **Step 3: Verify pass + full suite**

Run: `cmake --build build -j 2>&1 | grep error | head; ctest --test-dir build 2>&1 | tail -2`
Expected: 100% pass.

- [ ] **Step 4: Commit Chunk 2**

```bash
git add providers/llm/openai/structured_openai.c tests/system/test_openai_sse_e2e.c
git commit -m "feat: parse reasoning_content/reasoning SSE fields into reasoning deltas"
```

---

## Chunk 3: Mock model + CLI presentation

### Task 5: Mock model emits reasoning

**Files:**
- Modify: `src/model/model.c:185-210` (mock stream)

- [ ] **Step 1: Before the text-chunk loop**, emit two reasoning chunks (cancellation-checked like the text loop):

```c
    static const char* rparts[] = {"thinking ", "about it..."};
    for (size_t i = 0; i < 2; i++) {
        if (token && aegis_cancellation_token_is_cancelled(token)) return AEGIS_ERR_CANCELLED;
        aegis_model_stream_event_t ev = {.type = AEGIS_MODEL_STREAM_REASONING_DELTA,
                                         .data = rparts[i], .len = strlen(rparts[i])};
        aegis_status_t st = cb(&ev, user);
        if (st != AEGIS_OK) return st;
    }
```

- [ ] **Step 2: Run affected suites** (mock is used by many tests): `ctest --test-dir build 2>&1 | tail -2`. If a test asserts exact byte streams from mock, update it to tolerate/expect reasoning chunks (investigate before changing — most use `assert_contains`).

### Task 6: CLI dim-italic presentation

**Files:**
- Modify: `apps/aegis/cli_interactive.c:24-28` (ctx struct), `:44-92` (cli_event_cb)

- [ ] **Step 1: Add `bool reasoning_open;`** to `cli_stream_ctx_t`.

- [ ] **Step 2: In `cli_event_cb`** add the case and block-closing logic:

```c
    case AEGIS_AGENT_EVENT_REASONING_DELTA:
        if (ev->data && ev->len) {
            cli_stream_prelude(cx);
            if (!cx->reasoning_open) {
                fputs("\033[2m\033[3m", stdout);
                cx->reasoning_open = true;
            }
            fwrite(ev->data, 1, ev->len, stdout);
            fflush(stdout);
        }
        break;
```

In `AEGIS_AGENT_EVENT_TEXT_DELTA` and `AEGIS_AGENT_EVENT_TOOL_START` cases, **before** existing logic:

```c
        if (cx->reasoning_open) {
            fputs("\033[0m\n", stdout);
            cx->reasoning_open = false;
            cx->line_open      = false;
        }
```

Reasoning must NOT set `text_emitted` (dedup untouched).

- [ ] **Step 3: Extend `tests/integration/test_cli.c`** streaming case: assert output contains `"\033[2m"` (reasoning styled) and final message still printed exactly once. Mock now emits reasoning automatically.

- [ ] **Step 4: Verify + commit Chunk 3**

Run: `cmake --build build -j 2>&1 | grep error | head; ctest --test-dir build 2>&1 | tail -2`

```bash
git add src/model/model.c apps/aegis/cli_interactive.c tests/integration/test_cli.c
git commit -m "feat: mock emits reasoning; CLI streams it dim-italic"
```

---

## Chunk 4: Session JSONL persistence

### Task 7: Failing test — reasoning round-trips through save/load

**Files:**
- Modify: `tests/unit/test_session.c` (add a roundtrip case near existing message roundtrip test)

- [ ] **Step 1: Add test**: create session + assistant message with content + `aegis_message_set_reasoning(m, "deep thoughts")`, save, load into a second session, assert loaded message has identical reasoning. Also assert a session saved **without** reasoning loads with NULL reasoning (backward compat).

- [ ] **Step 2: Verify failure**: `ctest --test-dir build -R unit_session --output-on-failure 2>&1 | tail -6`

### Task 8: Implement JSONL save/load of reasoning

**Files:**
- Modify: `src/session/session.c:257-261` (save), `:426-470` (load)

- [ ] **Step 1: Save** — after `json_escape(f, content)`:

```c
        const char* reasoning = aegis_message_reasoning(m);
        if (reasoning) {
            fputs("\",\"reasoning\":\"", f);
            json_escape(f, reasoning);
        }
        fputs("\"}\n", f);
```

(replacing the current unconditional `fputs("\"}\n", f);`)

- [ ] **Step 2: Load** — mirror the content parser: extract `"reasoning":"` value with the same unescape loop, then `aegis_message_set_reasoning(m, reasoning)` after message creation (only when the key was present). Read lines 440-470 first and reuse the exact escape-handling pattern used for content.

- [ ] **Step 3: Verify + full suite**

Run: `cmake --build build -j 2>&1 | grep error | head; ctest --test-dir build 2>&1 | tail -2`

- [ ] **Step 4: Commit Chunk 4**

```bash
git add src/session/session.c tests/unit/test_session.c
git commit -m "feat: persist message reasoning in session JSONL"
```

---

## Chunk 5: Full acceptance

### Task 9: Verification + docs

- [ ] **Step 1: Full ctest (normal build)**: `ctest --test-dir build 2>&1 | tail -2` — expect 100%.
- [ ] **Step 2: ASan build + ctest**: `cmake -S . -B build-asan > /dev/null 2>&1 && cmake --build build-asan -j 2>&1 | grep error | head; ctest --test-dir build-asan 2>&1 | grep -E "passed|Failed"` — expect 100%, zero leaks.
- [ ] **Step 3: Live smoke** (optional but recommended): rerun `/tmp/aegis_live/runtest3.sh` against the mock SSE server — verify dim reasoning block appears before text, `/stream off` suppresses it, session JSONL contains `"reasoning":"..."`.
- [ ] **Step 4: Commit any stragglers** (e.g. integration test tweaks discovered in Task 9).
