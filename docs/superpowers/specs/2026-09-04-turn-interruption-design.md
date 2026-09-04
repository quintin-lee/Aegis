# Turn Interruption（Esc 中断 · Enter 触发）设计

Date: 2026-09-04
Status: Approved

## Goal

pi-agent 风格的交互中断：CLI 用户可以在一个 agent turn 进行中打断它
（Claude Code 风格的 "press Enter to interrupt"），中断立即生效于模型流
（in-flight HTTP 请求），已流出的 partial 回复保留进会话并带中断标记，
REPL 继续可用。

## User-facing behavior

- Turn 进行中（模型流式输出 / 工具执行）：
  - 按空 **Enter** → 立即中断当前 turn；打印 `⏹ interrupted`
  - 输入**有内容的行** + Enter → 该行排队，当前 turn 结束后依次执行
- 空闲时：空行忽略，重新显示提示符
- 中断后 REPL 完全可用；下一个 turn 不受影响（每轮全新 token）

## Design

### 1. Agent loop — partial 保留 + token 重绑

`include/aegis/agent/loop.h` / `src/agent/loop.c`:

- 新增 `aegis_agent_loop_set_token(aegis_agent_loop_t*, aegis_cancellation_token_t*)`
  — borrowed 指针，运行期重绑（同 `set_event_callback` 模式）。
- `run_turn` 模型流返回 `AEGIS_ERR_CANCELLED` 时，不再丢弃 `acc`：
  - 把 `acc.text`（可能为空串）作为 assistant 消息 append 进会话
    （reasoning、tool_calls 同样保留，走既有 append 路径）；
  - 再 append 一条 user 消息 `"[interrupted by user]"` 作为中断标记，
    下一轮 build_context 时模型能看到自己说到哪、被打断；
  - `set_state(AEGIS_AGENT_LOOP_CANCELLED)`，返回 `AEGIS_ERR_CANCELLED`。
- 工具执行后的取消检查路径已正确（追加 tool result 后返回 CANCELLED），不动。

### 2. Coding agent — interrupt() + 每轮新 token

`include/aegis/coding/coding_agent.h` / `src/coding/coding_agent.c`:

- agent 结构体持有 `aegis_cancellation_token_t* token`（owned）。
- create / replace_session / set_model 三处构建 loop config 时都传
  `lcfg.token = a->token`。
- `aegis_coding_agent_run()`：每轮开始时 destroy 旧 token → create 新
  token（cancellation.h 无 reset API，重建即复位）→
  `aegis_agent_loop_set_token` → run_turn。
- 新 API `aegis_coding_agent_interrupt(const aegis_coding_agent_t*)` →
  `aegis_agent_loop_cancel(a->loop)`。token 的 `request_cancel` 是
  lock-free，可从任意线程调用。

### 3. OpenAI provider — curl 即时中断

`providers/llm/openai/structured_openai.c`（stream 与 complete 两条路径）:

- `CURLOPT_NOPROGRESS = 0L` + `CURLOPT_XFERINFOFUNCTION`：进度回调里
  `aegis_cancellation_token_is_cancelled(state.token)` 为真时返回非零，
  curl 以 `CURLE_ABORTED_BY_CALLBACK` 中止传输。
- `curl_easy_perform` 返回后先查 token：已取消 → 返回
  `AEGIS_ERR_CANCELLED`（优先于 `cr != CURLE_OK` / HTTP 状态判断）。

### 4. CLI — reader 线程 + Enter 中断 + 排队

`apps/aegis/cli_interactive.c`:

- `cmd_interactive` 启动一个 pthread reader：阻塞 `fgets(stdin)`，把每行
  放入互斥锁保护的行队列（mutex + cond），EOF 时放入 `NULL` 哨兵。
- 主循环从队列取行：
  - **turn 进行中**：空行 → `aegis_coding_agent_interrupt(agent)`；
    有内容的行 → 入 pending 队列。
  - **空闲时**：空行忽略；有内容的行作为用户输入执行；`NULL` 哨兵 → 退出。
  - turn 结束后，pending 队列的行依次作为后续输入执行（每行一轮完整
    REPL 流程：命令解析 / agent run）。
- `run` 返回 `AEGIS_ERR_CANCELLED` → 打印 `⏹ interrupted`（不视为错误）。
- reader 线程只在进程退出前 join；CLI 二进制链接 pthread。
- 非交互路径（`cmd_run`、`/quit`、脚本驱动）行为不变。

### 5. CMake

- `CMakeLists.txt`：`aegis` target 链接 `pthread`。

## Testing

- **unit / agent loop**（`tests/unit/test_agent_events.c`）：fixture 模型流
  回调中触发 `aegis_agent_loop_cancel` → 断言 run 返回 `AEGIS_ERR_CANCELLED`、
  会话里 partial assistant 消息已 append（含已流式部分）、其后是
  `"[interrupted by user]"` user 消息、loop state == CANCELLED；
  `set_token` 重绑后旧 token 不再影响新 turn。
- **unit / coding agent**（`tests/unit/test_coding_agent.c`）：事件回调里
  `interrupt()` → run 返回 CANCELLED；**再次 run 正常完成**（新 token 验证）。
- **integration / CLI**（`tests/integration/test_cli.c`）：慢速 SSE fixture
  （delta 之间 sleep）真实驱动二进制：
  - 中断场景：发 `hello`，延迟发空行 → 输出含 `⏹ interrupted`，
    随后新输入正常得到回复；
  - 排队场景：turn 进行中连发两行内容 → 两轮都各自执行。
- 全量 ctest（普通 + ASan）绿。

## Non-goals

- 真 Esc 键 raw-mode 检测（需要自管行编辑；留待以后）
- `/stop` 命令（Enter 方案已覆盖；若需要可后续加）
- 中断跨 agent 实例 / 取消进行中的 shell 工具子进程
  （`bash` 工具已通过 token 轮询协作取消，天然受益）
