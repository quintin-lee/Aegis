#ifndef AEGIS_PROVIDER_H
#define AEGIS_PROVIDER_H

#include "aegis/common/cancellation/cancellation.h"
#include "aegis/types.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file provider.h
 * @brief Provider abstraction: stable ABI, lifecycle, and registry.
 *
 * A provider is an adapter to an external service (LLM API, embedding
 * model, storage backend, ...). Core never depends on a concrete vendor;
 * concrete integrations are delivered as separately registered providers.
 *
 * ## ABI stability
 * Every provider definition carries @c abi_version. The registry rejects
 * definitions whose version does not match AEGIS_PROVIDER_ABI_VERSION, so
 * binaries built against mismatched ABI generations fail fast at
 * registration instead of corrupting memory at dispatch time. The version
 * must be bumped whenever the layout of any public provider structure or
 * callback signature changes.
 *
 * ## Ownership
 * - The registry owns its internal copy of each definition (shallow copy).
 *   The strings pointed to by @c name and @c description are BORROWED and
 *   must outlive the registration; free them only after unregister.
 * - @c user is BORROWED and passed through to every callback verbatim.
 * - Registry lookup returns a def VALUE COPY — callers never hold interior
 *   pointers into registry storage.
 *
 * ## Lifecycle
 * REGISTERED -> (init) -> INITIALIZED -> (shutdown) -> REGISTERED.
 * - init/shutdown callbacks run without registry locks held.
 * - init failure leaves the provider REGISTERED and propagates the rc.
 * - shutdown is idempotent (a second call is a documented no-op).
 * - unregistering an INITIALIZED provider shuts it down first.
 * - destroying the registry shuts down every still-INITIALIZED provider.
 *
 * ## Thread safety
 * - The registry is thread-safe for register/find/unregister/count/state.
 * - init/shutdown/dispatch may be called concurrently; state transitions
 *   are serialized by the registry lock and callbacks run lock-free.
 * - Whether one provider instance tolerates concurrent dispatch is declared
 *   by @c thread_model and must be honored by callers (serialize externally
 *   for AEGIS_PROVIDER_SINGLE_THREAD providers).
 *
 * Lock order (global): provider registry lock is a LEAF lock — it never
 * nests another Aegis lock inside it.
 */

/** ABI generation this header implements. */
#define AEGIS_PROVIDER_ABI_VERSION 1u

/** Service class advertised by a provider definition. */
typedef enum aegis_provider_kind {
    AEGIS_PROVIDER_GENERIC   = 0, /**< Unclassified service adapter.            */
    AEGIS_PROVIDER_LLM       = 1, /**< Text completion / chat backend.          */
    AEGIS_PROVIDER_EMBEDDING = 2, /**< Vector embedding backend.               */
    AEGIS_PROVIDER_STORAGE   = 3  /**< Key/value blob storage backend.          */
} aegis_provider_kind_t;

/** Lifecycle state of a registered provider (query via aegis_provider_state). */
typedef enum aegis_provider_state {
    AEGIS_PROVIDER_REGISTERED  = 0, /**< Registered, not initialized.         */
    AEGIS_PROVIDER_INITIALIZED = 1  /**< init() succeeded; dispatch allowed.  */
} aegis_provider_state_t;

/**
 * @brief Concurrency contract declared by a provider implementation.
 *
 * The registry does not enforce this — it is metadata for callers that
 * fan out work across threads.
 */
typedef enum aegis_provider_thread_model {
    AEGIS_PROVIDER_SINGLE_THREAD = 0, /**< Dispatch must be serialized externally. */
    AEGIS_PROVIDER_THREAD_SAFE   = 1  /**< Concurrent dispatch is supported.       */
} aegis_provider_thread_model_t;

/**
 * @brief Lifecycle callback: prepare the provider for dispatch.
 *
 * Runs without registry locks held. May be NULL for stateless providers.
 *
 * @param user  The def's user pointer (borrowed).
 * @return AEGIS_OK on success; on failure the provider stays REGISTERED
 *         and the status is propagated to aegis_provider_init().
 */
typedef aegis_status_t (*aegis_provider_init_fn)(void* user);

/**
 * @brief Lifecycle callback: release provider resources.
 *
 * Runs without registry locks held. Must leave the provider able to be
 * init()'ed again. May be NULL.
 *
 * @param user  The def's user pointer (borrowed).
 */
typedef void (*aegis_provider_shutdown_fn)(void* user);

/**
 * @brief Provider definition (registration input).
 *
 * The registry shallow-copies this struct. Keep name/description pointers
 * valid for the lifetime of the registration.
 */
typedef struct aegis_provider_def {
    const char*                   name;         /**< Unique key (required, non-empty). */
    const char*                   description;  /**< Human-readable text (may be NULL).*/
    uint32_t                      abi_version;  /**< Must equal AEGIS_PROVIDER_ABI_VERSION. */
    aegis_provider_kind_t         kind;         /**< Service class.                    */
    aegis_capability_t            capabilities; /**< Capability bitmask required.      */
    aegis_provider_thread_model_t thread_model; /**< Concurrency contract.             */
    aegis_provider_init_fn        init;         /**< Optional lifecycle enter.         */
    aegis_provider_shutdown_fn    shutdown;     /**< Optional lifecycle exit.          */
    void*                         user;         /**< Borrowed callback context.        */
} aegis_provider_def_t;

/** Opaque provider registry handle. */
typedef struct aegis_provider_registry aegis_provider_registry_t;

/** Copied definition returned by lookup (value semantics; no interior pointers). */
typedef struct aegis_provider_view {
    aegis_provider_def_t   def;   /**< Shallow copy of the stored definition. */
    aegis_provider_state_t state; /**< Current lifecycle state.               */
} aegis_provider_view_t;

/* ── Registry ─────────────────────────────────────────────────────────────── */

/**
 * @brief Create an empty provider registry.
 *
 * @param[out] out  Receives the handle. Ownership: transferred.
 * @return AEGIS_OK, AEGIS_ERR_INVALID (NULL out), AEGIS_ERR_NOMEM.
 */
aegis_status_t aegis_provider_registry_create(aegis_provider_registry_t** out);

/**
 * @brief Destroy the registry.
 *
 * Shuts down every still-INITIALIZED provider first (shutdown callbacks
 * run under no other lock), then frees all owned entries.
 *
 * Safe to call with NULL (no-op).
 */
void aegis_provider_registry_destroy(aegis_provider_registry_t* reg);

/**
 * @brief Register a provider definition.
 *
 * @param reg   Registry (borrowed).
 * @param def   Definition to copy (borrowed). Validated: non-NULL, non-empty
 *              name, abi_version == AEGIS_PROVIDER_ABI_VERSION.
 * @return AEGIS_OK, AEGIS_ERR_INVALID, AEGIS_ERR_BUSY (name taken),
 *         AEGIS_ERR_NOMEM.
 */
aegis_status_t aegis_provider_register(aegis_provider_registry_t*  reg,
                                       const aegis_provider_def_t* def);

/**
 * @brief Look up a provider by name.
 *
 * @param[out] view  Receives a copy of the stored definition plus current
 *                   state (borrowed output buffer, required).
 * @return AEGIS_OK, AEGIS_ERR_INVALID, AEGIS_ERR_NOT_FOUND.
 */
aegis_status_t aegis_provider_find(const aegis_provider_registry_t* reg, const char* name,
                                   aegis_provider_view_t* view);

/**
 * @brief Unregister a provider.
 *
 * If the provider is INITIALIZED its shutdown callback runs first.
 * Re-registering the same name afterwards is legal.
 *
 * @return AEGIS_OK, AEGIS_ERR_INVALID, AEGIS_ERR_NOT_FOUND,
 *         or whatever the shutdown path reports (it cannot fail; kept
 *         aegis_status_t for future lifecycle hooks).
 */
aegis_status_t aegis_provider_unregister(aegis_provider_registry_t* reg, const char* name);

/**
 * @brief Number of registered providers (NULL reg → 0).
 */
size_t aegis_provider_count(const aegis_provider_registry_t* reg);

/* ── Lifecycle ────────────────────────────────────────────────────────────── */

/**
 * @brief Initialize a registered provider (REGISTERED -> INITIALIZED).
 *
 * @return AEGIS_OK, AEGIS_ERR_INVALID, AEGIS_ERR_NOT_FOUND,
 *         AEGIS_ERR_BUSY (already initialized), or the init callback's
 *         status verbatim (provider stays REGISTERED).
 */
aegis_status_t aegis_provider_init(aegis_provider_registry_t* reg, const char* name);

/**
 * @brief Shut down an initialized provider (INITIALIZED -> REGISTERED).
 *
 * Idempotent for lifecycle purposes: shutting down a provider that is
 * already REGISTERED returns AEGIS_OK without invoking any callback.
 *
 * @return AEGIS_OK, AEGIS_ERR_INVALID, AEGIS_ERR_NOT_FOUND.
 */
aegis_status_t aegis_provider_shutdown(aegis_provider_registry_t* reg, const char* name);

#ifdef __cplusplus
}
#endif

#endif /* AEGIS_PROVIDER_H */
