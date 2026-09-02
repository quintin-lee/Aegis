# Pi 风格核心可用性实现计划

> **For agentic workers:** REQUIRED: Use superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 为 Aegis Coding Agent 补齐 pi agent 式核心可用性：`list/glob/grep` 三个发现工具、coding agent 模型热切换 API、CLI 四条会话命令。

**Architecture:** 新增 `src/coding/discovery_tools.c`（复用现有 tool-def 模式，经 `aegis_coding_tools_register_all` 自动注册）；提取共享路径安全辅助 `src/coding/path_safety.h`；`coding_agent` 深拷贝 provider 配置并新增 `model_name/set_model` 公共 API（swap 模式重建 loop）；`cli_interactive.c` 仅经公共 API 实现命令。

**Tech Stack:** C11, dirent/fnmatch/regex.h (POSIX), CMake/CTest, 既有 aegis_tool/session/coding API。

**Spec:** `docs/superpowers/specs/2026-09-02-pi-style-core-usability-design.md`

**注意:** 工作区已有两处与本计划无关的未提交改动（`src/autonomous/execution.c`、`tests/system/test_autonomous_closed_loop.c`）。所有 `git add` 必须精确指定文件，不得 `git add -A`。

---

## File Map

| 文件 | 操作 | 职责 |
|------|------|------|
| `src/coding/path_safety.h` | 新建 | 模块内共享 `aegis_safe_relative_path()`（自 coding_tools.c 提取） |
| `include/aegis/coding/discovery_tools.h` | 新建 | list/glob/grep 工具定义 + 注册函数 |
| `src/coding/discovery_tools.c` | 新建 | 三个发现工具实现（walk_tree + 访问者） |
| `src/coding/coding_tools.c` | 修改 | 删除本地 safe_relative_path，改用共享头；register_all 追加 discovery 注册 |
| `src/coding/CMakeLists.txt` | 修改 | aegis_coding 源列表追加 discovery_tools.c |
| `include/aegis/coding/coding_agent.h` | 修改 | 新增 `model_name` / `set_model` 声明 |
| `src/coding/coding_agent.c` | 修改 | 深拷贝 provider 配置；实现两 API；destroy 释放 |
| `apps/aegis/cli_interactive.c` | 修改 | /help、banner、/model、/session、/sessions、/resume |
| `tests/unit/test_discovery_tools.c` | 新建 | 发现工具单元测试 |
| `tests/unit/test_coding_agent.c` | 新建 | coding agent 模型 API 单元测试 |
| `tests/integration/test_cli.c` | 修改 | run_cli_stdin + 交互命令集成测试 |
| `cmake/AegisTests.cmake` | 修改 | 注册两个新单测 + ASan 属性列表 |

---

## Chunk 1: 发现工具（list/glob/grep）

### Task 1: 提取共享路径安全辅助

**Files:**
- Create: `src/coding/path_safety.h`
- Modify: `src/coding/coding_tools.c`

- [ ] **Step 1: 新建 `src/coding/path_safety.h`**

```c
#ifndef AEGIS_CODING_PATH_SAFETY_H
#define AEGIS_CODING_PATH_SAFETY_H

#include <stdbool.h>

/**
 * @file path_safety.h
 * @brief Module-internal path containment shared by coding tools.
 *
 * Rejects absolute paths and any ".." path component so tool file access
 * stays inside the project root. Header-only; included by coding_tools.c
 * and discovery_tools.c only.
 */
static inline bool aegis_safe_relative_path(const char* path)
{
    if (!path || path[0] == '/' || path[0] == '\0') {
        return false;
    }
    const char* p = path;
    while (*p) {
        if ((p == path || p[-1] == '/') && p[0] == '.' && p[1] == '.' &&
            (p[2] == '\0' || p[2] == '/')) {
            return false;
        }
        ++p;
    }
    return true;
}

#endif /* AEGIS_CODING_PATH_SAFETY_H */
```

- [ ] **Step 2: `coding_tools.c` 删除本地 static `safe_relative_path`，追加 `#include "path_safety.h"`，全部调用点改名 `aegis_safe_relative_path`**（4 处：read/write/edit）

- [ ] **Step 3: 验证编译**

Run: `cmake --build build -j 2>&1 | tail -5`
Expected: 0 error / 0 warning

- [ ] **Step 4: Commit**（此步与 Task 3 合并提交亦可）

### Task 2: 发现工具失败测试（先写测试）

**Files:**
- Create: `tests/unit/test_discovery_tools.c`
- Modify: `cmake/AegisTests.cmake`

- [ ] **Step 1: 写测试**（风格对齐 `tests/unit/test_session.c`：assert + expect_ok，GTest main 由 gtest_main 提供）

```c
#define _POSIX_C_SOURCE 200809L
#include "aegis/coding/discovery_tools.h"
#include "aegis/common/cancellation/cancellation.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>

static void expect_ok(aegis_status_t rc, const char* msg)
{
    if (rc != AEGIS_OK) {
        fprintf(stderr, "FAIL %s: %d\n", msg, (int)rc);
        assert(0);
    }
}

/* fixture tree in mkdtemp:
 *   a.txt          "hello world\nsecond line\n"
 *   bin.dat        "abc\0def" (binary)
 *   sub/b.c        "int main() { return 0; }\n"
 *   sub/deep/c.h   "#pragma once\n"
 *   .git/x.txt     "ignored\n"
 */
static char* make_fixture(void)
{
    char tmpl[] = "/tmp/aegis_disc_XXXXXX";
    char* dir   = mkdtemp(tmpl);
    assert(dir);
    char path[512];
    snprintf(path, sizeof(path), "%s/sub/deep", dir);
    assert(mkdir(path, 0755) == 0 || errno == EEXIST);
    snprintf(path, sizeof(path), "%s/.git", dir);
    assert(mkdir(path, 0755) == 0 || errno == EEXIST);

    FILE* f = fopen((snprintf(path, sizeof(path), "%s/a.txt", dir), path), "w");
    assert(f); fputs("hello world\nsecond line\n", f); fclose(f);
    f = fopen((snprintf(path, sizeof(path), "%s/bin.dat", dir), path), "wb");
    assert(f); fwrite("abc\0def", 1, 7, f); fclose(f);
    f = fopen((snprintf(path, sizeof(path), "%s/sub/b.c", dir), path), "w");
    assert(f); fputs("int main() { return 0; }\n", f); fclose(f);
    f = fopen((snprintf(path, sizeof(path), "%s/sub/deep/c.h", dir), path), "w");
    assert(f); fputs("#pragma once\n", f); fclose(f);
    f = fopen((snprintf(path, sizeof(path), "%s/.git/x.txt", dir), path), "w");
    assert(f); fputs("ignored\n", f); fclose(f);
    return strdup(dir);
}

static aegis_status_t call_tool(aegis_tool_registry_t* reg, const char* name,
                                const char* key1, const char* val1,
                                const char* key2, const char* val2,
                                char** out_text)
{
    aegis_tool_args_t* args = NULL;
    aegis_status_t st = aegis_tool_args_create(&args);
    if (st != AEGIS_OK) return st;
    if (val1) aegis_tool_args_add_string(args, key1, val1);
    if (val2) aegis_tool_args_add_string(args, key2, val2);
    aegis_tool_result_t result = {0};
    st = aegis_tool_call(reg, name, args, 5000, &result);
    if (st == AEGIS_OK) {
        *out_text = strdup(result.value.type == AEGIS_TOOL_VAL_STRING && result.value.as.str.ptr
                               ? result.value.as.str.ptr : "");
    }
    aegis_tool_result_destroy(&result);
    aegis_tool_args_destroy(args);
    return st;
}

static void test_list(void)
{
    char* dir = make_fixture();
    char* cwdbuf = getcwd(NULL, 0);
    assert(chdir(dir) == 0);
    aegis_tool_registry_t* reg = NULL;
    expect_ok(aegis_tool_registry_create(&reg), "reg");
    expect_ok(aegis_coding_discovery_tools_register_all(reg), "register");

    char* out = NULL;
    expect_ok(call_tool(reg, "list", "path", ".", NULL, NULL, &out), "list .");
    assert(strstr(out, "a.txt\tfile\t"));
    assert(strstr(out, "sub\tdir\t"));
    assert(strstr(out, "bin.dat\tfile\t"));
    free(out);

    /* path traversal rejected */
    expect_ok(call_tool(reg, "list", "path", "../", NULL, NULL, &out), "list ..");
    assert(strstr(out, "error:"));
    free(out);

    aegis_tool_registry_destroy(reg);
    assert(chdir(cwdbuf) == 0);
    free(cwdbuf);
    char rm[1024]; snprintf(rm, sizeof(rm), "rm -rf %s", dir);
    (void)system(rm); free(dir);
    printf("list PASS\n");
}

static void test_glob(void)
{
    char* dir = make_fixture();
    char* cwdbuf = getcwd(NULL, 0);
    assert(chdir(dir) == 0);
    aegis_tool_registry_t* reg = NULL;
    expect_ok(aegis_tool_registry_create(&reg), "reg");
    expect_ok(aegis_coding_discovery_tools_register_all(reg), "register");

    char* out = NULL;
    expect_ok(call_tool(reg, "glob", "pattern", "*.c", NULL, NULL, &out), "glob *.c");
    assert(strstr(out, "sub/b.c"));
    assert(!strstr(out, ".git"));
    free(out);

    expect_ok(call_tool(reg, "glob", "pattern", "sub/deep/*.h", "path", ".", NULL, &out), "glob deep");
    assert(strstr(out, "sub/deep/c.h"));
    free(out);

    /* traversal root rejected */
    expect_ok(call_tool(reg, "glob", "pattern", "*.c", "path", "..", NULL, &out), "glob ..");
    assert(strstr(out, "error:"));
    free(out);

    aegis_tool_registry_destroy(reg);
    assert(chdir(cwdbuf) == 0); free(cwdbuf);
    char rm[1024]; snprintf(rm, sizeof(rm), "rm -rf %s", dir);
    (void)system(rm); free(dir);
    printf("glob PASS\n");
}

static void test_grep(void)
{
    char* dir = make_fixture();
    char* cwdbuf = getcwd(NULL, 0);
    assert(chdir(dir) == 0);
    aegis_tool_registry_t* reg = NULL;
    expect_ok(aegis_tool_registry_create(&reg), "reg");
    expect_ok(aegis_coding_discovery_tools_register_all(reg), "register");

    char* out = NULL;
    expect_ok(call_tool(reg, "grep", "pattern", "main", "include", "*.c", NULL, &out), "grep main");
    assert(strstr(out, "sub/b.c:1:"));
    free(out);

    /* binary file skipped, no NUL garbage */
    expect_ok(call_tool(reg, "grep", "pattern", "def", NULL, NULL, &out), "grep binary skip");
    assert(out[0] == '\0');
    free(out);

    /* invalid regex -> inline error */
    expect_ok(call_tool(reg, "grep", "pattern", "(", NULL, NULL, &out), "grep bad regex");
    assert(strstr(out, "error:"));
    free(out);

    /* traversal rejected */
    expect_ok(call_tool(reg, "grep", "pattern", "x", "path", "..", NULL, &out), "grep ..");
    assert(strstr(out, "error:"));
    free(out);

    /* cancelled token */
    aegis_cancellation_token_t* tok = NULL;
    expect_ok(aegis_cancellation_token_create(&tok), "tok");
    aegis_cancellation_token_request_cancel(tok);
    aegis_tool_args_t* args = NULL;
    expect_ok(aegis_tool_args_create(&args), "args");
    aegis_tool_args_add_string(args, "pattern", "x");
    aegis_tool_result_t result = {0};
    assert(aegis_tool_call(reg, "grep", args, 5000, &result) == AEGIS_ERR_CANCELLED);
    aegis_tool_result_destroy(&result);
    aegis_tool_args_destroy(args);
    aegis_cancellation_token_destroy(tok);

    aegis_tool_registry_destroy(reg);
    assert(chdir(cwdbuf) == 0); free(cwdbuf);
    char rm[1024]; snprintf(rm, sizeof(rm), "rm -rf %s", dir);
    (void)system(rm); free(dir);
    printf("grep PASS\n");
}

static void test_glob_truncation(void)
{
    char* dir = make_fixture();
    char path[512];
    for (int i = 0; i < 210; i++) {
        snprintf(path, sizeof(path), "%s/f%03d.txt", dir, i);
        FILE* f = fopen(path, "w");
        assert(f); fputs("x\n", f); fclose(f);
    }
    char* cwdbuf = getcwd(NULL, 0);
    assert(chdir(dir) == 0);
    aegis_tool_registry_t* reg = NULL;
    expect_ok(aegis_tool_registry_create(&reg), "reg");
    expect_ok(aegis_coding_discovery_tools_register_all(reg), "register");
    char* out = NULL;
    expect_ok(call_tool(reg, "glob", "pattern", "*.txt", NULL, NULL, &out), "glob many");
    assert(strstr(out, "truncated"));
    free(out);
    aegis_tool_registry_destroy(reg);
    assert(chdir(cwdbuf) == 0); free(cwdbuf);
    char rm[1024]; snprintf(rm, sizeof(rm), "rm -rf %s", dir);
    (void)system(rm); free(dir);
    printf("glob_truncation PASS\n");
}

int main(void)
{
    test_list();
    test_glob();
    test_grep();
    test_glob_truncation();
    printf("ALL_DISCOVERY_TESTS PASSED\n");
    return 0;
}
```

- [ ] **Step 2: AegisTests.cmake 注册**（`unit_tool_concurrent` 之后追加；并同步加入 ASan 属性列表）

```cmake
    aegis_add_test(unit_discovery_tools tests/unit/test_discovery_tools.c)
```

ASan `set_tests_properties(...)` 列表追加 `unit_discovery_tools`（及后续 `unit_coding_agent`）。

- [ ] **Step 3: 验证失败**

Run: `cmake --build build -j 2>&1 | tail -5`
Expected: 编译失败（`aegis/coding/discovery_tools.h` 不存在）

### Task 3: 实现发现工具

**Files:**
- Create: `include/aegis/coding/discovery_tools.h`
- Create: `src/coding/discovery_tools.c`
- Modify: `src/coding/CMakeLists.txt`、`src/coding/coding_tools.c`

- [ ] **Step 1: 公共头**

```c
#ifndef AEGIS_CODING_DISCOVERY_TOOLS_H
#define AEGIS_CODING_DISCOVERY_TOOLS_H

#include "aegis/tool/tool.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file discovery_tools.h
 * @brief Builtin discovery tools: list, glob, grep.
 *
 * All tools are project-root confined (relative paths only, no ".."),
 * skip .git directories during recursive walks, honor cooperative
 * cancellation between files, and cap results explicitly.
 */

extern const aegis_tool_def_t aegis_coding_tool_list;
extern const aegis_tool_def_t aegis_coding_tool_glob;
extern const aegis_tool_def_t aegis_coding_tool_grep;

aegis_status_t aegis_coding_discovery_tools_register_all(aegis_tool_registry_t* reg);

#ifdef __cplusplus
}
#endif

#endif /* AEGIS_CODING_DISCOVERY_TOOLS_H */
```

- [ ] **Step 2: 实现**。要点（完整代码见执行时编写，关键结构如下）：

```c
#define _POSIX_C_SOURCE 200809L
// 依赖: dirent.h, fnmatch.h, regex.h, sys/stat.h
// 常量: MAX_RESULTS 200, MAX_BYTES (64*1024), MAX_DEPTH 32, MAX_LINE 4096

// str_vec_t: growable char** (push=strdup, destroy free)
// out_buf_t: char* buf (grow*2), len, cap, count, truncated; append_line 封顶后只置 truncated
// walk_tree(root, rel, depth, visit, user, stop, token):
//   opendir(root/rel); 每项跳过 "." ".."; ".git" 目录跳过;
//   目录 → depth<32 递归; 文件 → visit(rel_path, full_path, user);
//   每项检查 *stop 与 token; visit 返回非 OK 即整体返回该码。
// visit_glob: fnmatch(pattern, rel, 0)==0 || fnmatch(pattern, basename(rel), 0)==0 → push rel
// visit_grep: include 过滤(fnmatch include, basename) → 首1KB NUL 探测跳二进制 →
//   fopen 逐行 fgets(MAX_LINE) → regexec → "path:line: text" 进 out_buf → 封顶置 stop
```

结果一律 `aegis_tool_result_set_string`；用户级错误（无法打开、非法 regex、路径逃逸）→ 内联 `error: ...` 字符串 + `AEGIS_OK`；仅 NOMEM 等内部失败返回真实状态码。`list` 输出 `name\tdir|file\t<size>`，目录优先、字典序。

- [ ] **Step 3: CMake 与注册接线**

```cmake
# src/coding/CMakeLists.txt
add_library(aegis_coding mutations.c coding_tools.c discovery_tools.c coding_agent.c)
```

`coding_tools.c` 的 `aegis_coding_tools_register_all` 末尾：

```c
#include "aegis/coding/discovery_tools.h"
// ...
    return aegis_coding_discovery_tools_register_all(reg);
```

- [ ] **Step 4: 验证通过**

Run: `cmake --build build -j && ctest --test-dir build -R unit_discovery_tools --output-on-failure`
Expected: ALL_DISCOVERY_TESTS PASSED

- [ ] **Step 5: Commit**

```bash
git add src/coding/path_safety.h src/coding/discovery_tools.c include/aegis/coding/discovery_tools.h src/coding/coding_tools.c src/coding/CMakeLists.txt tests/unit/test_discovery_tools.c cmake/AegisTests.cmake
git commit -m "feat(coding): add list/glob/grep discovery tools"
```

---

## Chunk 2: Coding Agent 模型热切换

### Task 4: 失败测试

**Files:**
- Create: `tests/unit/test_coding_agent.c`
- Modify: `cmake/AegisTests.cmake`

- [ ] **Step 1: 测试**

```c
#define _POSIX_C_SOURCE 200809L
#include "aegis/coding/coding_agent.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void expect_ok(aegis_status_t rc, const char* msg)
{
    if (rc != AEGIS_OK) { fprintf(stderr, "FAIL %s: %d\n", msg, (int)rc); assert(0); }
}

int main(void)
{
    aegis_coding_agent_config_t cfg = {0};
    cfg.project_root = ".";
    cfg.model = "mock";
    aegis_coding_agent_t* agent = NULL;
    expect_ok(aegis_coding_agent_create(&cfg, &agent), "create");
    assert(strcmp(aegis_coding_agent_model_name(agent), "mock") == 0);

    /* run works before switch (mock model) */
    expect_ok(aegis_coding_agent_run(agent, "hello"), "run before");

    /* invalid switches leave state intact */
    assert(aegis_coding_agent_set_model(agent, NULL) == AEGIS_ERR_INVALID);
    assert(aegis_coding_agent_set_model(agent, "") == AEGIS_ERR_INVALID);
    assert(strcmp(aegis_coding_agent_model_name(agent), "mock") == 0);

    /* hot switch */
    expect_ok(aegis_coding_agent_set_model(agent, "gpt-x"), "switch");
    assert(strcmp(aegis_coding_agent_model_name(agent), "gpt-x") == 0);
    expect_ok(aegis_coding_agent_run(agent, "again"), "run after");

    aegis_coding_agent_destroy(agent);
    printf("ALL_CODING_AGENT_TESTS PASSED\n");
    return 0;
}
```

- [ ] **Step 2: CMake 注册** `aegis_add_test(unit_coding_agent tests/unit/test_coding_agent.c)` + ASan 列表
- [ ] **Step 3: 验证编译失败**（`aegis_coding_agent_model_name` 未声明）

### Task 5: 实现模型 API

**Files:**
- Modify: `include/aegis/coding/coding_agent.h`、`src/coding/coding_agent.c`

- [ ] **Step 1: 头文件声明**

```c
const char*    aegis_coding_agent_model_name(const aegis_coding_agent_t* agent);
aegis_status_t aegis_coding_agent_set_model(aegis_coding_agent_t* agent, const char* model);
```

- [ ] **Step 2: 实现要点**

- 结构体新增 `char* model_name; char* provider; char* api_key; char* base_url;`（create 时 strdup 非空项，destroy 释放）
- 提取 `static aegis_status_t build_model(aegis_coding_agent_t* a, const char* model_name, aegis_model_client_t** out_client, aegis_openai_model_ctx_t** out_ctx)`：按 `a->provider == "llm-openai"`（ifdef 内）建 OpenAI 后端，否则 `aegis_model_client_create`（mock）——create 与 set_model 共用
- `set_model`：校验 → build_model 新实例 → 用当前 session/tools/system_prompt 建**新** loop（与 replace_session 相同 swap 模式）→ 成功后换入并销毁旧 loop/旧 client/旧 ctx；任一步失败销毁新实例返回错误，现状不动
- `system_prompt` 抽为文件内 static const，两处共用

- [ ] **Step 3: 验证**

Run: `cmake --build build -j && ctest --test-dir build -R unit_coding_agent --output-on-failure`
Expected: ALL_CODING_AGENT_TESTS PASSED

- [ ] **Step 4: Commit**

```bash
git add include/aegis/coding/coding_agent.h src/coding/coding_agent.c tests/unit/test_coding_agent.c cmake/AegisTests.cmake
git commit -m "feat(coding): model introspection and hot model switching"
```

---

## Chunk 3: CLI 命令

### Task 6: 实现 cli_interactive.c 命令

**Files:**
- Modify: `apps/aegis/cli_interactive.c`

- [ ] **Step 1: banner 反映真实模型**

```c
static void print_banner(const char* model)
{
    printf("Aegis Coding Agent\n");
    printf("project: %s\n", ".");
    printf("model: %s\n", model ? model : "mock");
    printf("type /help for commands\n\n");
}
```

- [ ] **Step 2: /help 更新**：`/help /model /session /sessions /resume /fork /tree /compact /json /clear /quit`
- [ ] **Step 3: 命令处理**（插入到 `/fork` 之前，需 `#include <dirent.h> <sys/stat.h> <time.h>`）：

- `/model`：无参打印 `model: <name>`（`aegis_coding_agent_model_name`）；有参调 `aegis_coding_agent_set_model`，成功打印 `switched model to <name>`，失败打印 `error: <status>`
- `/session`：合并 `/tree` 的输出（id/branch/parent/messages），`/tree` 保留为别名
- `/sessions`：`opendir(".aegis")`，筛 `session-*.jsonl` 后缀匹配，`stat` 取 mtime 用 `strftime` 打印 `<name>  <YYYY-MM-DD HH:MM:SS>`；目录不存在或为空打印 `(no saved sessions)`
- `/resume <path>`：无参打印 usage；`aegis_session_load` 成功 → `aegis_coding_agent_replace_session` → 打印 `resumed <id> (N messages)`；load 失败打印 `resume failed: <status str>`，当前会话不动

### Task 7: CLI 集成测试

**Files:**
- Modify: `tests/integration/test_cli.c`

- [ ] **Step 1: 新增 stdin 驱动 helper**

```c
static int run_cli_stdin(const char* input, char* out, size_t out_len, int* exit_code)
{
    const char* bin = find_cli_bin();
    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "printf '%s' | %s 2>&1", input, bin);
    // popen 同 run_cli；input 中不得含单引号
}
```

- [ ] **Step 2: 新增 test_interactive_commands**：临时目录 + chdir 后依次验证：
  1. `/model\n/model gpt-x\n/model\n/quit\n` → banner/current 含 `model: mock`、`switched model to gpt-x`、`model: gpt-x`；退出后 `.aegis/session-*.jsonl` 存在
  2. 用 dirent 找到实际会话文件路径 → `/resume <path>\n/session\n/quit\n` → 含 `resumed` 与 `session `
  3. `/sessions\n/quit\n` → 含 `.jsonl`
  4. `/help\n/quit\n` → 含 `/resume`
- [ ] **Step 3: main() 注册新用例，运行验证**

Run: `cmake --build build -j && ctest --test-dir build -R integration_cli --output-on-failure`
Expected: ALL_CLI_TESTS PASSED

- [ ] **Step 4: Commit**

```bash
git add apps/aegis/cli_interactive.c tests/integration/test_cli.c
git commit -m "feat(cli): implement /model /session /sessions /resume commands"
```

---

## Chunk 4: 全量验收

### Task 8: 回归与消毒

- [ ] **Step 1: 全量测试**

Run: `cmake --build build -j && ctest --test-dir build --output-on-failure`
Expected: 全部通过（含 57 既有用例 + 2 新单测 + 扩展的 integration_cli）

- [ ] **Step 2: ASan 构建**

Run: `cmake -S . -B build-asan -DCMAKE_C_FLAGS="-fsanitize=address -g" && cmake --build build-asan -j && ctest --test-dir build-asan --output-on-failure`
Expected: 0 泄漏 / 0 告警（新测试加入 ASan 属性列表）

- [ ] **Step 3: 格式检查**

Run: `cmake --build build --target format 2>&1 | tail -3`（若 clang-format 可用）
- [ ] **Step 4: 确认 `git status` 中仅本计划文件被新增/修改；无关脏文件不提交**

---

## 验收标准

- [ ] `list/glob/grep` 经 `aegis_coding_tools_register_all` 自动可用，路径逃逸/取消/截断/二进制跳过全部生效
- [ ] `aegis_coding_agent_set_model` 热切换生效，失败不破坏现状
- [ ] CLI `/model /session /sessions /resume` 按 spec §5 工作，`/help` 与 banner 与实际一致
- [ ] 全量 ctest + ASan 通过，0 编译警告
