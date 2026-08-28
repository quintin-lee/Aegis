/**
 * @file plugin.c
 * @brief Plugin loading subsystem implementation.
 */
#define _POSIX_C_SOURCE 200809L
#include "aegis/plugin/plugin.h"
#include "aegis/status.h"
#include "aegis/provider/provider.h"
#include "aegis/strategy/strategy.h"
#include "aegis/tool/tool.h"

#include "plugin_internal.h"
#include "lifecycle.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <pthread.h>

/* ── Global state ──────────────────────────────────────────────────────────── */

aegis_plugin_t* g_plugins[AEGIS_PLUGIN_MAX_PLUGINS];
int             g_n_plugins    = 0;
pthread_mutex_t g_plugins_lock = PTHREAD_MUTEX_INITIALIZER;

/* ── Validation ────────────────────────────────────────────────────────────── */

aegis_status_t aegis_plugin_validate_manifest(const aegis_plugin_manifest_t* m)
{
    if (!m || !m->name || m->name[0] == '\0') {
        return AEGIS_ERR_INVALID;
    }
    if (m->abi_version != AEGIS_PLUGIN_ABI_VERSION) {
        return AEGIS_ERR_INVALID;
    }
    if (m->struct_sizes[AEGIS_PLUGIN_STRUCT_PROVIDER_DEF] != sizeof(aegis_provider_def_t)) {
        return AEGIS_ERR_INVALID;
    }
    if (m->struct_sizes[AEGIS_PLUGIN_STRUCT_STRATEGY_DEF] != sizeof(aegis_strategy_def_t)) {
        return AEGIS_ERR_INVALID;
    }
    if (m->struct_sizes[AEGIS_PLUGIN_STRUCT_TOOL_DEF] != sizeof(aegis_tool_def_t)) {
        return AEGIS_ERR_INVALID;
    }
    return AEGIS_OK;
}

/* ── Table management ──────────────────────────────────────────────────────── */

aegis_status_t aegis_plugin_table_add(aegis_plugin_t* p)
{
    pthread_mutex_lock(&g_plugins_lock);
    if (g_n_plugins >= AEGIS_PLUGIN_MAX_PLUGINS) {
        pthread_mutex_unlock(&g_plugins_lock);
        return AEGIS_ERR_BUSY;
    }
    g_plugins[g_n_plugins++] = p;
    pthread_mutex_unlock(&g_plugins_lock);
    return AEGIS_OK;
}

void aegis_plugin_table_remove(aegis_plugin_t* p)
{
    pthread_mutex_lock(&g_plugins_lock);
    for (int i = 0; i < g_n_plugins; i++) {
        if (g_plugins[i] == p) {
            g_plugins[i]               = g_plugins[g_n_plugins - 1];
            g_plugins[g_n_plugins - 1] = NULL;
            g_n_plugins--;
            break;
        }
    }
    pthread_mutex_unlock(&g_plugins_lock);
}

/* ── Public API ────────────────────────────────────────────────────────────── */

aegis_status_t aegis_plugin_load(const char* path, aegis_plugin_t** out)
{
    AEGIS_CHECK_OUT(out);
    *out = NULL;

    if (!path || path[0] == '\0') {
        return AEGIS_ERR_INVALID;
    }

    void* handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        fprintf(stderr, "[PLUGIN] dlopen(%s) failed: %s\n", path, dlerror());
        return AEGIS_ERR_NOT_FOUND;
    }

    typedef const aegis_plugin_manifest_t* (*manifest_fn)(void);
    manifest_fn get_manifest = NULL;
    *(void**)(&get_manifest) = dlsym(handle, AEGIS_PLUGIN_MANIFEST_FN);
    if (!get_manifest) {
        fprintf(stderr, "[PLUGIN] missing symbol %s: %s\n", AEGIS_PLUGIN_MANIFEST_FN, dlerror());
        dlclose(handle);
        return AEGIS_ERR_NOT_FOUND;
    }

    const aegis_plugin_manifest_t* m = get_manifest();
    if (!m) {
        dlclose(handle);
        return AEGIS_ERR_INVALID;
    }

    aegis_status_t rc = aegis_plugin_validate_manifest(m);
    if (rc != AEGIS_OK) {
        fprintf(stderr, "[PLUGIN] manifest validation failed: %s\n", aegis_status_str(rc));
        dlclose(handle);
        return rc;
    }

    aegis_plugin_t* p = calloc(1, sizeof(*p));
    if (!p) {
        dlclose(handle);
        return AEGIS_ERR_NOMEM;
    }
    strncpy(p->path, path, sizeof(p->path) - 1);
    p->path[sizeof(p->path) - 1] = '\0';
    p->manifest                  = *m;
    p->handle                    = handle;
    p->initialized               = 0;

    typedef aegis_status_t (*init_fn_type)(void**);
    typedef void (*shutdown_fn_type)(void*);
    init_fn_type     init_sym     = NULL;
    shutdown_fn_type shutdown_sym = NULL;
    *(void**)(&init_sym)          = dlsym(handle, AEGIS_PLUGIN_INIT_FN);
    *(void**)(&shutdown_sym)      = dlsym(handle, AEGIS_PLUGIN_SHUTDOWN_FN);
    if (init_sym) {
        *(void**)(&p->init_fn) = *(void**)(&init_sym);
    }
    if (shutdown_sym) {
        *(void**)(&p->shutdown_fn) = *(void**)(&shutdown_sym);
    }

    if (init_sym) {
        rc = init_sym(&p->user_ctx);
        if (rc != AEGIS_OK) {
            fprintf(stderr, "[PLUGIN] init failed: %s\n", aegis_status_str(rc));
            free(p);
            dlclose(handle);
            return rc;
        }
        p->initialized = 1;
    }

    rc = aegis_plugin_table_add(p);
    if (rc != AEGIS_OK) {
        if (shutdown_sym) {
            shutdown_sym(p->user_ctx);
        }
        free(p);
        dlclose(handle);
        return rc;
    }

    *out = p;
    return AEGIS_OK;
}

aegis_status_t aegis_plugin_unload(aegis_plugin_t* plugin)
{
    if (!plugin) {
        return AEGIS_OK;
    }

    if (plugin->initialized && plugin->shutdown_fn) {
        typedef void (*shutdown_fn_type)(void*);
        shutdown_fn_type fn = NULL;
        *(void**)(&fn)      = plugin->shutdown_fn;
        fn(plugin->user_ctx);
        plugin->initialized = 0;
    }

    aegis_plugin_table_remove(plugin);
    plugin->user_ctx = NULL;

    if (plugin->handle) {
        dlclose(plugin->handle);
        plugin->handle = NULL;
    }

    free(plugin);
    return AEGIS_OK;
}

const aegis_plugin_manifest_t* aegis_plugin_manifest(const aegis_plugin_t* plugin)
{
    return plugin ? &plugin->manifest : NULL;
}

const char* aegis_plugin_path(const aegis_plugin_t* plugin)
{
    return plugin ? plugin->path : NULL;
}

size_t aegis_plugin_count(void)
{
    int n;
    pthread_mutex_lock(&g_plugins_lock);
    n = g_n_plugins;
    pthread_mutex_unlock(&g_plugins_lock);
    return (size_t)n;
}

aegis_plugin_t* aegis_plugin_at(size_t idx)
{
    pthread_mutex_lock(&g_plugins_lock);
    aegis_plugin_t* p = (idx < (size_t)g_n_plugins) ? g_plugins[idx] : NULL;
    pthread_mutex_unlock(&g_plugins_lock);
    return p;
}
