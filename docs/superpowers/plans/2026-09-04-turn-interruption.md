# Turn Interruption Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let the CLI user interrupt an in-flight agent turn (Enter key), cancel the model request immediately (curl progress callback), keep the partial reply in the session with an `[interrupted by user]` marker, and keep the REPL usable afterwards.

**Architecture:** Three layers. (1) Agent loop: on `CANCELLED` from the model stream, append the partial assistant message + a marker user message instead of discarding; add `aegis_agent_loop_set_token` for runtime token rebinding. (2) Coding agent: owns a cancellation token per turn (recreated each run), exposes `aegis_coding_agent_interrupt()`. (3) OpenAI provider: curl `XFERINFOFUNCTION` polls the token so an in-flight HTTP request aborts instantly. (4) CLI: a stdin reader thread feeds a line queue; empty line during a turn = interrupt, non-empty line = queued for after the turn.

**Tech Stack:** C11, pthreads, libcurl (`CURLOPT_XFERINFOFUNCTION`), existing `aegis_cancellation_token_t`.

**Spec:** `docs/superpowers/specs/2026-09-04-turn-interruption-design.md`

---

## Chunk 1: Agent loop — partial preservation + token rebinding

### Task 1: Failing test — cancellation preserves partial reply

**Files:**
- Modify: `tests/unit/test_agent_events.c` (add scenario before final `PASSED` print at line ~236)

The fixture backend (`model_backend_stream`, line 88) streams text `"done"` driven by an `int turn` counter in `backend.user`. For the interrupt scenario we need a *separate* fixture model whose stream callback triggers cancellation mid-stream. Steps:

- [ ] **Step 1: Add fixture + scenario to `tests/unit/test_agent_events.c`**

Add after `model_backend_stream` (line ~130) a new backend whose stream emits one TEXT_DELTA (`"partial text"`) and then returns `AEGIS_ERR_CANCELLED` (simulating the provider observing cancellation). It needs access to the loop to cancel; store the loop pointer in a static, set right after `aegis_agent_loop_create`:

```c
static aegis_status_t cancel_mid_stream(void* user, const aegis_model_request_t* request,
                                        const aegis_cancellation_token_t* token,
                                        aegis_model_stream_callback_fn    callback,
                                        void*                             callback_user)
{
    (void)user; (void)request; (void)token;
    const char*                part = "partial text";
    aegis_model_stream_event_t ev   = {
        .type = AEGIS_MODEL_STREAM_TEXT_DELTA, .data = part, .len = strlen(part),
    };
    assert(callback(&ev, callback_user) == AEGIS_OK);
    return AEGIS_ERR_CANCELLED; /* provider observed cancellation */
}
```

In `main`, before the final destroy/PASSED block, add a fresh loop sharing the same session, with `cancel_mid_stream` as backend:

```c
/* ── Cancellation: partial reply is preserved + marker appended ────── */
{
    int                   turn2     = 0;
    aegis_model_backend_t cbackend  = {
        .user = &turn2, .complete = NULL, .stream = cancel_mid_stream,
        .capabilities = AEGIS_MODEL_CAP_TEXT | AEGIS_MODEL_CAP_STREAMING,
    };
    aegis_model_client_t* cmodel = NULL;
    assert(aegis_model_client_create_with_backend("fixture-cancel", &cbackend, &cmodel) == AEGIS_OK);
    aegis_agent_loop_config_t ccfg = {
        .session = session, .model = cmodel, .tools = tools, .system_prompt = "fixture",
    };
    aegis_agent_loop_t* cloop = NULL;
    assert(aegis_agent_loop_create(&ccfg, &cloop) == AEGIS_OK);

    size_t before = aegis_session_message_count(session);
    assert(aegis_agent_loop_run_turn(cloop, "interrupt me") == AEGIS_ERR_CANCELLED);
    assert(aegis_agent_loop_state(cloop) == AEGIS_AGENT_LOOP_CANCELLED);

    /* session gained: user msg, partial assistant msg, marker user msg */
    assert(aegis_session_message_count(session) == before + 3);
    const aegis_message_t* amsg = aegis_session_message_at(session, before + 1);
    assert(aegis_message_role(amsg) == AEGIS_MESSAGE_ASSISTANT);
    assert(strcmp(aegis_message_content(amsg), "partial text") == 0);
    const aegis_message_t* marker = aegis_session_message_at(session, before + 2);
    assert(aegis_message_role(marker) == AEGIS_MESSAGE_USER);
    assert(strcmp(aegis_message_content(marker), "[interrupted by user]") == 0);

    aegis_agent_loop_destroy(cloop);
    aegis_model_client_destroy(cmodel);
}
```

- [ ] **Step 2: Build and verify RED**

Run: `cmake --build build -j 2>&1 | grep -E "error" | head -5`
Expected: `implicit declaration of function 'aegis_agent_loop_set_token'` is NOT expected here (test doesn't use it yet); the test should **compile** but FAIL at runtime:
`assert(aegis_session_message_count(session) == before + 3)` — because the current loop discards `acc` and appends nothing on CANCELLED.

- [ ] **Step 3: Implement partial preservation in `src/agent/loop.c`**

In `aegis_agent_loop_run_turn`, replace the model-stream error branch (lines ~594-601):

```c
st = aegis_model_stream(l->model, &req, l->token, stream_cb, &acc);
aegis_message_list_destroy(ctx_msgs);
if (st != AEGIS_OK) {
    if (st == AEGIS_ERR_CANCELLED) {
        /* Preserve whatever streamed before the interrupt, then mark it. */
        aegis_message_t* pm = NULL;
        if (aegis_message_create(AEGIS_MESSAGE_ASSISTANT, &pm) == AEGIS_OK &&
            aegis_message_set_content(pm, acc.text ? acc.text : "") == AEGIS_OK) {
            if (acc.reasoning) {
                aegis_message_set_reasoning(pm, acc.reasoning);
            }
            for (size_t i = 0; i < acc.call_count; i++) {
                aegis_tool_call_t* call = NULL;
                if (aegis_tool_call_create(&call) == AEGIS_OK &&
                    aegis_tool_call_set_id(call, acc.calls[i].call_id) == AEGIS_OK &&
                    aegis_tool_call_set_name(call, acc.calls[i].name) == AEGIS_OK &&
                    aegis_tool_call_set_arguments(
                        call, acc.calls[i].arguments ? acc.calls[i].arguments : "{}") == AEGIS_OK &&
                    aegis_message_add_tool_call(pm, call) == AEGIS_OK) {
                    /* attached */
                }
                aegis_tool_call_destroy(call);
            }
            aegis_session_append_message(l->session, pm);
            aegis_message_t* mk = NULL;
            if (aegis_message_create(AEGIS_MESSAGE_USER, &mk) == AEGIS_OK &&
                aegis_message_set_content(mk, "[interrupted by user]") == AEGIS_OK) {
                aegis_session_append_message(l->session, mk);
                aegis_message_destroy(mk);
            }
            aegis_message_destroy(pm);
        }
        stream_accum_destroy(&acc);
        set_state(l, AEGIS_AGENT_LOOP_CANCELLED);
        return AEGIS_ERR_CANCELLED;
    }
    stream_accum_destroy(&acc);
    set_state(l, AEGIS_AGENT_LOOP_FAILED);
    return st;
}
```

- [ ] **Step 4: Run test to verify GREEN**

Run: `ctest --test-dir build -R unit_agent_events --output-on-failure 2>&1 | tail -3`
Expected: `100% tests passed out of 1`

- [ ] **Step 5: Commit**

```bash
git add src/agent/loop.c tests/unit/test_agent_events.c
git commit -m "feat: preserve partial reply and marker on turn cancellation"
```

### Task 2: `aegis_agent_loop_set_token` runtime rebind

**Files:**
- Modify: `include/aegis/agent/loop.h` (after `aegis_agent_loop_cancel`, line ~85)
- Modify: `src/agent/loop.c` (next to `set_event_callback`, line ~96)
- Test: `tests/unit/test_agent_events.c`

- [ ] **Step 1: Add failing assertions to the interrupt scenario** (inside the Task-1 block, before destroy):

```c
/* Token rebind: a fresh token clears a cancelled state for the next run. */
aegis_cancellation_token_t* tok = NULL;
assert(aegis_cancellation_token_create(&tok) == AEGIS_OK);
assert(aegis_agent_loop_set_token(cloop, tok) == AEGIS_OK);
assert(aegis_agent_loop_set_token(NULL, tok) == AEGIS_ERR_INVALID);
assert(aegis_agent_loop_set_token(cloop, tok) == AEGIS_OK); /* idempotent */
aegis_cancellation_token_request_cancel(tok);
/* run_turn refuses immediately on a cancelled token (pre-existing guard) */
assert(aegis_agent_loop_run_turn(cloop, "x") == AEGIS_ERR_CANCELLED);
aegis_cancellation_token_destroy(tok);
```

Note: destroy of a bound token is the caller's responsibility to unbind first in real use; in the test the loop is destroyed right after, and the loop only stores the borrowed pointer, so this is safe here.

- [ ] **Step 2: Build → RED** (implicit declaration of `aegis_agent_loop_set_token`)

Run: `cmake --build build -j 2>&1 | grep -cE "error"` → expect ≥ 1

- [ ] **Step 3: Implement**

`include/aegis/agent/loop.h` (after the cancel/pause/resume declarations):

```c
/** Rebind the loop's cancellation token at runtime (borrowed; NULL = none).
 *  Thread-safe; takes the loop lock briefly. */
aegis_status_t aegis_agent_loop_set_token(aegis_agent_loop_t*         loop,
                                          aegis_cancellation_token_t* token);
```

`src/agent/loop.c` (mirror `set_event_callback`'s lock usage):

```c
aegis_status_t aegis_agent_loop_set_token(aegis_agent_loop_t* l, aegis_cancellation_token_t* token)
{
    if (!l) {
        return AEGIS_ERR_INVALID;
    }
    pthread_mutex_lock(&l->lock);
    l->token = token;
    pthread_mutex_unlock(&l->lock);
    return AEGIS_OK;
}
```

- [ ] **Step 4: GREEN** — `ctest --test-dir build -R unit_agent_events --output-on-failure 2>&1 | tail -1` → `100% tests passed out of 1`

- [ ] **Step 5: Commit** — `git add include/aegis/agent/loop.h src/agent/loop.c tests/unit/test_agent_events.c && git commit -m "feat: runtime token rebinding on agent loop"`

## Chunk 2: Coding agent — per-turn token + interrupt()

### Task 3: `aegis_coding_agent_interrupt` + token lifecycle

**Files:**
- Modify: `include/aegis/coding/coding_agent.h` (after `aegis_coding_agent_set_tool_approval`, line ~76)
- Modify: `src/coding/coding_agent.c` (struct line ~22, three `loop_create` sites lines 172/249/340, `run` line ~262, destroy line ~191)
- Test: `tests/unit/test_coding_agent.c`

- [ ] **Step 1: Failing test in `tests/unit/test_coding_agent.c`**

The existing mock model (`src/model/model.c` default backend) checks the token between stream chunks and returns `AEGIS_ERR_CANCELLED`. To test interrupt mid-turn, add a fixture backend (file already includes `aegis/tool/tool.h`; add `#include "aegis/common/cancellation/cancellation.h"` and `#include "aegis/agent/loop.h"` if not present) whose stream cancels the loop via `aegis_coding_agent_interrupt` — but the coding-agent API takes the agent; simplest: use a global `aegis_coding_agent_t* g_agent` set before run, stream emits one TEXT_DELTA then calls `aegis_coding_agent_interrupt(g_agent)` and returns `AEGIS_ERR_CANCELLED`:

```c
static aegis_coding_agent_t* g_int_agent = NULL;

static aegis_status_t int_stream(void* user, const aegis_model_request_t* request,
                                 const aegis_cancellation_token_t* token,
                                 aegis_model_stream_callback_fn    callback, void* callback_user)
{
    (void)user; (void)request; (void)token;
    const char*                part = "partial";
    aegis_model_stream_event_t ev   = {
        .type = AEGIS_MODEL_STREAM_TEXT_DELTA, .data = part, .len = strlen(part),
    };
    callback(&ev, callback_user);
    aegis_coding_agent_interrupt(g_int_agent);
    return AEGIS_ERR_CANCELLED;
}
```

Test body (before final PASSED):

```c
/* ── Interrupt: mid-turn cancel, partial kept, next run works ──────── */
{
    aegis_model_backend_t ibackend = {
        .user = NULL, .complete = NULL, .stream = int_stream,
        .capabilities = AEGIS_MODEL_CAP_TEXT | AEGIS_MODEL_CAP_STREAMING,
    };
    /* Build a custom agent via config with tools=NULL default? coding_agent
       config has no backend field — use set_model-style custom backend:
       instead reuse aegis_coding_agent_create then swap model is not exposed.
       Use the loop-level test for cancel semantics; here test that interrupt()
       is a no-op-safe call and run() recovers. */
    aegis_coding_agent_t* ia = NULL;
    expect_ok(aegis_coding_agent_create(&cfg, &ia), "create interrupt agent");
    g_int_agent = ia;
    assert(aegis_coding_agent_interrupt(NULL) == AEGIS_ERR_INVALID);
    assert(aegis_coding_agent_interrupt(ia) == AEGIS_OK); /* idle cancel is safe */
    expect_ok(aegis_coding_agent_run(ia, "hello"), "run after idle interrupt");
    /* each run recreates the token, so an earlier interrupt cannot leak */
    expect_ok(aegis_coding_agent_run(ia, "hello again"), "second run fresh token");
    aegis_coding_agent_destroy(ia);
    g_int_agent = NULL;
}
```

- [ ] **Step 2: Build → RED** (implicit declaration of `aegis_coding_agent_interrupt`)

- [ ] **Step 3: Implement**

Header:

```c
/** Request cooperative interruption of the current turn (lock-free,
 *  callable from any thread, including signal-adjacent contexts and the
 *  CLI reader thread). Safe when no turn is running. */
aegis_status_t aegis_coding_agent_interrupt(const aegis_coding_agent_t* agent);
```

Impl — struct gains `aegis_cancellation_token_t* token;` (owned). All three `loop_create` config literals gain `.token = a->token,`. `run` becomes:

```c
aegis_status_t aegis_coding_agent_run(aegis_coding_agent_t* a, const char* user_input)
{
    if (!a || !user_input) {
        return AEGIS_ERR_INVALID;
    }
    /* Fresh token per turn: cancellation is one-shot and must not leak
     * into the next run. */
    aegis_cancellation_token_t* tok = NULL;
    aegis_status_t              st  = aegis_cancellation_token_create(&tok);
    if (st != AEGIS_OK) {
        return st;
    }
    if (a->token) {
        aegis_cancellation_token_destroy(a->token);
    }
    a->token = tok;
    aegis_agent_loop_set_token(a->loop, tok);
    return aegis_agent_loop_run(a->loop, user_input);
}
```

New function:

```c
aegis_status_t aegis_coding_agent_interrupt(const aegis_coding_agent_t* a)
{
    if (!a || !a->loop) {
        return AEGIS_ERR_INVALID;
    }
    return aegis_agent_loop_cancel((aegis_agent_loop_t*)a->loop);
}
```

Destroy gains `if (a->token) { aegis_cancellation_token_destroy(a->token); }` before `free(a)`. Include `aegis/common/cancellation/cancellation.h` (already transitively available via loop.h; include explicitly if needed).

Note: `replace_session` (line ~249) and `set_model` (line ~340) rebuild the loop; add `.token = a->token,` so the current token stays bound.

- [ ] **Step 4: GREEN** — `ctest --test-dir build -R unit_coding_agent --output-on-failure 2>&1 | tail -1` → `100% tests passed out of 1`

- [ ] **Step 5: Commit** — `git add include/aegis/coding/coding_agent.h src/coding/coding_agent.c tests/unit/test_coding_agent.c && git commit -m "feat: coding agent per-turn token and interrupt()"`

## Chunk 3: OpenAI provider — curl immediate abort

### Task 4: XFERINFOFUNCTION polls the token

**Files:**
- Modify: `providers/llm/openai/structured_openai.c` (both `structured_stream` ~line 396 and the `complete` path's curl setup ~line 490s; `sse_state_t` at line ~35)

- [ ] **Step 1: Failing test in `tests/unit/test_structured_openai.c`** — check how existing tests there drive curl (they likely use a local fixture server or mock). If a unit-level test is impractical without a slow server, cover via the integration slow-SSE scenario in Task 6 instead, and here only assert compile-level wiring. Prefer: extend `tests/system/test_openai_sse_e2e.c` with a *slow* fixture (sleep between chunks) and assert `AEGIS_ERR_CANCELLED` when the token is cancelled from another thread mid-transfer:

```c
/* In the SSE e2e test file: scenario "slow stream + cancel mid-transfer".
 * Fixture server sends: chunk, 400ms sleep, chunk, ... (5 chunks).
 * Main thread: create token, bind to request, spawn pthread that sleeps
 * 300ms then request_cancel. Expect aegis_model_client_stream to return
 * AEGIS_ERR_CANCELLED well before the transfer completes. */
```

- [ ] **Step 2: Run → RED** (returns `AEGIS_ERR_PROVIDER` or blocks until server finishes — not `AEGIS_ERR_CANCELLED` promptly)

- [ ] **Step 3: Implement in `structured_openai.c`**

Add to `sse_state_t`: keep existing `token` field (already present, line 35). Add progress callback:

```c
static int sse_progress(void* user, curl_off_t dltotal, curl_off_t dlnow,
                        curl_off_t ultotal, curl_off_t ulnow)
{
    (void)dltotal; (void)dlnow; (void)ultotal; (void)ulnow;
    sse_state_t* s = user;
    if (s && s->token && aegis_cancellation_token_is_cancelled(s->token)) {
        return 1; /* non-zero aborts the transfer */
    }
    return 0;
}
```

In **both** curl setups (stream + complete; give the complete path's state a token field too, or reuse a small struct):

```c
curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, sse_progress);
curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &state);
```

After `curl_easy_perform` (both paths), **before** other error checks:

```c
if (token && aegis_cancellation_token_is_cancelled(token)) {
    /* cleanup */
    return AEGIS_ERR_CANCELLED;
}
```

(The stream path already returns CANCELLED post-perform but only *after* `cr != CURLE_OK` → `AEGIS_ERR_PROVIDER`; reorder so cancellation wins.)

- [ ] **Step 4: GREEN** — `ctest --test-dir build -R "system_openai_sse_e2e|unit_structured_openai" --output-on-failure 2>&1 | tail -1`

- [ ] **Step 5: Commit** — `git add providers/llm/openai/structured_openai.c tests/system/test_openai_sse_e2e.c && git commit -m "feat: abort in-flight OpenAI requests on cancellation"`

## Chunk 4: CLI — reader thread, Enter-to-interrupt, queueing

### Task 5: Reader thread + line queue + interrupt wiring

**Files:**
- Modify: `apps/aegis/cli_interactive.c` (REPL loop starting line ~244)
- Modify: `CMakeLists.txt` line ~95 (`target_link_libraries(aegis PRIVATE aegis_core)` → add pthread)
- Test: `tests/integration/test_cli.c`

- [ ] **Step 1: CMake — link pthread into `aegis`**

```cmake
target_link_libraries(aegis PRIVATE aegis_core pthread)
```

(Keep the OpenAI conditional block as-is.)

- [ ] **Step 2: Failing integration test in `tests/integration/test_cli.c`**

Reuse the slow-SSE fixture pattern from Task 4's test (a fixture server variant with inter-chunk sleeps; add a `mode=slow` branch to the existing fixture thread in `test_openai_provider_streaming`, or a second fixture function `test_openai_provider_interrupt`):

```c
/* Scenario: start CLI with slow fixture; feed "hello\n" then, ~200ms later,
 * "\n" (empty line = interrupt); then "hello2\n" and "/quit\n".
 * Assertions:
 *   - output contains "⏹ interrupted"
 *   - output contains reply for "hello2" (mock: "mock stream for: hello2")
 *   - exit code 0 */
/* Queueing scenario: feed "hello\n" then immediately "hello2\n" (both during
 * the slow first turn); assert both replies appear in order. */
```

- [ ] **Step 3: Run → RED** (empty line today is ignored → no `⏹ interrupted`)

Run: `timeout 60 ./build/integration_cli > /tmp/int_t.log 2>&1; tr -d '\r' < /tmp/int_t.log | grep -c "interrupted"` → expect 0

- [ ] **Step 4: Implement in `apps/aegis/cli_interactive.c`**

Add includes `<pthread.h>`, `<sched.h>` (or use nanosleep via `<time.h>`, already included). Add above `cmd_interactive`:

```c
/* ── stdin reader thread: decouples typing from turn execution ─────── */
typedef struct line_cell {
    char*              text;      /* heap copy; NULL = EOF sentinel */
    struct line_cell*  next;
} line_cell_t;

typedef struct line_queue {
    line_cell_t*  head;
    line_cell_t*  tail;
    pthread_mutex_t mu;
    pthread_cond_t  cv;
    bool            closed;
} line_queue_t;

static void lq_push(line_queue_t* q, char* text)
{
    line_cell_t* c = malloc(sizeof(*c));
    if (!c) { free(text); return; }
    *c = (line_cell_t){ .text = text, .next = NULL };
    pthread_mutex_lock(&q->mu);
    if (q->tail) q->tail->next = c; else q->head = c;
    q->tail = c;
    pthread_cond_signal(&q->cv);
    pthread_mutex_unlock(&q->mu);
}

static char* lq_pop(line_queue_t* q)  /* blocks; NULL = EOF */
{
    pthread_mutex_lock(&q->mu);
    while (!q->head && !q->closed) pthread_cond_wait(&q->cv, &q->mu);
    line_cell_t* c = q->head;
    char* text = NULL;
    if (c) {
        q->head = c->next;
        if (!q->head) q->tail = NULL;
        text = c->text;
        free(c);
    }
    pthread_mutex_unlock(&q->mu);
    return text;
}

static void lq_close(line_queue_t* q)
{
    pthread_mutex_lock(&q->mu);
    q->closed = true;
    pthread_cond_broadcast(&q->cv);
    pthread_mutex_unlock(&q->mu);
}

static line_queue_t g_lines;

static void* reader_main(void* arg)
{
    (void)arg;
    char buf[4096];
    while (fgets(buf, sizeof(buf), stdin)) {
        size_t n = strlen(buf);
        while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r')) buf[--n] = '\0';
        lq_push(&g_lines, strdup(buf));
    }
    lq_push(&g_lines, NULL); /* EOF sentinel */
    return NULL;
}
```

Restructure `cmd_interactive`'s REPL: outer state machine with `char** pending = NULL; size_t npending = 0;` and a helper `run_line(const char* line)` containing today's per-line body (command parsing + `aegis_coding_agent_run` + output). Skeleton:

```c
pthread_t reader;
line_queue_t lq_init = { .head=NULL,.tail=NULL,.closed=false,.mu=PTHREAD_MUTEX_INITIALIZER,.cv=PTHREAD_COND_INITIALIZER };
g_lines = lq_init;
pthread_create(&reader, NULL, reader_main, NULL);

bool running = true;
char* cur = NULL;                 /* line being processed this iteration */
char** pending = NULL; size_t npending = 0, cap_pending = 0;
while (running) {
    if (!cur) {
        cur = lq_pop(&g_lines);   /* NULL => EOF */
        if (!cur) break;
    }
    /* idle: strip as today */
    if (cur[0] == '\0' && npending == 0 && !busy) { free(cur); cur = NULL; continue; }
    busy = true;  /* set before run; cleared after */
    ...
}
```

Concretely (keep it simple, match existing style):

- `busy` flag around the `aegis_coding_agent_run` call.
- Before invoking `run_line(cur)`: if line is empty and busy → `aegis_coding_agent_interrupt(agent)` and free it; else if line is empty → ignore.
- While a turn runs we cannot pump the queue; instead: after pushing `cur` into pending when busy is impossible (single-threaded main), the flow is: pop a line; if busy → (shouldn't happen); execute it. During execution no new lines are read. After the turn returns, drain `pending`: each queued line goes through the same `run_line` path.

To let empty-line interrupts arrive *while* a turn runs, the turn must run while the main thread polls the queue. Simplest correct structure without extra threads: pre-drain the queue non-blockingly is impossible — the turn blocks the main thread. **So:** the interrupt check happens inside the existing event callback: `cli_event_cb` sees no stdin. **Final approach:** spawn the reader thread (it fills the queue while the turn runs); in `run_line`, before calling `aegis_coding_agent_run`, arm `stream_ctx.interrupt_on_empty = true`; and check the queue from `cli_event_cb`? — No: callbacks are sync on the run thread.

**Chosen design (thread-safe, minimal):** run `aegis_coding_agent_run` on the main thread as today, but first spawn a **watcher** (one per turn) that pops lines: empty line → `interrupt(agent)`; non-empty → append to `pending`; EOF → close. After the turn, main joins the watcher, prints `⏹ interrupted` if cancelled, and processes `pending` lines sequentially. Idle state: main itself pops lines (no watcher) — empty lines ignored, `/quit` etc. work as today.

```c
typedef struct watcher_ctx {
    aegis_coding_agent_t* agent;
    char**                pending;
    size_t                n, cap;
    bool                  interrupted;
} watcher_ctx_t;

static void* watcher_main(void* arg)
{
    watcher_ctx_t* w = arg;
    while (1) {
        char* line = lq_pop(&g_lines);
        if (!line) break;                     /* EOF */
        if (line[0] == '\0') {
            if (!w->interrupted) {
                w->interrupted = true;
                aegis_coding_agent_interrupt(w->agent);
            }
            free(line);
            continue;
        }
        if (w->n == w->cap) {
            w->cap = w->cap ? w->cap * 2 : 8;
            w->pending = realloc(w->pending, w->cap * sizeof(char*));
        }
        w->pending[w->n++] = line;
    }
    return NULL;
}
```

In the REPL: when the popped line is a turn-candidate (non-empty, non-command), start the watcher thread, call `run_line`, join, then loop over `w.pending` feeding each through the same path (commands and further turns; a turn inside the drain also gets a watcher — recursion depth is fine because each drain item is processed by the same outer code path; to keep it simple, process drain items iteratively by pushing them back into a local FIFO and re-entering the same single-level loop).

Implementation detail to keep the diff small: convert the existing `while(1) { fgets...; body }` into `while (1) { char* line = next_line(&w); body(line); }` where `next_line` encapsulates: idle pop → if turn-candidate, run watcher pattern. `/quit` → `lq_close` + `pthread_cancel(reader)` avoidance: set `g_lines.closed = true` then `pthread_join(reader)` after loop; reader wakes on closed via broadcast (already in `lq_pop`).

Cancellation display: `run_line` keeps today's `st2 == AEGIS_ERR_CANCELLED → printf("cancelled\n")` branch but change the text to `⏹ interrupted`.

- [ ] **Step 5: GREEN** — rebuild + `timeout 120 ./build/integration_cli > /tmp/int2.log 2>&1; echo $?; tr -d '\r' < /tmp/int2.log | grep -E "interrupted|mock stream for: hello2" | head -4`

- [ ] **Step 6: Commit** — `git add apps/aegis/cli_interactive.c CMakeLists.txt tests/integration/test_cli.c && git commit -m "feat: Enter-to-interrupt and message queueing in interactive CLI"`

## Chunk 5: Full verification

### Task 6: Full suite + ASan + live smoke

- [ ] **Step 1:** `pkill -9 -x aegis 2>/dev/null; ctest --test-dir build 2>&1 | grep "tests passed"` → expect `100% tests passed out of 60` (or 61+ with the new tests)
- [ ] **Step 2:** `cmake -S . -B build-asan > /dev/null 2>&1 && cmake --build build-asan -j > /dev/null 2>&1 && ctest --test-dir build-asan 2>&1 | grep "tests passed"` → expect all green, zero leaks
- [ ] **Step 3:** Live smoke with the real OpenAI mock server (`/tmp/aegis_live/mocksrv.py` extended with inter-chunk sleep): pipe `hello`, delay, empty line, `hello2`, `/quit` via a Python driver; verify `⏹ interrupted`, preserved partial text in the saved session JSONL, and that `hello2` gets a full reply
- [ ] **Step 4:** Commit any remaining fixes; working tree clean
