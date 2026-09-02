# CLI 流式输出（Live UX）设计

- 日期: 2026-09-02
- 状态: Approved（用户已确认设计）
- 前置: `docs/superpowers/specs/2026-09-02-pi-style-core-usability-design.md`（已交付）

## 1. 背景与目标

CLI 目前等 `aegis_coding_agent_run()` 整体返回后才打印会话末条消息：

- assistant 文本无逐 token 实时输出
- 工具调用对用户完全不可见（黑盒等待）
- 与 pi agent 的核心体验差距

目标：CLI 交互模式实时呈现文本 token 与工具调用/结果，机器可读模式（JSON、/print）输出不变。

## 2. 范围

**In：**
- agent loop 公共 API 新增类型化事件回调（观察者）
- coding agent 透传回调注册
- CLI 交互模式注册打印回调、`/stream on|off` 开关、流式优先去重

**Out（后续）：**
- reasoning delta 展示、usage 统计展示
- TUI（全屏界面）、多行重绘
- RPC/嵌入式层的事件消费示例

## 3. Agent Loop 事件回调（`include/aegis/agent/loop.h`）

```c
typedef enum aegis_agent_event_type {
    AEGIS_AGENT_EVENT_TEXT_DELTA = 0, /**< data/len: borrowed text fragment */
    AEGIS_AGENT_EVENT_TOOL_START,     /**< tool_name/call_id set */
    AEGIS_AGENT_EVENT_TOOL_END,       /**< tool_name/call_id set; status = tool
                                           result status; data/len = borrowed
                                           result string preview when ok */
} aegis_agent_event_type_t;

typedef struct aegis_agent_event {
    aegis_agent_event_type_t type;
    const void*              data;      /**< Borrowed; valid only during callback */
    size_t                   len;
    const char*              tool_name; /**< TOOL_* only */
    const char*              call_id;   /**< TOOL_* only */
    aegis_status_t           status;    /**< TOOL_END: tool outcome */
} aegis_agent_event_t;

typedef void (*aegis_agent_event_fn)(const aegis_agent_event_t* ev, void* user);
```

- `aegis_agent_loop_config_t` 新增 `aegis_agent_event_fn on_event; void* event_user;`（均可 NULL；NULL 时行为与现状完全一致）
- 语义：**观察者，非控制流**——`void` 返回，回调失败不改变 loop 行为；事件数据仅在回调期间有效
- **不持锁调用**（遵守 `docs/architecture/overview.md` §3 no-callback-under-lock）
- 触发点：
  - `stream_cb` 内 `TEXT_DELTA`（累积后转发，借用 acc 缓冲区间）
  - 工具执行（`WAITING_TOOL` 循环）：执行前 `TOOL_START`；`aegis_tool_execute` 返回并写入 tool message 后 `TOOL_END`（status = 工具状态；成功且结果为字符串时 data = 结果内容）
- 结构体新增 `on_event/event_user` 字段，create 时从 config 拷贝

## 4. Coding Agent 透传（`include/aegis/coding/coding_agent.h`）

```c
aegis_status_t aegis_coding_agent_set_event_callback(aegis_coding_agent_t* agent,
                                                     aegis_agent_event_fn  fn,
                                                     void*                 user);
```

- agent 结构体存 `fn/user`（borrowed）；`set_event_callback` 在**已存在**的 loop 上即时生效：内部调用新增的 `aegis_agent_loop_set_event_callback(loop, fn, user)`
- create / `replace_session` / `set_model` 建 loop 时写入 config（fn 默认 NULL）
- loop.h 同步暴露 `aegis_agent_loop_set_event_callback()`，支持运行期换绑

## 5. CLI 呈现（`apps/aegis/cli_interactive.c`）

- 交互模式默认注册回调；`/print` 与 JSON 模式不注册（机器可读输出不变）
- 回调内 `bool stream_enabled`（CLI 侧静态状态）为 false 时 early-return——`/stream on|off` 只翻转此布尔，不动 loop
- 呈现规则（简洁风格）：
  - `TEXT_DELTA` → `fwrite(data,1,len,stdout)` + `fflush`
  - `TOOL_START` → 若本轮尚未输出换行先补 `\n`；打印 `● <tool_name>(<args 摘要 ≤40 字符或 description>)` + fflush
  - `TOOL_END` → 打印 `  ✓ ok` 或 `  ✗ <aegis_status_str>`，成功且结果字符串时追加首行预览（≤60 字符）+ `\n`
  - 回调上下文持有 `text_emitted` / `line_open` 状态（user 指针指向 CLI 静态结构）
- **去重（流式优先）**：`run` 返回 OK 且 `text_emitted == true` 时，不再重复打印 last message；否则走现有打印路径。`/print` 与 JSON 模式永远走现有路径

## 6. 错误处理与降级

- 回调未注册 / `stream_enabled=false` / 非 TTY：行为与现状逐字节一致
- 回调 `void` 返回，不向 loop 传播错误（打印本身几乎不会失败；失败也不影响 loop 语义）
- loop 现有取消/错误/状态机路径零改动

## 7. 测试

- **单元**（`tests/unit/test_agent_events.c`，fixture 模型后端同 `test_coding_loop.c` 模式）：
  - 事件序列断言：text delta → TOOL_START(name/call_id) → TOOL_END(ok, 带结果) → text delta → END
  - 工具失败时 TOOL_END.status 非 OK
  - 未注册回调时既有行为回归（对照 `system_coding_loop` 既有断言）
  - 运行期 `aegis_agent_loop_set_event_callback` 换绑生效
- **集成**（扩展 `tests/integration/test_cli.c`）：
  - 交互 stdin 用例：输出含 `●`、`✓` 标记；同一文本不出现两次（流式优先去重）
  - `/stream off` 后输出不含标记
- 全量 ctest + ASan 通过

## 8. 验收标准

- [ ] 交互模式逐 token 实时显示 assistant 文本
- [ ] 工具调用显示 `● name(...)` 与 `✓/✗` 结果行
- [ ] 同一回复文本不重复输出；JSON 与 /print 模式输出与现状一致
- [ ] 回调 NULL / stream off 时行为与现状完全一致
- [ ] 全量测试 + ASan 通过，0 编译警告
