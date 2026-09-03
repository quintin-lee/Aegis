# 设计：思考过程（Reasoning）实时呈现

日期：2026-09-03
状态：已批准（用户确认）

## 背景与目标

pi-agent 风格的 CLI 需要把模型的思考过程（reasoning/thinking）实时呈现出来。当前基础设施：

- `include/aegis/model/stream.h` 已定义 `AEGIS_MODEL_STREAM_REASONING_DELTA` 事件类型，但**无生产者、无消费者**
- `aegis_message_set_reasoning()/reasoning()` 已存在于 message 层，但从未被写入
- 会话 JSONL 不序列化 reasoning
- CLI 具备流式呈现与去重机制（上一迭代），但不认识 reasoning

本设计将 reasoning 路径端到端打通：provider 解析 → loop 累积/转发 → CLI 呈现 → 会话持久化。

## 决策记录

| 决策点 | 结论 |
|---|---|
| 线格式 | 同时支持 `reasoning_content`（DeepSeek/Qwen 风格）与 `reasoning`（OpenRouter 风格）两个 JSON 字段；不做 `<think>` 标签解析 |
| 持久化 | reasoning 写入会话 JSONL（仅非空时），mock 模型也发 reasoning delta 便于测试 |
| 呈现风格 | 暗色斜体流式（`\033[2m\033[3m`），块结束补 `\033[0m` + 换行；`/stream off` 时不输出 reasoning |
| /stream off 行为 | reasoning 不打印；最终回复走旧路径 |

## 1. Agent Loop — 累积 + 转发

- `include/aegis/agent/loop.h`：新增 `AEGIS_AGENT_EVENT_REASONING_DELTA = 3`（payload 与 TEXT_DELTA 相同：borrowed `data/len`）。
- `src/agent/loop.c` `stream_accum_t`：新增 `reasoning` 缓冲；`stream_cb` 处理 `AEGIS_MODEL_STREAM_REASONING_DELTA` — 累积并转发 agent 事件给 `on_event` 观察者（与 TEXT_DELTA 相同的无锁、void 返回语义）。
- 组装处（`acc.text` → content 的位置）：非空时 `aegis_message_set_reasoning(am, acc.reasoning)` — 即使该轮以工具调用结束，reasoning 也挂到 assistant 消息上。

## 2. OpenAI Provider — 发射事件

`emit_record`（SSE 解析）检测 JSON 字段：

- `reasoning_content`（DeepSeek/Qwen 风格）
- `reasoning`（OpenRouter 风格）

命中则发 `AEGIS_MODEL_STREAM_REASONING_DELTA`（解码后字符串）。

子串安全已验证：`"content"` 与 `"reasoning"` 的 strstr 不会在 `"reasoning_content"` 内误命中（`"content"` 前有 `_` 不构成键；`"reasoning"` 匹配到的是 `"reasoning_content"` 的前缀——需先匹配长键再匹配短键，或用带引号的键 `"reasoning"` 精确匹配后确认后续字符是 `:`）。

两字段同时出现（罕见）时 `reasoning_content` 优先，避免双发。

## 3. Mock 模型 — 发射 reasoning

`src/model/model.c` mock stream 在文本块前发 1–2 个 `REASONING_DELTA` 块。这样所有既有测试设施（agent events、coding loop、CLI 集成）自动覆盖完整路径，无需新 fixture。

## 4. 会话 JSONL 持久化

- 保存（`aegis_session_save`）：message 行在 `content` 后追加 `"reasoning":"…"`（仅非 NULL 时）。
- 加载（`aegis_session_load`）：与 content 相同方式解析（复用转义还原逻辑），`aegis_message_set_reasoning` 恢复。
- 效果：resume 后 `/print` 与后续上下文构建可见 thinking。

## 5. CLI 呈现 — 暗色斜体流式

`cli_stream_ctx_t` 新增 `reasoning_open` 状态：

- `REASONING_DELTA`：若有未闭合行先补 `\n`；打印 `\033[2m\033[3m`（dim+italic），`fwrite` 块 + `fflush`。
- reasoning 后首个 `TEXT_DELTA` / `TOOL_START`：打印 `\033[0m` + `\n` 闭合块，与正文自然衔接。
- 去重不变：reasoning **不**置位 `text_emitted` — 纯 reasoning 轮次的最终回复仍走旧路径打印一次。
- `/stream off`：回调内 early-return，不输出 reasoning。

## 6. 测试（TDD 顺序）

1. **单元** `test_agent_events.c`：fixture 后端先发 REASONING 再发 TEXT → 断言转发事件序列 + 会话消息 `aegis_message_reasoning()` 非空。
2. **系统** `test_openai_sse_e2e.c`：两个新 SSE 场景 — `reasoning_content` 记录与 `reasoning` 记录 → 断言 REASONING_DELTA 事件。
3. **单元** `test_session.c`：save/load 往返保留 reasoning。
4. **CLI 集成** `test_cli.c`：mock 现在发 reasoning → 断言暗色样式思考出现一次、最终消息不重复。
5. 全量 ctest + ASan 绿。

## 7. 错误处理与降级

- 回调未注册 / 非 TTY / JSON 模式：与现状一致，reasoning 不输出。
- reasoning 缓冲分配失败：与 text 累积同路径（`AEGIS_ERR_NOMEM` 使 turn 失败）。
- 会话加载遇到无 `reasoning` 字段的旧行：保持 NULL，完全向后兼容。
