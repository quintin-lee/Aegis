# Pi 风格核心可用性设计（发现工具 + CLI 命令 + 模型切换）

- 日期: 2026-09-02
- 状态: Approved（用户已确认设计，待实现）
- 前置: `docs/architecture/overview.md` v2.0（Session/Message/Streaming 架构）

## 1. 背景与目标

Aegis Coding Agent 目前具备：

- 结构化 OpenAI 后端（SSE 流式 + 工具调用循环，`src/agent/loop.c`）
- 四个编码工具 `read/write/edit/bash`（路径约束、原子写、进程组取消，`src/coding/coding_tools.c`）
- JSONL 会话持久化：fork / compact / resume（`src/session/`）

对标 pi agent 的核心可用性差距：

1. **工具集不完整**：无 `glob`/`grep`/`list`，模型无法发现文件、搜索内容，面对陌生代码库基本不可用
2. **CLI 承诺未兑现**：`/help` 列出 `/model /session /sessions /resume`，但 `cli_interactive.c` 未实现任何一条
3. **Banner 失真**：硬编码 `model: mock`，不反映真实配置

## 2. 范围

**In：**
- 新增 `src/coding/discovery_tools.c` + 公共头 `include/aegis/coding/discovery_tools.h`
- `coding_agent` 新增模型访问/切换公共 API
- CLI 实现 `/model` `/session` `/sessions` `/resume`，更新 `/help` 与 banner
- 单元测试 + CLI 集成测试

**Out（后续迭代）：**
- CLI 流式输出 / thinking 展示（Live UX）
- Anthropic 等多 provider（Multi-provider）
- 工具审批流（approval flow）

## 3. 发现工具模块

新文件 `src/coding/discovery_tools.c`，不扩充已 570 行的 `coding_tools.c`。三个工具，完全沿用现有 tool-def 模式（静态 schema、字符串错误结果、capability 掩码）：

### 3.1 `list`

- 参数：`path`（可选，默认 `.`）
- 输出：每行 `name\t<dir|file>\t<size>`，目录优先排序
- 实现：`opendir/readdir`（`dirent` 已在 `src/skill/loader.c` 使用，无新依赖）

### 3.2 `glob`

- 参数：`pattern`（必填，如 `*.c`）、`path`（可选根目录）
- 语义：`fnmatch(3)` 对文件名匹配，**递归**遍历（等价 `**` 语义），返回相对 `path` 的路径列表
- 限制：递归深度上限 32；结果上限 200 条，超限追加 `... truncated`

### 3.3 `grep`

- 参数：`pattern`（必填，POSIX ERE）、`path`（可选，文件或目录）、`include`（可选，fnmatch 文件名过滤，如 `*.c`）
- 语义：`regcomp(REG_EXTENDED)` 编译，逐文件按行 `regexec`，输出 `path:line: text`
- 限制：结果上限 200 行 / 64KB，超限追加 `... truncated`；二进制文件跳过（首 1KB NUL 探测，与 `read` 一致）

### 3.4 共享安全与工程约束

- **路径约束**：将 `coding_tools.c` 中的静态 `safe_relative_path()` 提取为共享内部辅助（`src/internal/` 或 coding 模块内部头），两个模块共用同一实现；拒绝绝对路径与 `..` 逃逸
- **遍历排除**：`.git/` 目录始终跳过
- **取消**：三个工具在每个文件处理间隙轮询 cancellation token，置位即返回 `AEGIS_ERR_CANCELLED`
- **错误约定**：与 `read` 一致——单文件级错误（无法打开等）以内联 `error: ...` 字符串进结果；仅内部失败（NOMEM、regcomp 失败等）返回真实状态码

### 3.5 注册

`aegis_coding_tools_register_all()` 末尾追加调用 `aegis_coding_discovery_tools_register_all(reg)`。coding agent、autonomous 管线、既有测试零改动即获得新工具。

## 4. Coding Agent 模型切换 API

现状：`provider/api_key/base_url` 仅在 create 时使用一次。改造：

- agent 结构体持有 `provider/api_key/base_url` 的深拷贝
- 新增公共 API（`include/aegis/coding/coding_agent.h`）：

```c
const char*     aegis_coding_agent_model_name(const aegis_coding_agent_t* agent);
aegis_status_t  aegis_coding_agent_set_model(aegis_coding_agent_t* agent, const char* model);
```

- `set_model` 语义：按已存 provider 配置重建 model client（OpenAI 后端或 mock），随后用与 `replace_session` 相同的 swap 模式重建 agent loop（先建新 → 原子换入 → 销毁旧）；会话内容不动，下一轮生效
- 失败路径：新 client/loop 任一创建失败则原样保留现状并返回错误，agent 可继续使用

## 5. CLI 命令（`apps/aegis/cli_interactive.c`）

仅调用公共 API：

| 命令 | 行为 |
|------|------|
| `/model` | 打印当前模型名 |
| `/model <name>` | 调 `aegis_coding_agent_set_model`，成功/失败均有明确输出 |
| `/session` | 打印 id / branch / parent / 消息数（合并今日 `/tree` 的信息；`/tree` 保留为别名） |
| `/sessions` | 扫描 `.aegis/session-*.jsonl`，列文件名 + mtime |
| `/resume <path>` | `aegis_session_load` → `replace_session`；先加载后替换，失败保留当前会话 |
| `/help` | 与实际实现对齐 |

Banner 打印真实模型名。

## 6. 错误处理

- 不新增状态码；`AEGIS_ERR_INVALID/NOMEM/...` 显式传播
- `/resume` 失败时当前会话不受影响（load 成功后才 swap）
- `/model <name>` 切换失败时旧 model client 保持有效

## 7. 测试计划

- **单元**：`tests/unit/test_discovery_tools.c`——通过注册表 + `aegis_tool_call()` 驱动，覆盖：输出格式、glob 匹配与深度/数量上限、grep 行号 + 二进制跳过 + 截断、路径逃逸拒绝、取消
- **集成**：扩展 `integration_cli`——`/model` 切换（mock 后端）、`/sessions` 列表、`/resume` 已保存会话、`/session` 输出
- **回归**：全量 `ctest` 通过；ASan 构建零告警

## 8. 验收标准

- [ ] `glob`/`grep`/`list` 经注册表可调用，路径逃逸被拒绝，截断/取消语义生效
- [ ] CLI 四条命令按 §5 行为工作，`/help` 与 banner 与实际一致
- [ ] `aegis_coding_agent_set_model` 切换后下一轮使用新模型，失败不破坏现状
- [ ] 全量测试 + ASan 通过，无编译警告
