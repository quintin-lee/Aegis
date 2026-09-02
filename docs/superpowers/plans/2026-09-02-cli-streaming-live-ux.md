# CLI 流式输出（Live UX）实现计划

> **For agentic workers:** REQUIRED: Use superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Agent loop 新增观察者事件回调，CLI 交互模式实时呈现 assistant 文本 token 与工具调用/结果，机器可读模式不变。

**Architecture:** `aegis_agent_loop` 增加类型化事件回调（`on_event` config 字段 + 运行期 setter，void 返回、不持锁调用）；`aegis_coding_agent` 透传回调注册；`cli_interactive.c` 注册打印回调（●/✓ 工具行 + 逐 token 文本），配合流式优先去重与 `/stream on|off` 开关。

**Tech Stack:** C11, CMake/CTest, 既有 agent loop / model stream / tool API。

**Spec:** `docs/superpowers/specs/2026-09-02-cli-streaming-live-ux-design.md`

**注意:** 工作区有两处与本计划无关的未提交改动（`src/autonomous/execution.c`、`tests/system/test_autonomous_closed_loop.c`），`git add` 必须精确指定文件。

---

## File Map

| 文件 | 操作 | 职责 |
|------|------|------|
| `include/aegis/agent/loop.h` | 修改 | 事件枚举/结构/回调类型、config 字段、运行期 setter 声明 |
| `src/agent/loop.c` | 修改 | 存回调；stream_cb 转发 TEXT_DELTA；工具执行点发 TOOL_START/TOOL_END |
| `include/aegis/coding/coding_agent.h` | 修改 | `aegis_coding_agent_set_event_callback` 声明 |
| `src/coding/coding_agent.c` | 修改 | 存 fn/user；建 loop 时传入；即时换绑 |
| `apps/aegis/cli_interactive.c` | 修改 | 打印回调、`/stream` 开关、去重 |
| `tests/unit/test_agent_events.c` | 新建 | 事件序列单元测试 |
| `tests/integration/test_cli.c` | 修改 | 流式输出集成用例 |
| `cmake/AegisTests.cmake` | 修改 | 注册 unit_agent_events + ASan 列表 |

---

## Chunk 1: Agent Loop 事件回调

### Task 1: 失败测试（事件序列）

**Files:**
- Create: `tests/unit/test_agent_events.c`
- Modify: `cmake/AegisTests.cmake`

- [ ] **Step 1: 写测试**（fixture 模式照抄 `tests/system/test_coding_loop.c`：注册 read 探针工具 + 2 轮 fixture 后端：第 0 轮发 TOOL_CALL start/delta/end，第 1 轮发 text delta "done"）

```c
#define _POSIX_C_SOURCE 200809L
#include "aegis/agent/loop.h"
#include "aegis/message/message.h"
#include "aegis/model/model.h"
#include "aegis/session/session.h"
#include "aegis/tool/tool.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* 事件记录：最多 32 条 {type, tool_name, status} */
typedef struct ev_rec {
    aegis_agent_event_type_t type;
    char tool_name[64];
    aegis_status_t status;
    char text[64];
} ev_rec_t;

typedef struct ev_log {
    ev_rec_t items[32];
    size_t   n;
} ev_log_t;

static void ev_push(ev_log_t* log, aegis_agent_event_type_t t, const char* tool,
                    aegis_status_t st, const char* text)
{
    assert(log->n < 32);
    ev_rec_t* r = &log->items[log->n++];
    r->type = t; r->status = st;
    snprintf(r->tool_name, sizeof(r->tool_name), "%s", tool ? tool : "");
    snprintf(r->text, sizeof(r->text), "%s", text ? text : "");
}

static void ev_cb(const aegis_agent_event_t* ev, void* user)
{
    ev_log_t* log = (ev_log_t*)user;
    switch (ev->type) {
    case AEGIS_AGENT_EVENT_TEXT_DELTA:
        ev_push(log, ev->type, NULL, AEGIS_OK, (const char*)ev->data);
        break;
    case AEGIS_AGENT_EVENT_TOOL_START:
    case AEGIS_AGENT_EVENT_TOOL_END:
        ev_push(log, ev->type, ev->tool_name, ev->status, NULL);
        break;
    default: break;
    }
}

/* read 探针：断言 path=="README.md"，返回 "fixture contents" */
static aegis_status_t read_probe(...) { /* 同 test_coding_loop.c */ }

/* fixture 后端 stream：turn 0 发 tool call 事件；否则发 text "done" */
static aegis_status_t model_backend_stream(...) { /* 同 test_coding_loop.c */ }

int main(void)
{
    /* 建立 session/tools/model/loop，loop config 带 on_event=ev_cb, event_user=&log */
    /* run_turn -> 断言事件序列：
       [0]=TOOL_START(read) [1]=TOOL_END(read, ok) [2]=TEXT_DELTA("done") */
    /* run 后立即 set_event_callback(loop, NULL, NULL) 换绑为空并回归 run */
    puts("ALL_AGENT_EVENT_TESTS PASSED");
}
```

- [ ] **Step 2: CMake 注册** `aegis_add_test(unit_agent_events tests/unit/test_agent_events.c)` + ASan 属性列表追加
- [ ] **Step 3: 验证失败**：`cmake --build build -j` → 编译失败（枚举不存在）

### Task 2: 实现 loop 事件回调

**Files:**
- Modify: `include/aegis/agent/loop.h`、`src/agent/loop.c`

- [ ] **Step 1: loop.h 增加声明**

```c
typedef enum aegis_agent_event_type {
    AEGIS_AGENT_EVENT_TEXT_DELTA = 0,
    AEGIS_AGENT_EVENT_TOOL_START = 1,
    AEGIS_AGENT_EVENT_TOOL_END   = 2,
} aegis_agent_event_type_t;

typedef struct aegis_agent_event {
    aegis_agent_event_type_t type;
    const void*     data;
    size_t          len;
    const char*     tool_name;
    const char*     call_id;
    aegis_status_t  status;
} aegis_agent_event_t;

typedef void (*aegis_agent_event_fn)(const aegis_agent_event_t* ev, void* user);
```

config 增加 `aegis_agent_event_fn on_event; void* event_user;`；新增 `aegis_status_t aegis_agent_loop_set_event_callback(aegis_agent_loop_t* loop, aegis_agent_event_fn fn, void* user);`

- [ ] **Step 2: loop.c 实现**

- struct 加 `aegis_agent_event_fn on_event; void* event_user;`
- create：从 cfg 拷贝（在加锁之前，无并发问题）
- `static void emit_event(aegis_agent_loop_t* l, const aegis_agent_event_t* ev)`：**先读回调指针再调用，不持锁**（回调只读，运行期换绑只改指针，最坏情况错过/多收一条事件，可接受并注释说明）
- `stream_cb`：TEXT_DELTA 累积成功后，`if (l->on_event)` 构造 event（data=ev->data, len=ev->len）转发——注意 stream_cb 的 user 是 `&acc`，需在 acc 中存 loop 指针（`stream_accum_t` 加 `aegis_agent_loop_t* loop;` 字段，run_turn 里赋值）
- 工具执行点：`aegis_tool_execute` 前发 TOOL_START（tool_name/cid）；执行后、append tool message 后发 TOOL_END（status=st；成功且字符串结果时 data=result 内容, len=strlen）
- `aegis_agent_loop_set_event_callback`：加锁写指针字段（与 set_state 同一锁保护），返回 OK；NULL loop 返回 INVALID

- [ ] **Step 3: 验证通过**

Run: `cmake --build build -j && ctest --test-dir build -R unit_agent_events --output-on-failure`
Expected: ALL_AGENT_EVENT_TESTS PASSED

- [ ] **Step 4: 回归**：`ctest --test-dir build -R "system_coding_loop|system_openai" --output-on-failure` 全过
- [ ] **Step 5: Commit**

```bash
git add include/aegis/agent/loop.h src/agent/loop.c tests/unit/test_agent_events.c cmake/AegisTests.cmake
git commit -m "feat(agent): add observer event callback to agent loop"
```

---

## Chunk 2: Coding Agent 透传

### Task 3: 失败测试 + 实现

**Files:**
- Modify: `tests/unit/test_coding_agent.c`、`include/aegis/coding/coding_agent.h`、`src/coding/coding_agent.c`

- [ ] **Step 1: test_coding_agent.c 追加用例**：注册 ev_cb 记录事件 → `set_event_callback(agent, ev_cb, &log)` → run → 断言 log 非空且首事件为 TEXT_DELTA 或 TOOL_START → `set_event_callback(agent, NULL, NULL)` 后再 run 不再记录

- [ ] **Step 2: coding_agent.h 声明**

```c
aegis_status_t aegis_coding_agent_set_event_callback(aegis_coding_agent_t* agent,
                                                     aegis_agent_event_fn  fn,
                                                     void*                 user);
```

- [ ] **Step 3: coding_agent.c 实现**：struct 加 `aegis_agent_event_fn ev_fn; void* ev_user;`；`set_event_callback` 校验后先存字段再调 `aegis_agent_loop_set_event_callback(a->loop, fn, user)`；`replace_session` 与 `set_model` 建 loop 的 config 中带 `on_event=a->ev_fn, event_user=a->ev_user`；create 建 loop 同样带入（初始 NULL）

- [ ] **Step 4: 验证 + Commit**

Run: `ctest --test-dir build -R unit_coding_agent --output-on-failure` → PASS

```bash
git add include/aegis/coding/coding_agent.h src/coding/coding_agent.c tests/unit/test_coding_agent.c
git commit -m "feat(coding): expose agent event callback registration"
```

---

## Chunk 3: CLI 呈现

### Task 4: cli_interactive.c 打印回调与去重

**Files:**
- Modify: `apps/aegis/cli_interactive.c`

- [ ] **Step 1: 回调上下文与打印函数**（文件内 static）

```c
typedef struct cli_stream_ctx {
    bool enabled;       /* /stream on|off */
    bool text_emitted;  /* 本轮是否已输出流式文本 */
    bool line_open;     /* 当前行是否有未换行内容 */
} cli_stream_ctx_t;

static void cli_stream_prelude(cli_stream_ctx_t* cx)
{
    if (cx->line_open) { putchar('\n'); cx->line_open = false; }
}

static void cli_event_cb(const aegis_agent_event_t* ev, void* user)
{
    cli_stream_ctx_t* cx = (cli_stream_ctx_t*)user;
    if (!cx->enabled) return;
    switch (ev->type) {
    case AEGIS_AGENT_EVENT_TEXT_DELTA:
        if (ev->data && ev->len) {
            fwrite(ev->data, 1, ev->len, stdout);
            cx->text_emitted = true;
            cx->line_open = true;
            fflush(stdout);
        }
        break;
    case AEGIS_AGENT_EVENT_TOOL_START:
        cli_stream_prelude(cx);
        printf("● %s", ev->tool_name ? ev->tool_name : "?");
        cx->line_open = true;
        fflush(stdout);
        break;
    case AEGIS_AGENT_EVENT_TOOL_END:
        if (ev->status == AEGIS_OK) {
            /* 结果首行预览 ≤60 字符 */
            char preview[64] = {0};
            if (ev->data && ev->len) {
                const char* s = (const char*)ev->data;
                size_t take = ev->len < 60 ? ev->len : 60;
                memcpy(preview, s, take);
                for (size_t i = 0; i < take; i++) if (preview[i]=='\n') preview[i]=' ';
            }
            printf("  ✓ %s\n", preview[0] ? preview : "ok");
        } else {
            printf("  ✗ %s\n", aegis_status_str(ev->status));
        }
        cx->line_open = false;
        fflush(stdout);
        break;
    default: break;
    }
}
```

- [ ] **Step 2: cmd_interactive 接线**：`static cli_stream_ctx_t stream_ctx = {.enabled = true, ...}`（函数内 static 或文件级；每次 run 前 reset `text_emitted=false`）；`aegis_coding_agent_set_event_callback(agent, cli_event_cb, &stream_ctx)` 在 agent 创建成功后调用
- [ ] **Step 3: /stream 开关**：`/stream` 切换布尔并打印 `stream on/off`
- [ ] **Step 4: 去重**：`aegis_coding_agent_run` 返回 OK 时：

```c
if (!stream_ctx.text_emitted && !json_mode) { /* 现有 last-message 打印路径 */ }
/* json_mode 永远走现有路径 */
```

- [ ] **Step 5: /help 加 /stream**
- [ ] **Step 6: 编译验证**：`cmake --build build -j` 0 error

### Task 5: CLI 集成测试

**Files:**
- Modify: `tests/integration/test_cli.c`

- [ ] **Step 1: test_interactive_commands 追加**

```c
/* mock 模型下 prompt 会得到回复文本 */
/* /stream on 显式确保开启 */
run: "/stream on\nhello\n/quit\n"
  → 输出含 "mock stream for:"（token 流）且该文本只出现一次（去重生效）
run: "/stream off\nhello again\n/quit\n"
  → 输出仍恰有一次回复文本（走 last-message 路径），无 "stream: on" 残留干扰
```

断言要点：`strstr(out,"mock stream for:")` 首次出现位置 == 最后一次出现位置（唯一性）；`/stream` 命令回显 on/off。

- [ ] **Step 2: 运行**

Run: `ctest --test-dir build -R integration_cli --output-on-failure` → PASS
- [ ] **Step 3: Commit**

```bash
git add apps/aegis/cli_interactive.c tests/integration/test_cli.c
git commit -m "feat(cli): stream tokens and tool events live in interactive mode"
```

---

## Chunk 4: 全量验收

### Task 6: 回归与消毒

- [ ] **Step 1: 全量测试** `ctest --test-dir build --output-on-failure` → 全过（61+）
- [ ] **Step 2: ASan** `ctest --test-dir build-asan --output-on-failure` → 全过 0 泄漏
- [ ] **Step 3: 确认 git status 无计划外文件；无关脏文件不提交**

---

## 验收标准

- [ ] 交互模式逐 token 实时显示文本；工具调用显示 `● name` 与 `✓/✗` 结果行
- [ ] 同一回复文本不重复输出；JSON 与 /print 模式不变
- [ ] 回调 NULL / /stream off 时行为与现状一致
- [ ] 全量 ctest + ASan 通过，0 编译警告
