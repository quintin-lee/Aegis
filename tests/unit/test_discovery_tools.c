#define _POSIX_C_SOURCE 200809L
#include "aegis/coding/discovery_tools.h"
#include "aegis/common/cancellation/cancellation.h"
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

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
    char  tmpl[] = "/tmp/aegis_disc_XXXXXX";
    char* dir    = mkdtemp(tmpl);
    assert(dir);
    char path[512];
    snprintf(path, sizeof(path), "%s/sub", dir);
    assert(mkdir(path, 0755) == 0 || errno == EEXIST);
    snprintf(path, sizeof(path), "%s/sub/deep", dir);
    assert(mkdir(path, 0755) == 0 || errno == EEXIST);
    snprintf(path, sizeof(path), "%s/.git", dir);
    assert(mkdir(path, 0755) == 0 || errno == EEXIST);

    FILE* f;
    snprintf(path, sizeof(path), "%s/a.txt", dir);
    f = fopen(path, "w");
    assert(f);
    fputs("hello world\nsecond line\n", f);
    fclose(f);
    snprintf(path, sizeof(path), "%s/bin.dat", dir);
    f = fopen(path, "wb");
    assert(f);
    fwrite("abc\0def", 1, 7, f);
    fclose(f);
    snprintf(path, sizeof(path), "%s/sub/b.c", dir);
    f = fopen(path, "w");
    assert(f);
    fputs("int main() { return 0; }\n", f);
    fclose(f);
    snprintf(path, sizeof(path), "%s/sub/deep/c.h", dir);
    f = fopen(path, "w");
    assert(f);
    fputs("#pragma once\n", f);
    fclose(f);
    snprintf(path, sizeof(path), "%s/.git/x.txt", dir);
    f = fopen(path, "w");
    assert(f);
    fputs("ignored\n", f);
    fclose(f);
    return strdup(dir);
}

static aegis_status_t call_tool(aegis_tool_registry_t* reg, const char* name, const char* key1,
                                const char* val1, const char* key2, const char* val2,
                                char** out_text)
{
    aegis_tool_args_t* args = NULL;
    aegis_status_t     st   = aegis_tool_args_create(&args);
    if (st != AEGIS_OK) {
        return st;
    }
    if (val1) {
        aegis_tool_args_add_string(args, key1, val1);
    }
    if (val2) {
        aegis_tool_args_add_string(args, key2, val2);
    }
    aegis_tool_result_t result = {0};
    st                         = aegis_tool_call(reg, name, args, 5000, &result);
    if (st == AEGIS_OK) {
        *out_text = strdup(result.value.type == AEGIS_TOOL_VAL_STRING && result.value.as.str.ptr
                               ? result.value.as.str.ptr
                               : "");
    }
    aegis_tool_result_destroy(&result);
    aegis_tool_args_destroy(args);
    return st;
}

static void cleanup_fixture(char* dir, char* cwdbuf)
{
    if (cwdbuf) {
        assert(chdir(cwdbuf) == 0);
        free(cwdbuf);
    }
    char rm[1024];
    snprintf(rm, sizeof(rm), "rm -rf %s", dir);
    (void)system(rm);
    free(dir);
}

static void test_list(void)
{
    printf("[test] list ...\n");
    char*  dir    = make_fixture();
    char*  cwdbuf = getcwd(NULL, 0);
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

    /* default path: list with no args works */
    expect_ok(call_tool(reg, "list", NULL, NULL, NULL, NULL, &out), "list default");
    assert(strstr(out, "a.txt\tfile\t"));
    free(out);

    /* path traversal rejected */
    expect_ok(call_tool(reg, "list", "path", "../", NULL, NULL, &out), "list ..");
    assert(strstr(out, "error:"));
    free(out);

    aegis_tool_registry_destroy(reg);
    cleanup_fixture(dir, cwdbuf);
    printf("  PASS\n");
}

static void test_glob(void)
{
    printf("[test] glob ...\n");
    char*  dir    = make_fixture();
    char*  cwdbuf = getcwd(NULL, 0);
    assert(chdir(dir) == 0);
    aegis_tool_registry_t* reg = NULL;
    expect_ok(aegis_tool_registry_create(&reg), "reg");
    expect_ok(aegis_coding_discovery_tools_register_all(reg), "register");

    char* out = NULL;
    expect_ok(call_tool(reg, "glob", "pattern", "*.c", NULL, NULL, &out), "glob *.c");
    assert(strstr(out, "sub/b.c"));
    assert(!strstr(out, ".git"));
    free(out);

    expect_ok(call_tool(reg, "glob", "pattern", "sub/deep/*.h", NULL, NULL, &out), "glob deep");
    assert(strstr(out, "sub/deep/c.h"));
    free(out);

    /* traversal root rejected */
    expect_ok(call_tool(reg, "glob", "pattern", "*.c", "path", "..", &out), "glob ..");
    assert(strstr(out, "error:"));
    free(out);

    /* missing pattern rejected at schema validation */
    assert(call_tool(reg, "glob", NULL, NULL, NULL, NULL, &out) == AEGIS_ERR_INVALID);

    aegis_tool_registry_destroy(reg);
    cleanup_fixture(dir, cwdbuf);
    printf("  PASS\n");
}

static void test_grep(void)
{
    printf("[test] grep ...\n");
    char*  dir    = make_fixture();
    char*  cwdbuf = getcwd(NULL, 0);
    assert(chdir(dir) == 0);
    aegis_tool_registry_t* reg = NULL;
    expect_ok(aegis_tool_registry_create(&reg), "reg");
    expect_ok(aegis_coding_discovery_tools_register_all(reg), "register");

    char* out = NULL;
    expect_ok(call_tool(reg, "grep", "pattern", "main", "include", "*.c", &out),
              "grep main");
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
    expect_ok(call_tool(reg, "grep", "pattern", "x", "path", "..", &out), "grep ..");
    assert(strstr(out, "error:"));
    free(out);

    /* cancelled token: aegis_tool_execute passes the caller's token through */
    aegis_cancellation_token_t* tok = NULL;
    expect_ok(aegis_cancellation_token_create(&tok), "tok");
    aegis_cancellation_token_request_cancel(tok);
    aegis_tool_args_t* args = NULL;
    expect_ok(aegis_tool_args_create(&args), "args");
    aegis_tool_args_add_string(args, "pattern", "x");
    aegis_tool_result_t result = {0};
    assert(aegis_tool_execute(reg, "grep", args, tok, &result) == AEGIS_ERR_CANCELLED);
    aegis_tool_result_destroy(&result);
    aegis_tool_args_destroy(args);
    aegis_cancellation_token_destroy(tok);

    aegis_tool_registry_destroy(reg);
    cleanup_fixture(dir, cwdbuf);
    printf("  PASS\n");
}

static void test_glob_truncation(void)
{
    printf("[test] glob_truncation ...\n");
    char* dir = make_fixture();
    char  path[512];
    for (int i = 0; i < 210; i++) {
        snprintf(path, sizeof(path), "%s/f%03d.txt", dir, i);
        FILE* f = fopen(path, "w");
        assert(f);
        fputs("x\n", f);
        fclose(f);
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
    cleanup_fixture(dir, cwdbuf);
    printf("  PASS\n");
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
