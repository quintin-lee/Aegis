/**
 * @file plugin_mock.c
 * @brief Mock plugin shared object for testing the plugin loader.
 *
 * Compiled as a standalone .so with:
 *   gcc -shared -fPIC -o tests/plugin/mock_plugin.so tests/plugin/plugin_mock.c \
 *       -Iinclude -Isrc/internal -laegis_core
 */
#include "aegis/plugin.h"
#include "aegis/provider.h"
#include "aegis/strategy.h"
#include "aegis/tool.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ── Mock state ────────────────────────────────────────────────────────────── */

static int g_init_count = 0;
static int g_shutdown_count = 0;
static void* g_user_ctx = NULL;

/* ── Manifest ──────────────────────────────────────────────────────────────── */

static const aegis_plugin_manifest_t g_manifest = {
    .name          = "mock-plugin",
    .abi_version   = AEGIS_PLUGIN_ABI_VERSION,
    .struct_sizes  = {
        sizeof(aegis_provider_def_t),
        sizeof(aegis_strategy_def_t),
        sizeof(aegis_tool_def_t),
    },
    .capabilities  = (uint32_t)AEGIS_CAP_READ_FILE,
};

const aegis_plugin_manifest_t* get_plugin_manifest(void)
{
    return &g_manifest;
}

/* ── Lifecycle ─────────────────────────────────────────────────────────────── */

aegis_status_t aegis_plugin_init(void** out)
{
    if (!out) return AEGIS_ERR_INVALID;
    g_init_count++;
    g_user_ctx = malloc(1);
    *out = g_user_ctx;
    return AEGIS_OK;
}

void aegis_plugin_shutdown(void* ctx)
{
    (void)ctx;
    g_shutdown_count++;
    if (g_user_ctx) {
        free(g_user_ctx);
        g_user_ctx = NULL;
    }
}

/* ── Exposed counters ──────────────────────────────────────────────────────── */

int aegis_mock_plugin_init_count(void) { return g_init_count; }
int aegis_mock_plugin_shutdown_count(void) { return g_shutdown_count; }
