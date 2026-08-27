/**
 * @file plugin_internal.h
 * @warning This header must NOT be included from public code.
 */
#ifndef AEGIS_INTERNAL_PLUGIN_H
#define AEGIS_INTERNAL_PLUGIN_H

#include "aegis/plugin.h"

#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>
#include <stddef.h>

/* ── Constants ─────────────────────────────────────────────────────────────── */

#define AEGIS_PLUGIN_MAX_PATH 512
#define AEGIS_PLUGIN_MAX_PLUGINS 64

/* Manifest symbol name that every plugin must export. */
#define AEGIS_PLUGIN_MANIFEST_FN "aegis_plugin_manifest"
/* Init/shutdown symbols. */
#define AEGIS_PLUGIN_INIT_FN     "aegis_plugin_init"
#define AEGIS_PLUGIN_SHUTDOWN_FN "aegis_plugin_shutdown"

/* ── Struct size indices ──────────────────────────────────────────────────── */

enum {
    AEGIS_PLUGIN_STRUCT_PROVIDER_DEF = 0,
    AEGIS_PLUGIN_STRUCT_STRATEGY_DEF = 1,
    AEGIS_PLUGIN_STRUCT_TOOL_DEF     = 2,
};

/* ── Plugin handle ─────────────────────────────────────────────────────────── */

struct aegis_plugin {
    char                    path[AEGIS_PLUGIN_MAX_PATH];
    aegis_plugin_manifest_t manifest;
    void*                   handle;
    void*                   init_fn;
    void*                   shutdown_fn;
    void*                   user_ctx;
    int                     initialized;
};

/* ── Global state ──────────────────────────────────────────────────────────── */

extern aegis_plugin_t* g_plugins[AEGIS_PLUGIN_MAX_PLUGINS];
extern int             g_n_plugins;
extern pthread_mutex_t g_plugins_lock;

/* ── Internal helpers ──────────────────────────────────────────────────────── */

aegis_status_t aegis_plugin_validate_manifest(const aegis_plugin_manifest_t* m);
aegis_status_t aegis_plugin_table_add(aegis_plugin_t* p);
void           aegis_plugin_table_remove(aegis_plugin_t* p);

#endif /* AEGIS_INTERNAL_PLUGIN_H */
