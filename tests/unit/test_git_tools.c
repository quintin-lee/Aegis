#define _POSIX_C_SOURCE 200809L
/**
 * @file test_git_tools.c
 * @brief Unit tests for git status/diff/commit/log/branch tools.
 */
#include "aegis/coding/git_tools.h"
#include "aegis/tool/tool.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <limits.h>

/* ── Helpers ───────────────────────────────────────────────────────────── */

static void expect_ok(aegis_status_t rc, const char* msg)
{
    if (rc != AEGIS_OK) {
        fprintf(stderr, "FAIL %s: %d\n", msg, (int)rc);
        assert(0);
    }
}

static char g_test_dir[PATH_MAX];

/** Create a temporary directory with a git repo. */
static void setup_git_repo(void)
{
    snprintf(g_test_dir, sizeof(g_test_dir), "/tmp/aegis_git_test_XXXXXX");
    assert(mkdtemp(g_test_dir) != NULL);
    assert(chdir(g_test_dir) == 0);

    /* Init repo */
    int rc = system("git init -q");
    assert(rc == 0);
    rc = system("git config user.email \"test@test.com\"");
    assert(rc == 0);
    rc = system("git config user.name \"Test\"");
    assert(rc == 0);
}

static void cleanup_git_repo(void)
{
    char cmd[PATH_MAX + 32];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", g_test_dir);
    system(cmd);
}

static void create_file(const char* name, const char* content)
{
    FILE* f = fopen(name, "w");
    assert(f);
    if (content) {
        fputs(content, f);
    }
    fclose(f);
}

/* ── Tests ─────────────────────────────────────────────────────────────── */

static void test_git_status_basic(void)
{
    printf("[test] git_status_basic ...\n");
    setup_git_repo();

    create_file("hello.txt", "hello world\n");

    aegis_tool_result_t result = {0};
    aegis_status_t      st     = aegis_coding_tool_git_status.execute(NULL, NULL, NULL, &result);
    expect_ok(st, "git_status execute");

    assert(result.value.type == AEGIS_TOOL_VAL_STRING);
    assert(result.value.as.str.ptr != NULL);
    /* Should show the untracked file */
    assert(strstr(result.value.as.str.ptr, "hello.txt") != NULL);

    aegis_tool_result_destroy(&result);
    cleanup_git_repo();
    printf("  git_status_basic PASS\n");
}

static void test_git_status_with_path(void)
{
    printf("[test] git_status_with_path ...\n");
    setup_git_repo();

    mkdir("subdir", 0755);
    create_file("subdir/a.txt", "a\n");
    create_file("root.txt", "r\n");

    aegis_tool_args_t* args = NULL;
    expect_ok(aegis_tool_args_create(&args), "create args");
    expect_ok(aegis_tool_args_add_string(args, "path", "subdir"), "add path");

    aegis_tool_result_t result = {0};
    aegis_status_t      st     = aegis_coding_tool_git_status.execute(NULL, args, NULL, &result);
    expect_ok(st, "git_status with path");

    assert(result.value.type == AEGIS_TOOL_VAL_STRING);
    /* Should show subdir file, but filtering is done by git */
    assert(result.value.as.str.ptr != NULL);

    aegis_tool_result_destroy(&result);
    aegis_tool_args_destroy(args);
    cleanup_git_repo();
    printf("  git_status_with_path PASS\n");
}

static void test_git_diff_working(void)
{
    printf("[test] git_diff_working ...\n");
    setup_git_repo();

    create_file("test.c", "line1\n");
    system("git add test.c && git commit -q -m 'initial'");

    /* Modify the file */
    create_file("test.c", "line1\nmodified\n");

    aegis_tool_result_t result = {0};
    aegis_status_t      st     = aegis_coding_tool_git_diff.execute(NULL, NULL, NULL, &result);
    expect_ok(st, "git_diff execute");

    assert(result.value.type == AEGIS_TOOL_VAL_STRING);
    assert(result.value.as.str.ptr != NULL);
    assert(strstr(result.value.as.str.ptr, "modified") != NULL);

    aegis_tool_result_destroy(&result);
    cleanup_git_repo();
    printf("  git_diff_working PASS\n");
}

static void test_git_diff_staged(void)
{
    printf("[test] git_diff_staged ...\n");
    setup_git_repo();

    create_file("test.c", "line1\n");
    system("git add test.c && git commit -q -m 'initial'");

    /* Modify and stage */
    create_file("test.c", "line1\nstaged change\n");
    system("git add test.c");

    aegis_tool_args_t* args = NULL;
    expect_ok(aegis_tool_args_create(&args), "create args");
    expect_ok(aegis_tool_args_add_bool(args, "staged", true), "add staged");

    aegis_tool_result_t result = {0};
    aegis_status_t      st     = aegis_coding_tool_git_diff.execute(NULL, args, NULL, &result);
    expect_ok(st, "git_diff staged");

    assert(result.value.type == AEGIS_TOOL_VAL_STRING);
    assert(strstr(result.value.as.str.ptr, "staged change") != NULL);

    aegis_tool_result_destroy(&result);
    aegis_tool_args_destroy(args);
    cleanup_git_repo();
    printf("  git_diff_staged PASS\n");
}

static void test_git_commit_basic(void)
{
    printf("[test] git_commit_basic ...\n");
    setup_git_repo();

    create_file("new.txt", "new content\n");

    aegis_tool_args_t* args = NULL;
    expect_ok(aegis_tool_args_create(&args), "create args");
    expect_ok(aegis_tool_args_add_string(args, "message", "test commit"), "add message");

    aegis_tool_result_t result = {0};
    aegis_status_t      st     = aegis_coding_tool_git_commit.execute(NULL, args, NULL, &result);
    expect_ok(st, "git_commit execute");

    assert(result.value.type == AEGIS_TOOL_VAL_STRING);
    /* Should contain commit hash or success message */
    assert(result.value.as.str.ptr != NULL);
    assert(strlen(result.value.as.str.ptr) > 0);

    /* Verify: git log should show the commit */
    int rc = system("git log --oneline | grep -q 'test commit'");
    assert(rc == 0);

    aegis_tool_result_destroy(&result);
    aegis_tool_args_destroy(args);
    cleanup_git_repo();
    printf("  git_commit_basic PASS\n");
}

static void test_git_commit_specific_files(void)
{
    printf("[test] git_commit_specific_files ...\n");
    setup_git_repo();

    create_file("file1.txt", "one\n");
    create_file("file2.txt", "two\n");

    aegis_tool_args_t* args = NULL;
    expect_ok(aegis_tool_args_create(&args), "create args");
    expect_ok(aegis_tool_args_add_string(args, "message", "commit file1 only"), "add message");
    expect_ok(aegis_tool_args_add_string(args, "files", "file1.txt"), "add files");

    aegis_tool_result_t result = {0};
    aegis_status_t      st     = aegis_coding_tool_git_commit.execute(NULL, args, NULL, &result);
    expect_ok(st, "git_commit specific files");

    /* Verify file1 is committed, file2 is not */
    int rc1 = system("git log --oneline | grep -q 'commit file1 only'");
    assert(rc1 == 0);

    /* file2 should still be untracked */
    int rc2 = system("git status --porcelain | grep -q 'file2.txt'");
    assert(rc2 == 0);

    aegis_tool_result_destroy(&result);
    aegis_tool_args_destroy(args);
    cleanup_git_repo();
    printf("  git_commit_specific_files PASS\n");
}

static void test_git_commit_rejects_unsafe_message(void)
{
    printf("[test] git_commit_rejects_unsafe_message ...\n");
    setup_git_repo();

    create_file("x.txt", "x\n");

    aegis_tool_args_t* args = NULL;
    expect_ok(aegis_tool_args_create(&args), "create args");
    expect_ok(aegis_tool_args_add_string(args, "message", "test; rm -rf /"), "add unsafe msg");

    aegis_tool_result_t result = {0};
    aegis_status_t      st     = aegis_coding_tool_git_commit.execute(NULL, args, NULL, &result);
    expect_ok(st, "git_commit should return OK but with error in result");

    assert(result.value.type == AEGIS_TOOL_VAL_STRING);
    assert(strstr(result.value.as.str.ptr, "error") != NULL);

    aegis_tool_result_destroy(&result);
    aegis_tool_args_destroy(args);
    cleanup_git_repo();
    printf("  git_commit_rejects_unsafe_message PASS\n");
}

static void test_git_commit_rejects_unsafe_file(void)
{
    printf("[test] git_commit_rejects_unsafe_file ...\n");
    setup_git_repo();

    aegis_tool_args_t* args = NULL;
    expect_ok(aegis_tool_args_create(&args), "create args");
    expect_ok(aegis_tool_args_add_string(args, "message", "test"), "add message");
    expect_ok(aegis_tool_args_add_string(args, "files", "../escape.c"), "add unsafe file");

    aegis_tool_result_t result = {0};
    aegis_status_t      st     = aegis_coding_tool_git_commit.execute(NULL, args, NULL, &result);
    expect_ok(st, "git_commit should return OK with error in result");

    assert(result.value.type == AEGIS_TOOL_VAL_STRING);
    assert(strstr(result.value.as.str.ptr, "error") != NULL);
    assert(strstr(result.value.as.str.ptr, "unsafe") != NULL);

    aegis_tool_result_destroy(&result);
    aegis_tool_args_destroy(args);
    cleanup_git_repo();
    printf("  git_commit_rejects_unsafe_file PASS\n");
}

static void test_git_log_basic(void)
{
    printf("[test] git_log_basic ...\n");
    setup_git_repo();

    create_file("a.txt", "a\n");
    system("git add a.txt && git commit -q -m 'commit 1'");
    create_file("b.txt", "b\n");
    system("git add b.txt && git commit -q -m 'commit 2'");
    create_file("c.txt", "c\n");
    system("git add c.txt && git commit -q -m 'commit 3'");

    aegis_tool_args_t* args = NULL;
    expect_ok(aegis_tool_args_create(&args), "create args");
    expect_ok(aegis_tool_args_add_int(args, "count", 2), "add count");

    aegis_tool_result_t result = {0};
    aegis_status_t      st     = aegis_coding_tool_git_log.execute(NULL, args, NULL, &result);
    expect_ok(st, "git_log execute");

    assert(result.value.type == AEGIS_TOOL_VAL_STRING);
    assert(result.value.as.str.ptr != NULL);
    /* Should show exactly 2 commits (commit 3 and commit 2) */
    assert(strstr(result.value.as.str.ptr, "commit 3") != NULL);
    assert(strstr(result.value.as.str.ptr, "commit 2") != NULL);
    /* commit 1 should NOT appear (count=2) */
    assert(strstr(result.value.as.str.ptr, "commit 1") == NULL);

    aegis_tool_result_destroy(&result);
    aegis_tool_args_destroy(args);
    cleanup_git_repo();
    printf("  git_log_basic PASS\n");
}

static void test_git_log_by_path(void)
{
    printf("[test] git_log_by_path ...\n");
    setup_git_repo();

    system("mkdir -p src");
    create_file("src/main.c", "v1\n");
    system("git add src/main.c && git commit -q -m 'src change'");
    system("mkdir -p docs");
    create_file("docs/readme.md", "v1\n");
    system("git add docs/readme.md && git commit -q -m 'docs change'");

    aegis_tool_args_t* args = NULL;
    expect_ok(aegis_tool_args_create(&args), "create args");
    expect_ok(aegis_tool_args_add_string(args, "path", "src"), "add path");

    aegis_tool_result_t result = {0};
    aegis_status_t      st     = aegis_coding_tool_git_log.execute(NULL, args, NULL, &result);
    expect_ok(st, "git_log by path");

    assert(result.value.type == AEGIS_TOOL_VAL_STRING);
    assert(strstr(result.value.as.str.ptr, "src change") != NULL);

    aegis_tool_result_destroy(&result);
    aegis_tool_args_destroy(args);
    cleanup_git_repo();
    printf("  git_log_by_path PASS\n");
}

static void test_git_log_format_oneline(void)
{
    printf("[test] git_log_format_oneline ...\n");
    setup_git_repo();

    create_file("a.txt", "a\n");
    system("git add a.txt && git commit -q -m 'test oneline'");

    aegis_tool_args_t* args = NULL;
    expect_ok(aegis_tool_args_create(&args), "create args");
    expect_ok(aegis_tool_args_add_string(args, "format", "oneline"), "add format");

    aegis_tool_result_t result = {0};
    aegis_status_t      st     = aegis_coding_tool_git_log.execute(NULL, args, NULL, &result);
    expect_ok(st, "git_log oneline");

    assert(result.value.type == AEGIS_TOOL_VAL_STRING);
    /* Oneline format: "<hash> <subject>" */
    assert(result.value.as.str.ptr != NULL);
    /* Should have a short hash (7+ hex chars) followed by space */
    assert(strlen(result.value.as.str.ptr) > 8);

    aegis_tool_result_destroy(&result);
    aegis_tool_args_destroy(args);
    cleanup_git_repo();
    printf("  git_log_format_oneline PASS\n");
}

static void test_git_branch_list(void)
{
    printf("[test] git_branch_list ...\n");
    setup_git_repo();

    create_file("a.txt", "a\n");
    system("git add a.txt && git commit -q -m 'initial'");
    system("git branch feature-a");
    system("git branch feature-b");

    aegis_tool_args_t* args = NULL;
    expect_ok(aegis_tool_args_create(&args), "create args");
    expect_ok(aegis_tool_args_add_string(args, "action", "list"), "add action");

    aegis_tool_result_t result = {0};
    aegis_status_t      st     = aegis_coding_tool_git_branch.execute(NULL, args, NULL, &result);
    expect_ok(st, "git_branch list");

    assert(result.value.type == AEGIS_TOOL_VAL_STRING);
    assert(strstr(result.value.as.str.ptr, "feature-a") != NULL);
    assert(strstr(result.value.as.str.ptr, "feature-b") != NULL);

    aegis_tool_result_destroy(&result);
    aegis_tool_args_destroy(args);
    cleanup_git_repo();
    printf("  git_branch_list PASS\n");
}

static void test_git_branch_create(void)
{
    printf("[test] git_branch_create ...\n");
    setup_git_repo();

    create_file("a.txt", "a\n");
    system("git add a.txt && git commit -q -m 'initial'");

    aegis_tool_args_t* args = NULL;
    expect_ok(aegis_tool_args_create(&args), "create args");
    expect_ok(aegis_tool_args_add_string(args, "action", "create"), "add action");
    expect_ok(aegis_tool_args_add_string(args, "name", "feature/test"), "add name");

    aegis_tool_result_t result = {0};
    aegis_status_t      st     = aegis_coding_tool_git_branch.execute(NULL, args, NULL, &result);
    expect_ok(st, "git_branch create");

    assert(result.value.type == AEGIS_TOOL_VAL_STRING);
    assert(strstr(result.value.as.str.ptr, "Created branch") != NULL);

    /* Verify branch exists */
    int rc = system("git branch | grep -q 'feature/test'");
    assert(rc == 0);

    aegis_tool_result_destroy(&result);
    aegis_tool_args_destroy(args);
    cleanup_git_repo();
    printf("  git_branch_create PASS\n");
}

static void test_git_branch_create_rejects_unsafe_name(void)
{
    printf("[test] git_branch_create_rejects_unsafe_name ...\n");
    setup_git_repo();

    create_file("a.txt", "a\n");
    system("git add a.txt && git commit -q -m 'initial'");

    aegis_tool_args_t* args = NULL;
    expect_ok(aegis_tool_args_create(&args), "create args");
    expect_ok(aegis_tool_args_add_string(args, "action", "create"), "add action");
    expect_ok(aegis_tool_args_add_string(args, "name", "a; rm -rf /"), "add unsafe name");

    aegis_tool_result_t result = {0};
    aegis_status_t      st     = aegis_coding_tool_git_branch.execute(NULL, args, NULL, &result);
    expect_ok(st, "git_branch should return OK with error");

    assert(result.value.type == AEGIS_TOOL_VAL_STRING);
    assert(strstr(result.value.as.str.ptr, "error") != NULL);
    assert(strstr(result.value.as.str.ptr, "invalid") != NULL);

    aegis_tool_result_destroy(&result);
    aegis_tool_args_destroy(args);
    cleanup_git_repo();
    printf("  git_branch_create_rejects_unsafe_name PASS\n");
}

static void test_git_branch_switch(void)
{
    printf("[test] git_branch_switch ...\n");
    setup_git_repo();

    create_file("a.txt", "a\n");
    system("git add a.txt && git commit -q -m 'initial'");
    system("git branch dev");

    aegis_tool_args_t* args = NULL;
    expect_ok(aegis_tool_args_create(&args), "create args");
    expect_ok(aegis_tool_args_add_string(args, "action", "switch"), "add action");
    expect_ok(aegis_tool_args_add_string(args, "name", "dev"), "add name");

    aegis_tool_result_t result = {0};
    aegis_status_t      st     = aegis_coding_tool_git_branch.execute(NULL, args, NULL, &result);
    expect_ok(st, "git_branch switch");

    assert(result.value.type == AEGIS_TOOL_VAL_STRING);
    assert(strstr(result.value.as.str.ptr, "Switched to branch") != NULL);

    aegis_tool_result_destroy(&result);
    aegis_tool_args_destroy(args);
    cleanup_git_repo();
    printf("  git_branch_switch PASS\n");
}

static void test_git_status_not_a_repo(void)
{
    printf("[test] git_status_not_a_repo ...\n");
    char tmpdir[PATH_MAX];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/aegis_git_norepo_XXXXXX");
    assert(mkdtemp(tmpdir) != NULL);
    assert(chdir(tmpdir) == 0);

    aegis_tool_result_t result = {0};
    aegis_status_t      st     = aegis_coding_tool_git_status.execute(NULL, NULL, NULL, &result);
    expect_ok(st, "git_status should return OK (with error in result)");

    assert(result.value.type == AEGIS_TOOL_VAL_STRING);
    assert(result.value.as.str.ptr != NULL);
    assert(strlen(result.value.as.str.ptr) > 0);

    aegis_tool_result_destroy(&result);

    char cmd[PATH_MAX + 32];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", tmpdir);
    system(cmd);
    printf("  git_status_not_a_repo PASS\n");
}

static void test_git_tools_register_all(void)
{
    printf("[test] git_tools_register_all ...\n");
    aegis_tool_registry_t* reg = NULL;
    expect_ok(aegis_tool_registry_create(&reg), "create registry");

    expect_ok(aegis_coding_git_tools_register_all(reg), "register git tools");

    assert(aegis_tool_registry_count(reg) == 5);

    /* Verify each tool is findable */
    aegis_tool_def_t found;
    expect_ok(aegis_tool_registry_find(reg, "git_status", &found), "find git_status");
    assert(strcmp(found.name, "git_status") == 0);
    assert(found.description != NULL);
    assert(found.execute != NULL);

    expect_ok(aegis_tool_registry_find(reg, "git_diff", &found), "find git_diff");
    assert(strcmp(found.name, "git_diff") == 0);

    expect_ok(aegis_tool_registry_find(reg, "git_commit", &found), "find git_commit");
    assert(strcmp(found.name, "git_commit") == 0);

    expect_ok(aegis_tool_registry_find(reg, "git_log", &found), "find git_log");
    assert(strcmp(found.name, "git_log") == 0);

    expect_ok(aegis_tool_registry_find(reg, "git_branch", &found), "find git_branch");
    assert(strcmp(found.name, "git_branch") == 0);

    /* Not found */
    assert(aegis_tool_registry_find(reg, "nonexistent", &found) == AEGIS_ERR_NOT_FOUND);

    aegis_tool_registry_destroy(reg);
    printf("  git_tools_register_all PASS\n");
}

/* ── Main ──────────────────────────────────────────────────────────────── */

int main(void)
{
    test_git_status_basic();
    test_git_status_with_path();
    test_git_diff_working();
    test_git_diff_staged();
    test_git_commit_basic();
    test_git_commit_specific_files();
    test_git_commit_rejects_unsafe_message();
    test_git_commit_rejects_unsafe_file();
    test_git_log_basic();
    test_git_log_by_path();
    test_git_log_format_oneline();
    test_git_branch_list();
    test_git_branch_create();
    test_git_branch_create_rejects_unsafe_name();
    test_git_branch_switch();
    test_git_status_not_a_repo();
    test_git_tools_register_all();

    printf("ALL_GIT_TOOLS_TESTS PASSED\n");
    return 0;
}
