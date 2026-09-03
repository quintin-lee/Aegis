# 设计：工具执行前审批（Per-tool Approval）

日期：2026-09-03
状态：已批准（用户确认）

## 背景与目标

pi 风格 agent 在执行工具前给用户确认机会。当前 `loop.c` 在 `WAITING_TOOL` 阶段直接调用 `aegis_tool_execute`，无任何拦截点。本设计新增审批钩子（控制流回调），CLI 提供交互式 y/n/a 确认与 `/approvals` 开关。

## 决策记录

| 决策点 | 结论 |
|---|---|
| 交互方式 | 每次工具调用前提示参数，用户输入 y/n/a；`a` = 本次会话总是允许该工具 |
| 拒绝语义 | 拒绝信息作为工具结果返回给模型（"user denied tool <name>"），turn 继续，不中断会话 |
| 名单存储 | CLI 进程内存（不持久化，重启重置）；开关 `/approvals on|off`，交互模式默认 off |
| 默认行为 | 回调未注册或开关 off：与现状完全一致（放行） |

## 1. Agent Loop — 审批钩子

`include/aegis/agent/loop.h` 新增：

```c
typedef enum aegis_tool_approval {
    AEGIS_TOOL_APPROVAL_ALLOW = 0,
    AEGIS_TOOL_APPROVAL_DENY  = 1,
} aegis_tool_approval_t;

/* 控制流回调（非观察者）：工具执行前同步调用；返回值决定是否执行。 */
typedef aegis_tool_approval_t (*aegis_tool_approval_fn)(
    const char* tool_name, const char* arguments_json, void* user);
```

- `aegis_agent_loop_config_t` 增 `tool_approval` / `approval_user`（可 NULL）
- `loop.c` 在 TOOL_START 事件**之前**调用；DENY 时跳过 `aegis_tool_execute`，工具结果消息内容为 `user denied tool <name>`，照常写 session、返回给模型
- 新增 `aegis_agent_loop_set_tool_approval(loop, fn, user)` 运行时重绑（同 `set_event_callback` 模式，`AEGIS_ERR_INVALID` 校验）
- 不持锁调用（工具执行点本就无锁）

## 2. Coding Agent 透传

`aegis_coding_agent_set_tool_approval(agent, fn, user)`：存于 agent 结构体；create 与 `set_model` 重建 loop 时写入 loop config（同 event callback 模式）。

## 3. CLI 呈现与交互

`cli_stream_ctx_t` 扩展：

- `bool approvals`（默认 off）
- `char allowed_tools[MAX_ALLOWED][64]` + `size_t allowed_count`（MAX_ALLOWED=16，超出后 a 退化为 y）

approval 回调实现（`approval_cb`）：

1. `approvals == off` → ALLOW
2. 工具名在 `allowed_tools` → ALLOW
3. 否则：结束当前未闭合行（prelude + reasoning 闭合），打印
   `approve <name> <args-json>? [y/n/a] ` + `fflush`，`fgets` stdin
   - `y` → ALLOW；`a` → 名单登记 + ALLOW；`n`/EOF/其他 → DENY
4. DENY 后 TOOL_END 事件照常发出，CLI 打 `✗ user denied`（status 非.OK 时的现有路径；denial 结果字符串首行预览即文案）

命令：

- `/approvals` — 显示当前状态与名单
- `/approvals on|off` — 切换（on 即注册回调，off 注销；通过运行时 setter，不动 loop）
- `/help` 更新

## 4. 测试（TDD 顺序）

1. **单元** `test_agent_events.c`：approval 回调三态 —— ALLOW 执行正常；DENY 时 execute 未被调用（probe 计数不变）、工具结果内容 `user denied tool read`、turn 返回 OK；`set_tool_approval` 运行时重绑与 NULL 校验。
2. **集成** `integration_cli` 新场景：SSE fixture 两轮工具调用；stdin 预填 `n`、`a`：
   - 断言提示 `approve read` 出现
   - 拒绝后最终回复仍到达（denial 进了上下文）
   - `a` 之后第二次同工具调用不再提示
3. 全量 ctest + ASan 绿。

## 5. 错误处理

- 回调失败不存在（枚举返回）；EOF 视为 DENY
- 名单满（16 个）：`a` 行为等同 `y`
- 回调在重建 loop（`set_model`）后自动恢复（coding agent 持有 fn/user）
