# Git Tools Integration — Detailed Design

> **Status:** Design Phase
> **Estimated Effort:** 2–3 days
> **Dependencies:** None (standalone addition to coding tools)

---

## 1. Motivation

The Coding Agent currently has 7 tools: `read`, `write`, `edit`, `bash`, `list`, `glob`, `grep`. All file modifications are silent — the agent has no version control awareness. This creates two problems:

1. **No rollback safety net.** If the agent makes a bad edit, there's no built-in way to revert.
2. **No commit automation.** The agent must manually construct `bash git commit -m ...` commands, which is error-prone and loses the structured metadata that native tools could provide.

Git tools give the agent **first-class version control awareness** while keeping the security model tight (capabilities + approval hooks).

---

## 2. Architecture

```
src/coding/
├── coding_tools.c         ← existing: read, write, edit, bash
├── git_tools.c            ← NEW: 5 git tools
├── discovery_tools.c      ← existing: list, glob, grep
├── coding_agent.c         ← unchanged
├── mutations.c            ← unchanged
├── path_safety.h          ← unchanged (git paths use same safety)
└── CMakeLists.txt         ← add git_tools.c to sources

include/aegis/coding/
├── coding_tools.h         ← add git tool declarations
├── discovery_tools.h      ← unchanged
└── git_tools.h            ← NEW: git tool declarations
```

### Design Decision: Separate File vs. Extend coding_tools.c

**Chosen: Separate `git_tools.c` + `git_tools.h`**

Rationale:
- Git tools share a common pattern (shell out to git) that is distinct from the file-I/O pattern of read/write/edit
- The execute functions are structurally similar to each other (extract args → build git command → fork/exec → capture output)
- Keeps `coding_tools.c` focused on file operations
- Follows the same pattern as `discovery_tools.c` being separate from `coding_tools.c`

---

## 3. Tool Definitions

### 3.1 `git_status` — Show working tree status

```
Name:        git_status
Description: "Show working tree status: branch, modified/added/deleted files"
Capabilities: AEGIS_CAP_READ_FILE
```

**Parameters:**

| Name | Type | Required | Description |
|------|------|----------|-------------|
| path | STRING | no | Subdirectory to check (default: project root) |

**Implementation:**
```
Executes: git -C <project_root> status --porcelain=v1 --branch
```

**Output format:**
```
## main...origin/main [ahead 1]
 M src/coding/coding_tools.c
 M src/coding/git_tools.c
A  tests/unit/test_git_tools.c
D  old_file.c
?? untracked.txt
```

**Constraints:**
- Path must pass `aegis_safe_relative_path()` (prevents directory traversal)
- Output capped at 64KB (same as discovery tools)

**Example call:**
```json
{"tool": "git_status", "arguments": {"path": "src/coding"}}
```

**Example result:**
```
## main...origin/main
 M git_tools.c
?? test_git_tools.c
```

---

### 3.2 `git_diff` — Show file changes

```
Name:        git_diff
Description: "Show unified diff of changes (working tree, staged, or between commits)"
Capabilities: AEGIS_CAP_READ_FILE
```

**Parameters:**

| Name | Type | Required | Description |
|------|------|----------|-------------|
| path | STRING | no | File or directory to diff (default: all) |
| staged | BOOL | no | Show staged changes instead of working tree (default: false) |
| base | STRING | no | Base commit/ref for comparison (default: HEAD) |

**Implementation:**
```
Working tree:  git -C <root> diff [--] [<path>]
Staged:        git -C <root> diff --staged [--] [<path>]
Compare:       git -C <root> diff <base> [--] [<path>]
```

**Constraints:**
- Path must pass `aegis_safe_relative_path()`
- Diff output capped at 128KB (diffs can be large; truncate with "... truncated")
- Check cancellation token before returning large output

**Example call:**
```json
{"tool": "git_diff", "arguments": {"path": "src/coding/git_tools.c", "staged": false}}
```

**Example result:**
```
diff --git a/src/coding/git_tools.c b/src/coding/git_tools.c
index abc1234..def5678 100644
--- a/src/coding/git_tools.c
+++ b/src/coding/git_tools.c
@@ -1,5 +1,8 @@
+#include "aegis/coding/git_tools.h"
+
 static aegis_status_t tool_git_status_execute(...)
 {
+    // new implementation
 }
```

---

### 3.3 `git_commit` — Create a commit

```
Name:        git_commit
Description: "Stage files and create a git commit"
Capabilities: AEGIS_CAP_SHELL | AEGIS_CAP_WRITE_FILE
```

**Parameters:**

| Name | Type | Required | Description |
|------|------|----------|-------------|
| message | STRING | yes | Commit message |
| files | STRING | no | Comma-separated file paths to stage (default: all tracked changes) |

**Implementation:**
```
1. If files provided:
   git -C <root> add -- <file1> <file2> ...
   (each file validated via aegis_safe_relative_path)
2. If no files (default):
   git -C <root> add -u
3. git -C <root> commit -m "<message>"
```

**Security constraints:**
- Requires `AEGIS_CAP_SHELL` capability
- Requires Tool Approval (destructive/persistent operation)
- Each file path validated via `aegis_safe_relative_path()`
- Commit message length capped at 512 characters
- **Refuse to commit if `message` contains shell metacharacters** (`$`, `` ` ``, `$(`, etc.) — use `git commit -m` with safe argument passing via `execvp` array, NOT `system()`

**Output:**
```
Committed abc1234: "fix: resolve null pointer in tool execution"
```

**Example call:**
```json
{"tool": "git_commit", "arguments": {"message": "feat: add git tools to coding agent", "files": "src/coding/git_tools.c,include/aegis/coding/git_tools.h"}}
```

**Example result:**
```
Committed def5678: "feat: add git tools to coding agent"
```

---

### 3.4 `git_log` — Show commit history

```
Name:        git_log
Description: "Show recent commit history with stats"
Capabilities: AEGIS_CAP_READ_FILE
```

**Parameters:**

| Name | Type | Required | Description |
|------|------|----------|-------------|
| count | INT | no | Number of commits to show (default: 10, max: 50) |
| path | STRING | no | Filter by file or directory |
| format | STRING | no | "oneline" (default), "short", "full" |

**Implementation:**
```
git -C <root> log --format=<fmt> -n <count> [-- <path>]
  oneline: --format="%h %s"
  short:   --format="%h %ad %an: %s" --date=short
  full:    --format="%H%n%ad%nAuthor: %an <%ae>%n%n    %s%n" --date=iso
```

**Constraints:**
- `count` clamped to [1, 50]
- Path validated via `aegis_safe_relative_path()`
- Output capped at 64KB

**Example call:**
```json
{"tool": "git_log", "arguments": {"count": 5, "format": "short"}}
```

**Example result:**
```
def5678 2026-09-08 Alice: feat: add git tools
abc1234 2026-09-07 Alice: fix: resolve null pointer
9f8e7d6 2026-09-06 Bob: refactor: extract tool registry
```

---

### 3.5 `git_branch` — List and create branches

```
Name:        git_branch
Description: "List, create, or switch git branches"
Capabilities: AEGIS_CAP_SHELL | AEGIS_CAP_READ_FILE
```

**Parameters:**

| Name | Type | Required | Description |
|------|------|----------|-------------|
| action | STRING | yes | "list", "create", or "switch" |
| name | STRING | no | Branch name (required for create/switch) |

**Implementation:**
```
list:    git -C <root> branch -a --list
create:  git -C <root> branch <name>
switch:  git -C <root> switch <name>  (or: checkout -b if create+switch needed)
```

**Security constraints:**
- `create` and `switch` require `AEGIS_CAP_SHELL` capability
- Branch name validated: must match `^[a-zA-Z0-9._/\-]+$` (no shell metacharacters)
- `create` and `switch` require Tool Approval

**Output:**
```
list:    "* main\n  feature/git-tools\n  remotes/origin/main"
create:  "Created branch feature/git-tools"
switch:  "Switched to branch feature/git-tools"
```

**Example call:**
```json
{"tool": "git_branch", "arguments": {"action": "list"}}
```

**Example result:**
```
* main
  feature/git-tools
  remotes/origin/main
```

---

## 4. Implementation Details

### 4.1 Common Git Execution Helper

All 5 tools share the same shell-out pattern. Extract into a shared helper:

```c
/**
 * @brief Execute a git command and capture stdout+stderr.
 *
 * Uses fork/exec (NOT system()) for security and cancellation support.
 * Validates that no shell metacharacters appear in user-supplied arguments.
 *
 * @param project_root  Working directory for git.
 * @param argv          NULL-terminated argument array (argv[0] = "git").
 * @param token         Cancellation token (polls during wait).
 * @param[out] out      Heap-allocated output string (caller frees).
 * @param[out] exit_code Git exit code.
 * @return AEGIS_OK on success, AEGIS_ERR_CANCELLED if token tripped.
 */
static aegis_status_t git_exec(
    const char* project_root,
    char** argv,
    const aegis_cancellation_token_t* token,
    char** out,
    int* exit_code);
```

This reuses the fork/exec/pipe/poll pattern from `tool_bash_execute()` in `coding_tools.c` but:
- Uses `execvp("git", argv)` directly (no shell)
- Polls cancellation token during wait
- Captures stdout + stderr
- Returns exit code

### 4.2 Argument Validation Helper

```c
/**
 * @brief Check a string for shell metacharacters.
 *
 * Rejects: $ ` " ' \ ( ) & ; | # ! { } < > \n \r
 * Used to sanitize user-provided commit messages and branch names.
 *
 * @return true if safe, false if contains dangerous characters.
 */
static bool git_arg_is_safe(const char* s);

/**
 * @brief Validate a branch name.
 *
 * Must match: [a-zA-Z0-9._/\-]+
 * Rejects: spaces, shell metacharacters, empty strings.
 */
static bool git_branch_name_valid(const char* name);
```

### 4.3 Output Capping

Reuse the `out_buf_t` pattern from `discovery_tools.c`:

```c
#define GIT_MAX_OUTPUT  (128 * 1024)  /* 128KB for diffs */
#define GIT_MAX_LOG     (64 * 1024)   /* 64KB for log/status */
```

### 4.4 Cancellation Support

Every tool polls the cancellation token in two places:
1. Before executing git (check early exit)
2. During the `poll()` loop in `git_exec()` (check between reads)

---

## 5. Registration

### 5.1 New Header: `include/aegis/coding/git_tools.h`

```c
#ifndef AEGIS_CODING_GIT_TOOLS_H
#define AEGIS_CODING_GIT_TOOLS_H

#include "aegis/tool/tool.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file git_tools.h
 * @brief Builtin git tools: status, diff, commit, log, branch.
 *
 * All tools shell out to git via fork/exec (no shell interpolation).
 * Paths are project-root confined. Commit messages and branch names
 * are validated against shell metacharacters.
 */

extern const aegis_tool_def_t aegis_coding_tool_git_status;
extern const aegis_tool_def_t aegis_coding_tool_git_diff;
extern const aegis_tool_def_t aegis_coding_tool_git_commit;
extern const aegis_tool_def_t aegis_coding_tool_git_log;
extern const aegis_tool_def_t aegis_coding_tool_git_branch;

aegis_status_t aegis_coding_git_tools_register_all(aegis_tool_registry_t* reg);

#ifdef __cplusplus
}
#endif

#endif /* AEGIS_CODING_GIT_TOOLS_H */
```

### 5.2 Update `include/aegis/coding/coding_tools.h`

Add:
```c
#include "aegis/coding/git_tools.h"
```

### 5.3 Update `src/coding/CMakeLists.txt`

```cmake
add_library(aegis_coding
    mutations.c
    coding_tools.c
    discovery_tools.c
    git_tools.c          # ← NEW
    coding_agent.c
)
```

### 5.4 Update Registration Chain

In `aegis_coding_tools_register_all()` (coding_tools.c), add at the end:

```c
    // Register git tools
    st = aegis_coding_git_tools_register_all(reg);
    if (st != AEGIS_OK) {
        return st;
    }
    return aegis_coding_discovery_tools_register_all(reg);
```

This means the tool count goes from **7 → 12**:
```
read, write, edit, bash,                          ← coding_tools
git_status, git_diff, git_commit, git_log, git_branch,  ← git_tools
list, glob, grep                                  ← discovery_tools
```

---

## 6. Security Rules

### 6.1 Capability Requirements

| Tool | Capabilities | Rationale |
|------|-------------|-----------|
| `git_status` | `READ_FILE` | Read-only operation |
| `git_diff` | `READ_FILE` | Read-only operation |
| `git_log` | `READ_FILE` | Read-only operation |
| `git_commit` | `SHELL \| WRITE_FILE` | Creates commits (persistent) |
| `git_branch` (list) | `READ_FILE` | Read-only |
| `git_branch` (create/switch) | `SHELL` | Modifies git state |

### 6.2 Policy Rules (for `aegis_security_policy_t`)

Add to the default security policy (or document for users):

```c
// Read-only git operations — always allowed for coding agents
aegis_security_policy_add_rule(policy, "git_status",  AEGIS_CAP_READ_FILE);
aegis_security_policy_add_rule(policy, "git_diff",    AEGIS_CAP_READ_FILE);
aegis_security_policy_add_rule(policy, "git_log",     AEGIS_CAP_READ_FILE);

// Destructive git operations — require approval
aegis_security_policy_add_rule(policy, "git_commit",  AEGIS_CAP_SHELL | AEGIS_CAP_WRITE_FILE);
aegis_security_policy_add_rule(policy, "git_branch",  AEGIS_CAP_SHELL | AEGIS_CAP_READ_FILE);
```

### 6.3 Tool Approval Integration

Git tools work with the existing approval hook:
- `git_status`, `git_diff`, `git_log` → **auto-allow** (read-only)
- `git_commit` → **prompt y/n/a** (persistent change)
- `git_branch` (create/switch) → **prompt y/n/a** (modifies state)

The approval callback receives the tool name; the CLI's `cli_approval_cb()` can check:
```c
if (strcmp(tool_name, "git_commit") == 0 || strcmp(tool_name, "git_branch") == 0) {
    // prompt user
}
```

### 6.4 Input Validation Matrix

| Input | Validation | Failure Mode |
|-------|-----------|--------------|
| `path` parameter | `aegis_safe_relative_path()` | Returns "error: path must stay inside the project" |
| `message` (commit) | `git_arg_is_safe()` + length ≤ 512 | Returns "error: unsafe commit message" |
| `name` (branch) | `git_branch_name_valid()` regex | Returns "error: invalid branch name" |
| `count` (log) | Clamped to [1, 50] | Silently clamped |
| `files` (commit) | Each path validated via `aegis_safe_relative_path()` | Returns "error: unsafe file path" |
| `base` (diff) | `git_arg_is_safe()` | Returns "error: unsafe ref" |

---

## 7. Test Plan

### 7.1 Unit Tests: `tests/unit/test_git_tools.c`

Following the existing `test_coding_agent.c` pattern (mock model + GTest):

```
Test Suite: git_tools

test_git_status_basic:
    - Create temp dir, init git repo
    - Create a file, call git_status tool
    - Assert output contains the filename

test_git_status_with_path:
    - Create subdirectory with changes
    - Call git_status with path="subdir"
    - Assert only subdir changes shown

test_git_diff_working:
    - Modify a tracked file
    - Call git_diff
    - Assert diff contains the modification

test_git_diff_staged:
    - Stage a change (git add)
    - Call git_diff with staged=true
    - Assert diff shown

test_git_commit_basic:
    - Modify a file
    - Call git_commit with message="test commit"
    - Assert output contains commit hash
    - Verify: git log shows the commit

test_git_commit_specific_files:
    - Modify two files
    - Call git_commit with files="file1.c"
    - Assert only file1.c is committed, file2.c remains unstaged

test_git_commit_rejects_unsafe_message:
    - Call git_commit with message="test; rm -rf /"
    - Assert error: "unsafe commit message"

test_git_commit_rejects_unsafe_file:
    - Call git_commit with files="../escape.c"
    - Assert error: "unsafe file path"

test_git_log_basic:
    - Create 3 commits
    - Call git_log with count=2
    - Assert output shows exactly 2 commits

test_git_log_by_path:
    - Create commits touching different files
    - Call git_log with path="src/main.c"
    - Assert only relevant commits shown

test_git_log_format_oneline:
    - Call git_log with format="oneline"
    - Assert format matches "%h %s" pattern

test_git_branch_list:
    - Create branches "a" and "b"
    - Call git_branch with action="list"
    - Assert output contains both branch names

test_git_branch_create:
    - Call git_branch with action="create", name="feature/test"
    - Assert output contains "Created branch"
    - Verify: git branch shows the new branch

test_git_branch_create_rejects_unsafe_name:
    - Call git_branch with action="create", name="a; rm -rf /"
    - Assert error: "invalid branch name"

test_git_branch_switch:
    - Create and switch to new branch
    - Assert output contains "Switched to branch"

test_git_status_not_a_repo:
    - Call git_status in a non-git directory
    - Assert graceful error message

test_git_tools_register_all:
    - Create registry
    - Call aegis_coding_git_tools_register_all()
    - Assert count == 5
    - Assert each tool name is found via registry_find()
```

### 7.2 Integration Tests: `tests/integration/test_cli.c`

Add scenario to existing integration test:

```
test_git_tools_interactive:
    - Init git repo in temp dir
    - Create file
    - Feed CLI: "/tools" — assert git_status, git_diff etc appear
    - Feed CLI: "commit this file with message 'initial'" 
    - (with mock model returning a git_commit tool call)
    - Assert output contains "Committed"
```

---

## 8. Implementation Order

| Step | Files | Description |
|------|-------|-------------|
| 1 | `include/aegis/coding/git_tools.h` | Header with 5 extern tool declarations + register_all |
| 2 | `src/coding/git_tools.c` | Shared `git_exec()` helper + 5 tool implementations |
| 3 | `src/coding/CMakeLists.txt` | Add `git_tools.c` to sources |
| 4 | `src/coding/coding_tools.c` | Add `#include "git_tools.h"` + call `register_all` in registration chain |
| 5 | `tests/unit/test_git_tools.c` | Unit tests (18 test cases) |
| 6 | `tests/integration/test_cli.c` | Integration test scenario |
| 7 | `cmake/AegisTests.cmake` | Register `unit_git_tools` test target |

---

## 9. Example Agent Workflow

With Git tools, a coding agent session might look like:

```
User: "Fix the null pointer crash in tool execution"

Agent:
  1. git_status → sees clean working tree
  2. git_log --count=3 → understands recent changes
  3. grep "tool_execute" --include="*.c" → finds relevant code
  4. read src/coding/tool.c → reads the file
  5. edit src/coding/tool.c → fixes the null check
  6. git_diff → reviews the change
  7. git_commit(message="fix: null check in tool_execute", files="src/coding/tool.c")
     → Committed abc1234: "fix: null check in tool_execute"

[Tool approval prompt: approve git_commit? [y/n/a]]
```

---

## 10. Future Extensions

- **`git_blame`** — annotate lines with commit authors (useful for understanding code ownership)
- **`git_stash`** — stash changes temporarily (for context switching)
- **`git_merge`** — merge branches (high-risk, would require double approval)
- **`git_rebase`** — rebase onto another branch (even higher risk)
- **`git_remote`** — push/pull operations (requires network capability)
