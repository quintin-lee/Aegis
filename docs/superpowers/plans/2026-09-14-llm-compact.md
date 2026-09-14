# LLM-Summarization Compact Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add `aegis_session_compact_with_summary` so session compaction can replace dropped (older) messages with a single LLM-generated summary message instead of pure truncation, degrading to truncation when no model is available or the call fails.

**Architecture:** New API mirrors `aegis_session_compact`'s tool-pair truncation exactly, then optionally calls `aegis_model_complete` with a "summarize these messages" request built from the dropped prefix. On success the summary is prepended as one assistant message via `aegis_message_list_prepend`; on any failure the truncated list is kept as-is and `*out_summary` stays NULL. Two callers (coding agent auto-compact, CLI `/compact`) switch to the new API.

**Tech Stack:** C, existing `aegis_model` / `aegis_session` / `aegis_message` / `aegis_cancellation` modules. No new dependencies.

---

## Spec

`docs/superpowers/specs/2026-09-14-llm-compact-design.md` (committed c02fda5)

## File Map

| File | Responsibility |
|---|---|
| `include/aegis/session/session.h` | Add includes (`aegis/model/model.h`, `aegis/common/cancellation/cancellation.h`) + declare `aegis_session_compact_with_summary` |
| `src/session/session.c` | Implement `aegis_session_compact_with_summary` after `aegis_session_compact` (~line 307) |
| `src/coding/coding_agent.c` | Switch auto-compact call (~line 431) to new API |
| `apps/aegis/cli_interactive.c` | Switch `/compact` call (~line 244) to new API |
| `tests/unit/test_session.c` | Add 5 with_summary test cases |
| `cmake/AegisTests.cmake` | Add `aegis_model` to `unit_session` link line (~line 158) |

**Constraints:**
- Comment-only additions to `session.c` / `session.h` follow the established Doxygen style (`/** @brief ... @param ... @return */`, English).
- No behavior change to plain `aegis_session_compact` — its tests must stay green.
- Do NOT use subagents (user standing instruction); execute directly via executing-plans.
- Verify each chunk with `cmake --build build --parallel 8` + `ctest` + `clang-format --dry-run` on changed files.

---

## Chunk 1: TDD — failing tests for `aegis_session_compact_with_summary`

**Files:**
- Modify: `tests/unit/test_session.c`
- Modify: `cmake/AegisTests.cmake:157-160` (add `aegis_model` to `unit_session` link)

- [ ] **Step 1: Add `aegis_model` to the `unit_session` test target link line.**

Edit `cmake/AegisTests.cmake` line ~158:

```cmake
target_link_libraries(unit_session PRIVATE aegis_session GTest::gtest_main)
```

becomes:

```cmake
target_link_libraries(unit_session PRIVATE aegis_session aegis_model GTest::gtest_main)
```

Reason: the new tests call `aegis_model_client_create` and `aegis_session_compact_with_summary` (which needs `aegis_model` symbols).

- [ ] **Step 2: Write the 5 failing test cases in `tests/unit/test_session.c`.**

The test file `tests/unit/test_session.c` is **plain C** (no GTest) — it uses `static void` test functions, `assert()`, a local `expect_ok()` helper, and `printf` PASS markers, all driven from `main()`. Add the new tests in that exact style.

**First, add the new includes** at the top of `tests/unit/test_session.c` (after the existing includes at lines 6-12):

```c
#include "aegis/model/model.h"
#include "aegis/common/cancellation/cancellation.h"
```

**Then append the test functions** before `int main(void)`:

```c
/* ── aegis_session_compact_with_summary tests (plain C, no GTest) ──── */

static void add_user_msgs(aegis_session_t* s, size_t total)
{
    for (size_t i = 0; i < total; ++i) {
        aegis_message_t* m = NULL;
        expect_ok(aegis_message_create(AEGIS_MESSAGE_USER, &m), "msg");
        char content[32];
        snprintf(content, sizeof(content), "msg %zu", i);
        expect_ok(aegis_message_set_content(m, content), "content");
        expect_ok(aegis_session_append_message(s, m), "append");
        aegis_message_destroy(m);
    }
}

/* (i) NULL model + dropped msgs -> pure truncation, no summary. */
static void test_compact_summary_null_model(void)
{
    aegis_session_t* s = NULL;
    expect_ok(aegis_session_create("/tmp/sum-null", &s), "create");
    add_user_msgs(s, 10);
    aegis_message_t* summary = NULL;
    expect_ok(aegis_session_compact_with_summary(s, 3, NULL, NULL, &summary), "compact");
    assert(summary == NULL);
    assert(aegis_session_message_count(s) == 3);
    aegis_session_destroy(s);
    printf("compact_summary_null_model PASS\n");
}

/* (ii) mock model -> summary prepended as head; count = kept + 1. */
static void test_compact_summary_mock(void)
{
    aegis_session_t* s = NULL;
    expect_ok(aegis_session_create("/tmp/sum-mock", &s), "create");
    add_user_msgs(s, 5);
    aegis_model_client_t* model = NULL;
    expect_ok(aegis_model_client_create("mock", &model), "model");
    aegis_message_t* summary = NULL;
    expect_ok(aegis_session_compact_with_summary(s, 2, model, NULL, &summary), "compact");
    assert(summary != NULL);
    assert(aegis_session_message_count(s) == 3); /* 2 kept + 1 summary */
    const aegis_message_t* head = aegis_session_message_at(s, 0);
    assert(head);
    assert(aegis_message_role(head) == AEGIS_MESSAGE_ASSISTANT);
    /* Mock echoes "mock response to: <last USER content in the request>".
     * The summarization request's messages = [system, dropped...]; the LAST
     * USER message in that list is the newest dropped msg ("msg 2"). */
    assert(strcmp(aegis_message_content(head), "mock response to: msg 2") == 0);
    aegis_model_client_destroy(model);
    aegis_session_destroy(s);
    printf("compact_summary_mock PASS\n");
}

/* (iii) model backend that always fails -> truncation still happens, no summary. */
static aegis_status_t failing_complete(void* user, const aegis_model_request_t* req,
                                       const aegis_cancellation_token_t* token,
                                       aegis_model_response_t** out)
{
    (void)user;
    (void)req;
    (void)token;
    *out = NULL;
    return AEGIS_ERR_PROVIDER;
}

static void test_compact_summary_model_fails(void)
{
    aegis_session_t* s = NULL;
    expect_ok(aegis_session_create("/tmp/sum-fail", &s), "create");
    add_user_msgs(s, 5);
    aegis_model_backend_t backend = {
        .user         = NULL,
        .complete     = failing_complete,
        .stream       = NULL,
        .capabilities = AEGIS_MODEL_CAP_TEXT,
    };
    aegis_model_client_t* model = NULL;
    expect_ok(aegis_model_client_create_with_backend("mock", &backend, &model), "model");
    aegis_message_t* summary = NULL;
    expect_ok(aegis_session_compact_with_summary(s, 2, model, NULL, &summary), "compact");
    assert(summary == NULL);
    assert(aegis_session_message_count(s) == 2); /* pure truncation */
    aegis_model_client_destroy(model);
    aegis_session_destroy(s);
    printf("compact_summary_model_fails PASS\n");
}

/* (iv) pre-cancelled token -> no model call, truncation, no summary. */
static void test_compact_summary_cancelled(void)
{
    aegis_session_t* s = NULL;
    expect_ok(aegis_session_create("/tmp/sum-cancel", &s), "create");
    add_user_msgs(s, 5);
    aegis_model_client_t* model = NULL;
    expect_ok(aegis_model_client_create("mock", &model), "model");
    aegis_cancellation_token_t* token = NULL;
    expect_ok(aegis_cancellation_token_create(&token), "token");
    aegis_cancellation_token_request_cancel(token);
    aegis_message_t* summary = NULL;
    expect_ok(aegis_session_compact_with_summary(s, 2, model, token, &summary), "compact");
    assert(summary == NULL);
    assert(aegis_session_message_count(s) == 2);
    aegis_cancellation_token_destroy(token);
    aegis_model_client_destroy(model);
    aegis_session_destroy(s);
    printf("compact_summary_cancelled PASS\n");
}

/* (v) keep >= count -> no-op, list unchanged, no summary. */
static void test_compact_summary_noop(void)
{
    aegis_session_t* s = NULL;
    expect_ok(aegis_session_create("/tmp/sum-noop", &s), "create");
    add_user_msgs(s, 3);
    aegis_model_client_t* model = NULL;
    expect_ok(aegis_model_client_create("mock", &model), "model");
    aegis_message_t* summary = NULL;
    expect_ok(aegis_session_compact_with_summary(s, 5, model, NULL, &summary), "compact");
    assert(summary == NULL);
    assert(aegis_session_message_count(s) == 3);
    aegis_model_client_destroy(model);
    aegis_session_destroy(s);
    printf("compact_summary_noop PASS\n");
}
```

**Finally, wire the 5 new tests into `main()`** (after `test_fork();` on line 288):

```c
    test_compact_summary_null_model();
    test_compact_summary_mock();
    test_compact_summary_model_fails();
    test_compact_summary_cancelled();
    test_compact_summary_noop();
```

Notes:
- `aegis_session_message_at` returns `const aegis_message_t*` (per `session.h`); `aegis_message_role`/`aegis_message_content` accept it.
- `AEGIS_MODEL_CAP_TEXT` is from `aegis/model/capability.h` (pulled in via `aegis/model/model.h`).
- `failing_complete` is a file-scope static — define it once before `test_compact_summary_model_fails`.
- The mock summary content assertion in (ii) depends on the mock backend echoing the LAST USER message in the request. The summarization request prepends a SYSTEM message then the dropped USER msgs, so the last USER is the newest dropped msg. If the mock's last-USER search skips the system message, this holds; verify against `src/model/model.c` `aegis_model_complete` behavior.

- [ ] **Step 3: Build the tests — confirm they FAIL (function not declared yet).**

```bash
cmake -S . -B build && cmake --build build --parallel 8
```

Expected: compile error — `aegis_session_compact_with_summary` is not declared in `session.h`. This confirms the test is genuinely exercising the new API.

- [ ] **Step 4: Re-run ctest to confirm the 5 new tests fail at link/compile stage.**

```bash
cd build && ctest -R unit_session --output-on-failure
```

Expected: build failure (undefined reference to `aegis_session_compact_with_summary`).

- [ ] **Step 5: Commit the failing tests + CMake link change.**

```bash
git add tests/unit/test_session.c cmake/AegisTests.cmake
git commit -m "test(session): add failing tests for aegis_session_compact_with_summary"
```

---

## Chunk 2: Implement `aegis_session_compact_with_summary`

**Files:**
- Modify: `include/aegis/session/session.h`
- Modify: `src/session/session.c`

- [ ] **Step 1: Add the needed includes to `include/aegis/session/session.h`.**

At the top of `session.h` (with the existing `#include` lines), add:

```c
#include "aegis/model/model.h"
#include "aegis/common/cancellation/cancellation.h"
```

Place them after the existing `aegis/message/message.h` include. (These provide the `aegis_model_client_t` and `aegis_cancellation_token_t` types used by the new declaration.)

- [ ] **Step 2: Declare `aegis_session_compact_with_summary` in `session.h`.**

Immediately after the existing `aegis_session_compact` declaration (~line 78), add:

```c
/**
 * @brief Compact the session, optionally replacing the dropped prefix with a
 *        single LLM-generated summary message.
 *
 * Mirrors aegis_session_compact() exactly (tool-pair-preserving truncation to
 * the newest @p keep messages). When @p model is non-NULL, @p token is not
 * cancelled, and there are dropped messages, a one-shot aegis_model_complete()
 * call summarizes the dropped prefix; the returned summary is prepended to the
 * retained list as a single assistant message. On any model failure, NULL
 * model, or pre-cancelled token, the function degrades to plain truncation.
 *
 * @param[in]  s          Session (non-NULL).
 * @param[in]  keep       Number of newest messages to retain (0 = drop all).
 * @param[in]  model      Model client used for summarization; NULL disables it.
 * @param[in]  token      Cancellation token; pre-cancelled skips summarization.
 * @param[out] out_summary  Receives the prepended summary message (owned by the
 *                          session's message list) on success, or NULL when no
 *                          summary was generated. Initialized to NULL on entry.
 * @return AEGIS_OK on success (including the truncation fallback),
 *   AEGIS_ERR_INVALID on NULL @p s, AEGIS_ERR_NOMEM on allocation failure.
 */
aegis_status_t aegis_session_compact_with_summary(aegis_session_t* s, size_t keep,
                                                  aegis_model_client_t* model,
                                                  const aegis_cancellation_token_t* token,
                                                  aegis_message_t** out_summary);
```

- [ ] **Step 3: Add the needed includes to `src/session/session.c`.**

After the existing includes (~line 15), add:

```c
#include "aegis/model/model.h"
#include "aegis/model/request.h"
#include "aegis/model/response.h"
#include "aegis/common/cancellation/cancellation.h"
```

- [ ] **Step 4: Implement `aegis_session_compact_with_summary` in `src/session/session.c`.**

Insert immediately after `aegis_session_compact` (which ends ~line 307). The body reuses the exact truncation logic, then adds the summary step:

```c
aegis_status_t aegis_session_compact_with_summary(aegis_session_t* s, size_t keep,
                                                  aegis_model_client_t* model,
                                                  const aegis_cancellation_token_t* token,
                                                  aegis_message_t** out_summary)
{
    if (!s || !out_summary) {
        return AEGIS_ERR_INVALID;
    }
    *out_summary = NULL;
    aegis_message_list_t* messages = s->messages;
    if (!messages) {
        return AEGIS_OK; /* nothing to compact */
    }
    size_t count = aegis_message_list_count(messages);
    if (keep >= count) {
        return AEGIS_OK; /* nothing to drop */
    }

    /* ── Truncation: identical to aegis_session_compact ────────────────── */
    size_t start = count - keep;
    /* If the boundary message is a tool result, pull back to its owning
     * assistant message (matching tool_call_id) so the pair is preserved. */
    aegis_message_t* boundary = aegis_message_list_at(messages, start);
    if (boundary && aegis_message_role(boundary) == AEGIS_MESSAGE_TOOL) {
        const char* call_id = aegis_message_tool_call_id(boundary);
        while (start > 0) {
            --start;
            aegis_message_t* prev = aegis_message_list_at(messages, start);
            if (!prev || aegis_message_role(prev) != AEGIS_MESSAGE_ASSISTANT) {
                ++start;
                break;
            }
            /* Check whether this assistant message owns a tool call with the
             * matching id. (Mirror the exact walk-back in aegis_session_compact.) */
            if (call_id && aegis_message_tool_call_count(prev) > 0) {
                bool match = false;
                for (size_t t = 0; t < aegis_message_tool_call_count(prev); ++t) {
                    const aegis_tool_call_t* tc = aegis_message_tool_call_at(prev, t);
                    if (aegis_tool_call_id(tc) && strcmp(aegis_tool_call_id(tc), call_id) == 0) {
                        match = true;
                        break;
                    }
                }
                if (match) {
                    break;
                }
            }
            ++start; /* revert; keep walking back */
        }
    }
    /* If the boundary assistant message has tool calls, extend forward over
     * the whole tool-result run so the pair is preserved. */
    aegis_message_t* b2 = aegis_message_list_at(messages, start);
    if (b2 && aegis_message_role(b2) == AEGIS_MESSAGE_ASSISTANT &&
        aegis_message_tool_call_count(b2) > 0) {
        size_t end = start + 1;
        while (end < count && aegis_message_list_at(messages, end) &&
               aegis_message_role(aegis_message_list_at(messages, end)) == AEGIS_MESSAGE_TOOL) {
            ++end;
        }
        if (end - start > keep) {
            keep = end - start; /* retain the full tool run even if it exceeds keep */
        }
    }

    /* Build the retained sub-list [start, count). */
    aegis_message_list_t* retained = NULL;
    aegis_status_t st = aegis_message_list_create(&retained);
    if (st != AEGIS_OK) {
        return st;
    }
    for (size_t i = start; i < count; ++i) {
        aegis_message_t* m = aegis_message_list_at(messages, i);
        if (aegis_message_list_append(retained, m) != AEGIS_OK) {
            aegis_message_list_destroy(retained);
            return AEGIS_ERR_NOMEM;
        }
    }

    /* ── Summarization: optional, degrades to truncation ───────────────── */
    bool can_summarize = (model && start > 0) &&
                         !(token && aegis_cancellation_token_is_cancelled(token));
    if (can_summarize) {
        /* Build the request: system prompt + each dropped message in order. */
        aegis_message_list_t* dropped = NULL;
        if (aegis_message_list_create(&dropped) == AEGIS_OK) {
            aegis_message_t* sys = NULL;
            if (aegis_message_create(AEGIS_MESSAGE_SYSTEM, &sys) == AEGIS_OK) {
                aegis_message_set_content(sys,
                    "Summarize the following conversation concisely, "
                    "preserving decisions, findings, file paths, and next steps.");
                aegis_message_list_append(dropped, sys);
                aegis_message_destroy(sys);
            }
            for (size_t i = 0; i < start; ++i) {
                aegis_message_list_append(dropped, aegis_message_list_at(messages, i));
            }
            aegis_model_request_t req = {0};
            req.messages = dropped;
            req.stream   = false;
            /* req.model stays NULL: the client's own model name is used. */
            aegis_model_response_t* resp = NULL;
            aegis_status_t mst = aegis_model_complete(model, &req, token, &resp);
            if (mst == AEGIS_OK && resp && resp->message &&
                aegis_message_content(resp->message)) {
                aegis_message_t* sum = NULL;
                if (aegis_message_create(AEGIS_MESSAGE_ASSISTANT, &sum) == AEGIS_OK) {
                    aegis_message_set_content(sum, aegis_message_content(resp->message));
                    if (aegis_message_list_prepend(retained, sum) == AEGIS_OK) {
                        *out_summary = sum; /* owned by the session's retained list */
                    } else {
                        aegis_message_destroy(sum);
                    }
                }
            }
            /* On any failure above: *out_summary stays NULL; retained (truncated)
             * list is used as-is. */
            aegis_model_response_destroy(resp);
            aegis_message_list_destroy(dropped);
        }
    }

    /* Swap the retained list into the session. */
    aegis_message_list_destroy(s->messages);
    s->messages = retained;
    s->updated_at = now_ms();
    return AEGIS_OK;
}
```

**IMPORTANT — before finalizing this body:** Read the *actual* `aegis_session_compact` implementation at `src/session/session.c:250-307` and copy its EXACT truncation logic (the walk-back / tool-run-extension / boundary matching) verbatim into this function's truncation section, rather than the paraphrase above. The above is a structural sketch; the real walk-back may use different helper names or a different matching strategy. Match the existing code precisely so tool-pair preservation behaves identically.

- [ ] **Step 5: Build — confirm it compiles.**

```bash
cmake --build build --parallel 8
```

Expected: clean build. If the message-list accessor names differ from the sketch (`aegis_message_list_at`, `aegis_message_list_prepend`, `aegis_message_tool_call_count`, `aegis_message_tool_call_at`), fix the calls to match the real API (confirmed to exist in `include/aegis/message/message.h`).

- [ ] **Step 6: Run the 5 new tests + all existing session tests.**

```bash
cd build && ctest -R unit_session --output-on-failure
```

Expected: all 5 new tests PASS, existing plain-compact tests still PASS.

- [ ] **Step 7: Commit the implementation.**

```bash
git add include/aegis/session/session.h src/session/session.c tests/unit/test_session.c cmake/AegisTests.cmake
git commit -m "feat(session): add aegis_session_compact_with_summary (LLM summarization with truncation fallback)"
```

---

## Chunk 3: Switch the two callers

**Files:**
- Modify: `src/coding/coding_agent.c` (~line 431)
- Modify: `apps/aegis/cli_interactive.c` (~line 244)

- [ ] **Step 1: Switch the coding-agent auto-compact call.**

In `src/coding/coding_agent.c`, find:

```c
if (aegis_agent_loop_context_dropped(a->loop) > 0) {
    (void)aegis_session_compact(a->session, AEGIS_LOOP_CONTEXT_WINDOW);
}
```

Replace with:

```c
if (aegis_agent_loop_context_dropped(a->loop) > 0) {
    aegis_message_t* summary = NULL;
    (void)aegis_session_compact_with_summary(a->session, AEGIS_LOOP_CONTEXT_WINDOW,
                                              a->model, a->token, &summary);
}
```

- [ ] **Step 2: Switch the CLI `/compact` command.**

In `apps/aegis/cli_interactive.c` (~line 244), find the `/compact` handler:

```c
st = aegis_session_compact(sess, 32);
```

Replace with the new API. Pass the model client if one is in scope in that handler; otherwise pass `NULL` (degrades to plain truncation = current behavior). Check the surrounding scope for a session model client variable before editing:

```c
aegis_message_t* summary = NULL;
st = aegis_session_compact_with_summary(sess, 32, /*model*/ <model_or_NULL>,
                                        /*token*/ <token_or_NULL>, &summary);
```

If no model client or token is available in that scope, use:

```c
aegis_message_t* summary = NULL;
st = aegis_session_compact_with_summary(sess, 32, NULL, NULL, &summary);
```

- [ ] **Step 3: Build both targets.**

```bash
cmake --build build --parallel 8
```

Expected: clean.

- [ ] **Step 4: Run the coding-agent and CLI-related tests.**

```bash
cd build && ctest --output-on-failure
```

Expected: all tests pass (the coding-agent auto-compact path is exercised by existing loop tests; `/compact` by CLI tests if any).

- [ ] **Step 5: Commit the caller switch.**

```bash
git add src/coding/coding_agent.c apps/aegis/cli_interactive.c
git commit -m "feat(coding,cli): use aegis_session_compact_with_summary for auto-compact and /compact"
```

---

## Chunk 4: Final verification

- [ ] **Step 1: Full build + ctest.**

```bash
cmake --build build --parallel 8 && cd build && ctest --output-on-failure
```

Expected: 100% build, all tests pass.

- [ ] **Step 2: Format-check all changed files.**

```bash
for f in include/aegis/session/session.h src/session/session.c \
         src/coding/coding_agent.c apps/aegis/cli_interactive.c \
         tests/unit/test_session.c cmake/AegisTests.cmake; do
    [ "$f" = "cmake/AegisTests.cmake" ] && continue
    clang-format --dry-run "$f"
done
```

Expected: no `clang-format-violations` warnings. If any, run `clang-format -i` on the offending C files only (never on the `.cmake` file).

- [ ] **Step 3: Audit the diff is comment/logic-consistent (no stray changes).**

```bash
git diff --stat
```

Confirm only the 6 files in the File Map are touched.

- [ ] **Step 4: Final commit (if any cleanup edits landed after Chunk 3).**

```bash
git status --porcelain
# commit any leftover cleanup
```

---

## Verification Criteria (definition of done)

- [ ] `aegis_session_compact_with_summary` implemented; plain `aegis_session_compact` unchanged.
- [ ] All 5 new tests in `test_session.c` PASS.
- [ ] All pre-existing tests PASS (no regression).
- [ ] Both callers (coding agent auto-compact, CLI `/compact`) use the new API.
- [ ] `clang-format --dry-run` clean on all changed C files.
- [ ] Build 100%, ctest 100%.
- [ ] 3 commits: (1) failing tests + cmake link, (2) implementation, (3) caller switch.

## Rollback

Each chunk is an independent commit; revert with `git revert <sha>` if a chunk regresses. Chunk 2 is the core; Chunk 3 callers are a no-op fallback when `model==NULL`, so they are safe to land independently.
