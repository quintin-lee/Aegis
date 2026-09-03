# Tool Approval Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add an interactive per-tool approval flow: loop-level approval hook (control-flow callback), CLI y/n/a prompt with in-memory always-allow list, `/approvals` switch.

**Architecture:** Loop gains an optional `aegis_tool_approval_fn` called before `aegis_tool_execute`; DENY skips execution and feeds `"user denied tool <name>"` back as the tool result so the turn continues. Coding agent forwards the hook (same pattern as the event callback). CLI implements the prompt and `/approvals on|off`.

**Tech Stack:** C11/POSIX, CMake+CTest, no new dependencies.

**Spec:** `docs/superpowers/specs/2026-09-03-tool-approval-design.md`

---

## Chunk 1: Loop approval hook

### Task 1: Failing test — deny blocks execution, feeds denial text, turn continues

**Files:**
- Modify: `tests/unit/test_agent_events.c`

- [ ] **Step 1: Make `read_probe` count invocations**

Add `static int read_calls = 0;` near `read_probe`; first line of the probe: `++read_calls;`.

- [ ] **Step 2: Add approval callbacks + test at end of `main()` (before cleanup)**

```c
    /* ── Tool approval hook ─────────────────────────────────────────── */
    {
        /* allow-all: executes normally */
        aegis_agent_loop_set_tool_approval(loop,
            [](const char*, const char*, void* u) { (void)u; return AEGIS_TOOL_APPROVAL_ALLOW; },
            NULL);
```

NOTE: the project is C11, not C++. Use named static functions:

```c
static aegis_tool_approval_t approve_all(const char* n, const char* a, void* u)
{ (void)n; (void)a; (void)u; return AEGIS_TOOL_APPROVAL_ALLOW; }

static aegis_tool_approval_t deny_all(const char* n, const char* a, void* u)
{ (void)n; (void)a; (void)u; return AEGIS_TOOL_APPROVAL_DENY; }
```

Then in `main()` after the existing assertions (loop still alive, `turn` reset to 0):

```c
    /* Deny-all: probe must NOT run; denial text goes back as tool result. */
    read_calls = 0;
    turn = 0;
    assert(aegis_agent_loop_set_tool_approval(loop, deny_all, NULL) == AEGIS_OK);
    assert(aegis_agent_loop_run_turn(loop, "read README.md again") == AEGIS_OK);
    assert(read_calls == 0);
    /* find the tool result message and check its content */
    size_t nm = aegis_session_message_count(session);
    const aegis_message_t* tr_msg = NULL;
    for (size_t i = nm; i > 0; i--) {
        const aegis_message_t* m = aegis_session_message_at(session, i - 1);
        if (aegis_message_role(m) == AEGIS_MESSAGE_TOOL) { tr_msg = m; break; }
    }
    assert(tr_msg && strstr(aegis_message_content(tr_msg), "user denied tool read") != NULL);

    /* Allow-all executes */
    read_calls = 0;
    turn = 0;
    assert(aegis_agent_loop_set_tool_approval(loop, approve_all, NULL) == AEGIS_OK);
    assert(aegis_agent_loop_run_turn(loop, "read README.md once more") == AEGIS_OK);
    assert(read_calls == 1);

    /* invalid args */
    assert(aegis_agent_loop_set_tool_approval(NULL, approve_all, NULL) == AEGIS_ERR_INVALID);
```

- [ ] **Step 3: Build + run to verify failure**

Run: `cmake --build build -j 2>&1 | grep -E "error" | head -3`
Expected: `aegis_agent_loop_set_tool_approval` / `AEGIS_TOOL_APPROVAL_ALLOW` undeclared.

### Task 2: Implement loop hook

**Files:**
- Modify: `include/aegis/agent/loop.h`, `src/agent/loop.c`

- [ ] **Step 1: Header** — after `aegis_agent_event_fn` typedef:

```c
typedef enum aegis_tool_approval {
    AEGIS_TOOL_APPROVAL_ALLOW = 0,
    AEGIS_TOOL_APPROVAL_DENY  = 1,
} aegis_tool_approval_t;

typedef aegis_tool_approval_t (*aegis_tool_approval_fn)(const char* tool_name,
                                                        const char* arguments_json,
                                                        void*       user);
```

Config struct gains:

```c
    aegis_tool_approval_fn      tool_approval;  // optional gate, NULL = allow all
    void*                       approval_user;  // borrowed
```

Declare `aegis_status_t aegis_agent_loop_set_tool_approval(aegis_agent_loop_t* loop, aegis_tool_approval_fn fn, void* user);`

- [ ] **Step 2: loop.c** — struct fields `aegis_tool_approval_fn tool_approval; void* approval_user;`; copy from cfg in create (same block as on_event); the deny branch in the WAITING_TOOL section:

```c
            aegis_tool_approval_t verdict = AEGIS_TOOL_APPROVAL_ALLOW;
            if (l->tool_approval) {
                verdict = l->tool_approval(name,
                                           raw_args ? raw_args : "{}",
                                           l->approval_user);
            }
            aegis_tool_result_t result = {0};
            if (verdict == AEGIS_TOOL_APPROVAL_DENY) {
                char denial[128];
                snprintf(denial, sizeof(denial), "user denied tool %s", name);
                st = aegis_tool_result_set_string(&result, denial);
            } else {
                st = aegis_tool_execute(l->tools, name, args, l->token, &result);
            }
```

(keep the rest of the TOOL_END handling identical — the denial flows through the existing result path; keep the TOOL_START event emission **before** the verdict so CLI shows ● even for denied calls.)

- [ ] **Step 3: Runtime setter** near `aegis_agent_loop_set_event_callback`:

```c
aegis_status_t aegis_agent_loop_set_tool_approval(aegis_agent_loop_t*    loop,
                                                  aegis_tool_approval_fn fn,
                                                  void*                  user)
{
    if (!loop) return AEGIS_ERR_INVALID;
    loop->tool_approval = fn;
    loop->approval_user = user;
    return AEGIS_OK;
}
```

- [ ] **Step 4: Run test to verify pass** — `ctest --test-dir build -R unit_agent_events --output-on-failure 2>&1 | tail -3`

- [ ] **Step 5: Full suite + commit Chunk 1**

Run: `ctest --test-dir build 2>&1 | tail -2`

```bash
git add include/aegis/agent/loop.h src/agent/loop.c tests/unit/test_agent_events.c
git commit -m "feat: tool approval hook in agent loop"
```

---

## Chunk 2: Coding agent forwarding

### Task 3: Test + implement `aegis_coding_agent_set_tool_approval`

**Files:**
- Modify: `tests/unit/test_coding_agent.c` (approval counter), `include/aegis/coding/coding_agent.h`, `src/coding/coding_agent.c`

- [ ] **Step 1: Failing test** — coding agent fixture (mock model, existing harness): set a deny-all approval, run a turn, assert the denial appears in the session; then switch to allow via the same setter and assert execution. Also `NULL` agent → `AEGIS_ERR_INVALID`.

- [ ] **Step 2: Header** next to `aegis_coding_agent_set_event_callback`:

```c
aegis_status_t aegis_coding_agent_set_tool_approval(aegis_coding_agent_t*  agent,
                                                    aegis_tool_approval_fn fn,
                                                    void*                  user);
```

- [ ] **Step 3: coding_agent.c** — struct fields `aegis_tool_approval_fn ap_fn; void* ap_user;`; write both into `lcfg` at create and in `set_model`'s `lcfg` (`.tool_approval = a->ap_fn, .approval_user = a->ap_user`); setter mirrors `set_event_callback`.

- [ ] **Step 4: Verify + commit Chunk 2**

Run: `ctest --test-dir build -R "unit_coding_agent|unit_agent_events" --output-on-failure 2>&1 | tail -3`

```bash
git add include/aegis/coding/coding_agent.h src/coding/coding_agent.c tests/unit/test_coding_agent.c
git commit -m "feat: forward tool approval through coding agent"
```

---

## Chunk 3: CLI prompt, /approvals, integration test

### Task 4: CLI implementation

**Files:**
- Modify: `apps/aegis/cli_interactive.c`

- [ ] **Step 1: Extend `cli_stream_ctx_t`**:

```c
#define CLI_MAX_ALLOWED 16
    bool approvals;                      /**< /approvals on|off           */
    char allowed_tools[CLI_MAX_ALLOWED][64];
    size_t allowed_count;
```

- [ ] **Step 2: Approval callback** (before `cli_event_cb`):

```c
static aegis_tool_approval_t cli_approval_cb(const char* tool_name, const char* args_json,
                                             void* user)
{
    cli_stream_ctx_t* cx = (cli_stream_ctx_t*)user;
    if (!cx || !cx->approvals || !tool_name) {
        return AEGIS_TOOL_APPROVAL_ALLOW;
    }
    for (size_t i = 0; i < cx->allowed_count; i++) {
        if (strcmp(cx->allowed_tools[i], tool_name) == 0) {
            return AEGIS_TOOL_APPROVAL_ALLOW;
        }
    }
    cli_stream_prelude(cx);
    if (cx->reasoning_open) {
        fputs("\033[0m\n", stdout);
        cx->reasoning_open = false;
        cx->line_open      = false;
    }
    printf("approve %s %s? [y/n/a] ", tool_name, args_json ? args_json : "");
    fflush(stdout);
    char answer[16] = {0};
    if (!fgets(answer, sizeof(answer), stdin)) {
        cx->line_open = false;
        return AEGIS_TOOL_APPROVAL_DENY;   /* EOF => deny */
    }
    cx->line_open = false;
    if (answer[0] == 'a' && cx->allowed_count < CLI_MAX_ALLOWED) {
        snprintf(cx->allowed_tools[cx->allowed_count++],
                 sizeof(cx->allowed_tools[0]), "%s", tool_name);
        return AEGIS_TOOL_APPROVAL_ALLOW;
    }
    if (answer[0] == 'a') {
        return AEGIS_TOOL_APPROVAL_ALLOW;  /* list full: degrade to y */
    }
    if (answer[0] == 'y') {
        return AEGIS_TOOL_APPROVAL_ALLOW;
    }
    return AEGIS_TOOL_APPROVAL_DENY;
}
```

- [ ] **Step 3: `/approvals` command** near `/stream` handling:

```c
        if (strcmp(line, "/approvals") == 0 || strncmp(line, "/approvals ", 11) == 0) {
            const char* arg = line + 9;
            while (*arg == ' ') arg++;
            if (strcmp(arg, "on") == 0 || strcmp(arg, "off") == 0) {
                stream_ctx.approvals = (arg[0] == 'o' && arg[1] == 'n');
                printf("approvals %s\n", stream_ctx.approvals ? "on" : "off");
            } else if (*arg == '\0') {
                printf("approvals %s, always-allowed:",
                       stream_ctx.approvals ? "on" : "off");
                for (size_t i = 0; i < stream_ctx.allowed_count; i++) {
                    printf(" %s", stream_ctx.allowed_tools[i]);
                }
                printf("\n");
            } else {
                printf("usage: /approvals [on|off]\n");
            }
            continue;
        }
```

Register the callback once at startup (next to `set_event_callback`; harmless when off):

```c
    aegis_coding_agent_set_tool_approval(agent, cli_approval_cb, &stream_ctx);
```

Update `/help` string to include `/approvals`.

- [ ] **Step 4: Build + existing suites stay green** — `cmake --build build -j 2>&1 | grep error | head; ctest --test-dir build -R integration_cli --output-on-failure 2>&1 | tail -3`

### Task 5: Integration test — approve/deny/always-allow

**Files:**
- Modify: `tests/integration/test_cli.c` (new scenario `test_openai_provider_approvals`, reuse `sse_fixture_t`)

- [ ] **Step 1: Extend the SSE fixture** to serve tool calls on **every** turn (drop the reasoning-only branch or parameterize; simplest: always serve the tool-call body). Assert-based scenario uses two identical tool-call turns.

- [ ] **Step 2: Scenario** — input file: `/approvals on\nhello\nn\nhello\na\n/quit\n` (n denies first call; second turn's call gets `a`):

```c
    fputs("/approvals on\nhello\nn\nhello\na\n/quit\n", f);
```

Spawn exactly like `test_openai_provider_streaming`. Assertions:

```c
    assert_contains(out, "approvals on", "switch echo");
    assert_contains(out, "approve read", "approval prompt shown");
    assert_contains(out, "✗ user denied", "denial surfaced on tool event");
    assert(strstr(out, "approve read") == strstr(out, "approve read")); /* first prompt */
    /* second turn: exactly one prompt (the first); 'a' allowed the second */
    const char* p1 = strstr(out, "approve read");
    assert(p1 && strstr(p1 + 1, "approve read") == NULL);
```

(Also assert a final `Done.`-ish text if the fixture serves a final answer on turn 2+ — adjust the fixture so turn >= 2 serves `content` "done" after tool rounds: the fixture currently serves tool calls forever; change the loop to serve the answer body on turn 3.)

- [ ] **Step 3: Verify + full suite + commit Chunk 3**

Run: `cmake --build build -j 2>&1 | grep error | head; ctest --test-dir build 2>&1 | tail -2`

```bash
git add apps/aegis/cli_interactive.c tests/integration/test_cli.c
git commit -m "feat: interactive /approvals with y/n/a prompt"
```

---

## Chunk 4: Acceptance

### Task 6: ASan + live smoke

- [ ] **Step 1: ASan build + full ctest** — `cmake -S . -B build-asan > /dev/null 2>&1 && cmake --build build-asan -j 2>&1 | grep error | head; timeout 180 ctest --test-dir build-asan 2>&1 | grep -E "tests passed|Failed"`

- [ ] **Step 2: Live smoke** against `/tmp/aegis_live` mock server with `AEGIS_APPROVALS` via `/approvals on` — verify prompt reads from the TTY pipe correctly (`runtest3.sh` variant with `n` + `a` inputs).

- [ ] **Step 3: Commit stragglers.**
