/**
 * @file security.h
 * @brief Security module: policy, permission, capability, sandbox.
 *
 * Execution pipeline (enforced, no bypass):
 *
 *   Tool Request
 *     → Schema Validation       (aegis_tool_validate_args)
 *     → Capability Check        (aegis_security_check_capabilities)
 *     → Policy Evaluation       (aegis_security_evaluate)
 *     → Permission Grant        (aegis_security_has_permission)
 *     → Sandbox Apply           (aegis_security_sandbox_apply)
 *     → Execute
 *
 * Design principles:
 *   - Default-deny: any capability not explicitly granted is blocked.
 *   - Policy decisions are always logged to the audit log (immutable, append-only).
 *   - Capabilities are bitmask flags defined in aegis/types.h (aegis_capability_t).
 *   - Tool registration requires a non-zero capabilities mask — a tool that
 *     declares no capabilities cannot perform any restricted operation.
 *   - Sandboxes are pluggable via aegis_security_sandbox_set_vtable().
 *     The built-in default sandbox is a no-op mock; a real Linux sandbox
 *     (namespaces, seccomp, chroot) is wired in later.
 */
#ifndef AEGIS_SECURITY_H
#define AEGIS_SECURITY_H

#include "aegis/types.h"
#include "aegis/executor/cancellation.h"

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Audit log ─────────────────────────────────────────────────────────────── */

/** Maximum number of audit entries retained in a ring buffer. */
#define AEGIS_SECURITY_AUDIT_MAX_ENTRIES 1024

/** Audit event type — what category of decision was made. */
typedef enum aegis_security_audit_type {
    AEGIS_SECURITY_AUDIT_CAP_CHECK,   /**< Capability requirement verified.          */
    AEGIS_SECURITY_AUDIT_POLICY_EVAL, /**< Policy rule evaluated.                     */
    AEGIS_SECURITY_AUDIT_PERMISSION,  /**< Permission granted or denied.              */
    AEGIS_SECURITY_AUDIT_SANDBOX,     /**< Sandbox restriction applied.               */
    AEGIS_SECURITY_AUDIT_DECISION,    /**< Final allow/deny decision.                 */
} aegis_security_audit_type_t;

/** Audit decision outcome. */
typedef enum aegis_security_audit_decision {
    AEGIS_SECURITY_AUDIT_ALLOW, /**< Action permitted.                          */
    AEGIS_SECURITY_AUDIT_DENY,  /**< Action denied.                             */
    AEGIS_SECURITY_AUDIT_INFO,  /**< Informational (no decision).               */
} aegis_security_audit_decision_t;

/**
 * @brief One entry in the security audit log.
 *
 * Immutable once written — entries are never modified or removed (ring
 * buffer overwrites the oldest when full).
 */
typedef struct aegis_security_audit_entry {
    uint64_t                        timestamp;    /**< Monotonic nanoseconds since epoch.   */
    aegis_security_audit_type_t     type;         /**< Category of event.                   */
    aegis_security_audit_decision_t decision;     /**< ALLOW / DENY / INFO.                 */
    aegis_capability_t              capability;   /**< Capability flag, or AEGIS_CAP_NONE.   */
    uint32_t                        rule_index;   /**< Index of rule evaluated (-1 if N/A).  */
    char                            message[128]; /**< Human-readable description.           */
} aegis_security_audit_entry_t;

/* ── Sandbox ───────────────────────────────────────────────────────────────── */

/**
 * @brief Sandbox context — opaque handle to the execution sandbox.
 *
 * The sandbox constrains what system operations a tool invocation may
 * perform (file paths, network, process spawn, etc.). The default
 * sandbox is a no-op mock; swap in a real implementation via
 * aegis_security_sandbox_set_vtable().
 */
typedef struct aegis_security_sandbox aegis_security_sandbox_t;

/* ── Policy ────────────────────────────────────────────────────────────────── */

/**
 * @brief Opaque security policy handle.
 *
 * A policy contains:
 *   - A set of named rules, each specifying which capabilities are
 *     required for a given tool name pattern.
 *   - An audit log recording every decision.
 *   - A sandbox attachment for runtime restriction.
 *
 * Policies are NOT thread-safe; external synchronization is required
 * for concurrent rule modification. Audit reads are safe.
 */
typedef struct aegis_security_policy aegis_security_policy_t;

/* ── Permission ─────────────────────────────────────────────────────────────── */

/**
 * @brief Opaque permission-check handle.
 *
 * A permission engine holds the current grant state derived from the
 * active policy. It answers: "given tool X with capabilities Y, is it
 * allowed?"  The result is always reflected in the policy's audit log.
 */
typedef struct aegis_security_permission aegis_security_permission_t;

/* ── Capability helper ───────────────────────────────────────────────────────── */

/**
 * @brief Return a human-readable comma-separated list of capability names
 *        for the bitmask @p caps.
 *
 * @param caps Capability bitmask (from aegis_capability_t).
 * @return Static string; do not free.
 */
const char* aegis_security_capabilities_str(aegis_capability_t caps);

/* ── Audit log access ───────────────────────────────────────────────────────── */

/**
 * @brief Get the number of entries currently in the audit log.
 *
 * @param policy Policy handle (borrowed).
 * @return Number of entries (0 ≤ count ≤ AEGIS_SECURITY_AUDIT_MAX_ENTRIES).
 */
size_t aegis_security_audit_count(const aegis_security_policy_t* policy);

/**
 * @brief Get a specific audit entry by index.
 *
 * @param policy Policy handle (borrowed).
 * @param idx    Zero-based index (must be < aegis_security_audit_count()).
 * @return Pointer to the entry (borrowed, immutable); NULL if idx out of range.
 */
const aegis_security_audit_entry_t* aegis_security_audit_get(const aegis_security_policy_t* policy,
                                                             size_t                         idx);

/**
 * @brief Get the most-recently-written audit entry.
 *
 * @param policy Policy handle (borrowed).
 * @return Pointer to the latest entry; NULL if log is empty.
 */
const aegis_security_audit_entry_t* aegis_security_audit_latest(
    const aegis_security_policy_t* policy);

/* ── Policy lifecycle ───────────────────────────────────────────────────────── */

/**
 * @brief Create an empty security policy with a fresh audit log.
 *
 * @param[out] out Receives the new policy handle. Ownership: transferred.
 * @return AEGIS_OK, AEGIS_ERR_INVALID (out NULL), AEGIS_ERR_NOMEM.
 */
aegis_status_t aegis_security_policy_create(aegis_security_policy_t** out);

/**
 * @brief Destroy a policy and release all associated resources.
 *
 * Safe to call with NULL (no-op).
 *
 * @param policy Handle to destroy (ownership: consumed).
 */
void aegis_security_policy_destroy(aegis_security_policy_t* policy);

/* ── Rule management ────────────────────────────────────────────────────────── */

/**
 * @brief Add a rule to the policy.
 *
 * A rule maps a tool name pattern to a required capability mask.
 * When evaluating a tool call, every capability bit set in the
 * tool's declared mask must also be set in at least one matching rule.
 *
 * @param policy         Policy (mutable, borrowed).
 * @param tool_pattern   Glob-like pattern for tool names ("*" matches all).
 * @param required_caps  Bitmask of capabilities this rule permits.
 * @return AEGIS_OK, AEGIS_ERR_INVALID (policy NULL, empty pattern),
 *         AEGIS_ERR_NOMEM.
 */
aegis_status_t aegis_security_policy_add_rule(aegis_security_policy_t* policy,
                                              const char*              tool_pattern,
                                              aegis_capability_t       required_caps);

/**
 * @brief Remove all rules from a policy.
 *
 * @param policy Policy (mutable, borrowed).
 */
void aegis_security_policy_clear_rules(aegis_security_policy_t* policy);

/**
 * @brief Number of rules currently in the policy.
 *
 * @param policy Policy (borrowed).
 * @return Rule count (0 if policy is NULL).
 */
size_t aegis_security_policy_rule_count(const aegis_security_policy_t* policy);

/* ── Evaluation (the core gate) ─────────────────────────────────────────────── */

/**
 * @brief Evaluate a tool invocation against the policy.
 *
 * This is the central gate function. It performs:
 *   1. Capability check — does the tool declare any capabilities?
 *   2. Rule matching — does any rule cover this tool's capabilities?
 *   3. Permission grant — records ALLOW/DENY in the audit log.
 *   4. Final decision — returns AEGIS_OK (allowed) or AEGIS_ERR_PERM (denied).
 *
 * Every call is logged to the audit log, even informational entries.
 * The audit log is append-only; old entries are discarded when full.
 *
 * @param policy         Policy (borrowed, mutable for audit write).
 * @param tool_name      Tool being invoked (borrowed, must be non-NULL).
 * @param tool_caps      Capabilities declared by the tool (from aegis_tool_def_t).
 * @param ctx            Optional caller-supplied context string (may be NULL).
 * @return AEGIS_OK (permitted), AEGIS_ERR_PERM (denied),
 *         AEGIS_ERR_INVALID (null args).
 */
aegis_status_t aegis_security_evaluate(aegis_security_policy_t* policy, const char* tool_name,
                                       aegis_capability_t tool_caps, const char* ctx);

/**
 * @brief Convenience: check whether a tool's capabilities are granted
 *        under the current policy.
 *
 * Equivalent to aegis_security_evaluate() but only reports the binary
 * allow/deny result. Always logs to the audit log.
 *
 * @param policy Policy (borrowed).
 * @param tool_name Tool name (borrowed, non-NULL).
 * @param tool_caps Capability bitmask.
 * @return true if permitted, false if denied.
 */
bool aegis_security_has_permission(const aegis_security_policy_t* policy, const char* tool_name,
                                   aegis_capability_t tool_caps);

/* ── Sandbox ───────────────────────────────────────────────────────────────── */

/**
 * @brief Create the default (mock/no-op) sandbox.
 *
 * The default sandbox imposes no restrictions. Swap in a real
 * implementation by calling aegis_security_sandbox_set_vtable().
 *
 * @param[out] out Receives the new sandbox handle. Ownership: transferred.
 * @return AEGIS_OK, AEGIS_ERR_INVALID, AEGIS_ERR_NOMEM.
 */
aegis_status_t aegis_security_sandbox_create(aegis_security_sandbox_t** out);

/**
 * @brief Destroy a sandbox.
 *
 * Safe to call with NULL (no-op).
 *
 * @param sandbox Handle to destroy (ownership: consumed).
 */
void aegis_security_sandbox_destroy(aegis_security_sandbox_t* sandbox);

/**
 * @brief Vtable for pluggable sandbox implementations.
 *
 * Each sandbox must implement these hooks. The execute hook is the
 * only required one; others may be NULL (defaults to no-op).
 */
typedef struct aegis_security_sandbox_vtable {
    /**
     * @brief Pre-execution hook: apply sandbox restrictions.
     *
     * Called BEFORE the tool's execute() function. If this returns
     * non-AEGIS_OK, the tool is NOT executed and the error propagates.
     *
     * @param sandbox Sandbox handle (borrowed, mutable).
     * @param tool_name Tool being executed (borrowed).
     * @param tool_caps Capabilities required (borrowed).
     * @param token   Cancellation token (borrowed).
     * @return AEGIS_OK to proceed, or an error to abort execution.
     */
    aegis_status_t (*pre_execute)(aegis_security_sandbox_t* sandbox, const char* tool_name,
                                  aegis_capability_t                tool_caps,
                                  const aegis_cancellation_token_t* token);

    /**
     * @brief Post-execution hook: clean up sandbox state.
     *
     * Called AFTER the tool's execute() function, regardless of
     * success or failure. Must not block indefinitely.
     *
     * @param sandbox Sandbox handle (borrowed, mutable).
     * @param result  Tool result (borrowed; may be NULL on failure).
     */
    void (*post_execute)(aegis_security_sandbox_t* sandbox, const void* result);
} aegis_security_sandbox_vtable_t;

/**
 * @brief Replace the sandbox's vtable with a custom implementation.
 *
 * The new vtable takes effect immediately for all subsequent calls.
 * The old vtable is NOT called back; the caller is responsible for
 * cleaning up any state the previous implementation may have held.
 *
 * @param sandbox Sandbox handle (mutable, borrowed).
 * @param vtable  New vtable (borrowed; must remain valid for sandbox lifetime).
 */
void aegis_security_sandbox_set_vtable(aegis_security_sandbox_t*              sandbox,
                                       const aegis_security_sandbox_vtable_t* vtable);

/**
 * @brief Get the current vtable pointer.
 *
 * @param sandbox Sandbox handle (borrowed).
 * @return Pointer to the current vtable, or NULL if none set.
 */
const aegis_security_sandbox_vtable_t* aegis_security_sandbox_get_vtable(
    const aegis_security_sandbox_t* sandbox);

/* ── Integration helpers ───────────────────────────────────────────────────── */

/**
 * @brief Full pipeline gate: evaluate + sandbox pre-check.
 *
 * Calls aegis_security_evaluate() followed by sandbox->pre_execute().
 * Both must succeed for the call to be allowed.
 *
 * @param policy       Policy (borrowed).
 * @param sandbox      Sandbox (borrowed, may be NULL for no sandbox).
 * @param tool_name    Tool name (borrowed, non-NULL).
 * @param tool_caps    Tool capabilities (from aegis_tool_def_t).
 * @param token        Cancellation token (borrowed).
 * @return AEGIS_OK to proceed, or the first error encountered.
 */
aegis_status_t aegis_security_gate(aegis_security_policy_t*  policy,
                                   aegis_security_sandbox_t* sandbox, const char* tool_name,
                                   aegis_capability_t                tool_caps,
                                   const aegis_cancellation_token_t* token);

#ifdef __cplusplus
}
#endif

#endif /* AEGIS_SECURITY_H */
