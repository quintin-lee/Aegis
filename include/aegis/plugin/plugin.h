/**
 * @file plugin.h
 * @brief Plugin loading subsystem with ABI validation and lifecycle management.
 *
 * Plugins are dynamically loaded shared objects (.so) that provide
 * providers, tools, and strategies to the Aegis runtime without
 * recompiling the core.
 *
 * ## ABI Stability
 * Every exported symbol from a plugin is gated behind a manifest that
 * carries:
 *   - abi_version: must match AEGIS_PLUGIN_ABI_VERSION at load time
 *   - struct_sizes: sizeof() of every public struct the plugin touches
 *     (mismatch means memory layout drift → reject)
 *   - capabilities: bitmask declared by the plugin
 *
 * Loading a plugin with mismatched ABI or struct sizes fails fast with
 * AEGIS_ERR_INVALID before any code from the plugin executes.
 *
 * ## Ownership
 * - aegis_plugin_load() transfers ownership of the plugin handle to the
 *   caller; aegis_plugin_unload() consumes it.
 * - The loader does NOT free the plugin library (dlclose is deferred
 *   until the caller calls unload).
 * - Provider/tool/strategy objects produced by a plugin must NOT be used
 *   after unload — this is the caller's responsibility.
 *
 * ## Thread Safety
 * - aegis_plugin_load/unload are NOT thread-safe against concurrent
 *   access to the same plugin path.
 * - The global plugin table supports concurrent reads.
 */
#ifndef AEGIS_PLUGIN_H
#define AEGIS_PLUGIN_H

#include "aegis/types.h"

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Current ABI version. Bump on breaking changes to this header. */
#define AEGIS_PLUGIN_ABI_VERSION 1u

/** Maximum number of struct sizes tracked in the manifest. */
#define AEGIS_PLUGIN_MAX_STRUCT_SIZES 8

/* ── Manifest ──────────────────────────────────────────────────────────────── */

/**
 * @brief Plugin manifest — the gatekeeper inspected at load time.
 *
 * Every plugin MUST export a function named AEGIS_PLUGIN_MANIFEST_FN
 * that returns a pointer to an instance of this struct.
 *
 * The core checks:
 *   1. abi_version == AEGIS_PLUGIN_ABI_VERSION
 *   2. struct_sizes[i] == sizeof(the corresponding struct)
 *   3. name is non-NULL and non-empty
 *
 * Any check failure causes aegis_plugin_load() to return AEGIS_ERR_INVALID.
 */
typedef struct aegis_plugin_manifest {
    /** Plugin human-readable name (required, non-NULL, non-empty). */
    const char* name;
    /** Must equal AEGIS_PLUGIN_ABI_VERSION. */
    uint32_t abi_version;
    /**
     * Size-of-check array. Index mapping:
     *   0 = sizeof(aegis_provider_def_t)
     *   1 = sizeof(aegis_strategy_def_t)
     *   2 = sizeof(aegis_tool_def_t)
     *   3+ = reserved for future use.
     * Each entry must match the actual sizeof() or the plugin is rejected.
     */
    size_t struct_sizes[AEGIS_PLUGIN_MAX_STRUCT_SIZES];
    /** Capability bitmask this plugin requires (aegis_capability_t). */
    uint32_t capabilities;
} aegis_plugin_manifest_t;

/* ── Plugin handle ─────────────────────────────────────────────────────────── */

/** Opaque plugin handle. */
typedef struct aegis_plugin aegis_plugin_t;

/* ── Lifecycle ─────────────────────────────────────────────────────────────── */

/**
 * @brief Load a plugin from a shared object file.
 *
 * Steps:
 *   1. Open the .so via dlopen.
 *   2. Look up the manifest symbol.
 *   3. Validate abi_version and struct_sizes.
 *   4. Call the plugin's init function (if provided).
 *   5. Register into the global plugin table.
 *
 * On failure the plugin is unloaded and NULL is returned.
 *
 * @param path  Path to the .so file (borrowed, must remain valid).
 * @param[out] out Receives the plugin handle. Ownership: transferred.
 * @return AEGIS_OK on success, or:
 *         AEGIS_ERR_INVALID (bad manifest / ABI mismatch),
 *         AEGIS_ERR_NOT_FOUND (file not found / symbol missing),
 *         AEGIS_ERR_NOMEM.
 */
aegis_status_t aegis_plugin_load(const char* path, aegis_plugin_t** out);

/**
 * @brief Unload a plugin.
 *
 * Calls the plugin's shutdown function, removes it from the global
 * table, and calls dlclose. Any provider/tool/strategy objects that
 * were registered from this plugin become invalid — callers must not
 * use them after unload.
 *
 * Safe to call with NULL (no-op).
 *
 * @param plugin Handle to unload (ownership: consumed).
 * @return AEGIS_OK on success.
 */
aegis_status_t aegis_plugin_unload(aegis_plugin_t* plugin);

/**
 * @brief Get the manifest of a loaded plugin.
 *
 * @param plugin Plugin handle (borrowed).
 * @return Pointer to the manifest (borrowed, immutable); NULL if plugin is NULL.
 */
const aegis_plugin_manifest_t* aegis_plugin_manifest(const aegis_plugin_t* plugin);

/**
 * @brief Get the path from which a plugin was loaded.
 *
 * @param plugin Plugin handle (borrowed).
 * @return Path string (borrowed); NULL if plugin is NULL.
 */
const char* aegis_plugin_path(const aegis_plugin_t* plugin);

/* ── Global table ──────────────────────────────────────────────────────────── */

/**
 * @brief Number of currently loaded plugins.
 *
 * @return Plugin count.
 */
size_t aegis_plugin_count(void);

/**
 * @brief Get a loaded plugin by index.
 *
 * @param idx Zero-based index (must be < aegis_plugin_count()).
 * @return Plugin handle (borrowed); NULL if idx is out of range.
 */
aegis_plugin_t* aegis_plugin_at(size_t idx);

#ifdef __cplusplus
}
#endif

#endif /* AEGIS_PLUGIN_H */
