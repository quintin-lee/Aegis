/**
 * @file test_plugin.c
 * @brief Unit tests for the Plugin loading subsystem.
 */
#include "aegis/plugin/plugin.h"
#include "aegis/status.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

/* ── Helpers ───────────────────────────────────────────────────────────────── */

static void expect_ok(aegis_status_t rc, const char* msg)
{
    assert(rc == AEGIS_OK);
    (void)msg;
}

/* ── Tests ─────────────────────────────────────────────────────────────────── */

static void test_null_safety(void)
{
    assert(aegis_plugin_load(NULL, NULL) == AEGIS_ERR_INVALID);
    assert(aegis_plugin_load("", NULL) == AEGIS_ERR_INVALID);
    assert(aegis_plugin_unload(NULL) == AEGIS_OK);
    assert(aegis_plugin_manifest(NULL) == NULL);
    assert(aegis_plugin_path(NULL) == NULL);
    assert(aegis_plugin_count() == 0);
    assert(aegis_plugin_at(0) == NULL);
}

static void test_load_mock_plugin(void)
{
    const char* path = "tests/plugin/mock_plugin.so";
    struct stat st;
    if (stat(path, &st) != 0) {
        fprintf(stderr, "[SKIP] mock_plugin.so not found, build it first:\n");
        fprintf(stderr,
                "  gcc -shared -fPIC -o tests/plugin/mock_plugin.so "
                "tests/plugin/plugin_mock.c -Iinclude -Isrc/internal -laegis_core\n");
        return;
    }

    aegis_plugin_t* p = NULL;
    expect_ok(aegis_plugin_load(path, &p), "load mock");
    assert(p != NULL);
    assert(aegis_plugin_manifest(p) != NULL);
    assert(strcmp(aegis_plugin_manifest(p)->name, "mock-plugin") == 0);
    assert(aegis_plugin_manifest(p)->abi_version == AEGIS_PLUGIN_ABI_VERSION);
    assert(aegis_plugin_path(p) != NULL);

    aegis_plugin_unload(p);
}

static void test_plugin_lifecycle(void)
{
    const char* path = "tests/plugin/mock_plugin.so";
    struct stat st;
    if (stat(path, &st) != 0) {
        return;
    }

    aegis_plugin_t* p = NULL;
    expect_ok(aegis_plugin_load(path, &p), "load");
    assert(aegis_plugin_count() == 1);
    assert(aegis_plugin_at(0) == p);

    expect_ok(aegis_plugin_unload(p), "unload");
    assert(aegis_plugin_count() == 0);
    assert(aegis_plugin_at(0) == NULL);
}

static void test_missing_file(void)
{
    aegis_plugin_t* p = NULL;
    assert(aegis_plugin_load("/nonexistent/path.so", &p) == AEGIS_ERR_NOT_FOUND);
    assert(p == NULL);
}

/* ── Main ──────────────────────────────────────────────────────────────────── */

int main(void)
{
    test_null_safety();
    test_load_mock_plugin();
    test_plugin_lifecycle();
    test_missing_file();

    printf("plugin: all tests passed\n");
    return 0;
}
