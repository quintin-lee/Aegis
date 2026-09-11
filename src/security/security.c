/**
 * @file security.c
 * @brief Security module: policy, permission, capability, sandbox.
 *
 * Default-deny security enforcement. Every tool invocation is checked
 * against a configurable policy before execution proceeds. All decisions
 * are recorded in an append-only audit log.
 *
 * Execution pipeline (enforced, no bypass):
 *   Schema Validation -> Capability Check -> Policy Evaluation
 *   -> Permission Grant -> Sandbox Apply -> Execute
 */
#define _POSIX_C_SOURCE 200809L
#include "aegis/security/security.h"
#include "aegis/status.h"
#include "aegis/types.h"

#include "security_internal.h"

#include "lifecycle.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdarg.h>

/* ── Time ──────────────────────────────────────────────────────────────────── */

uint64_t aegis_security_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

/* ── Audit ─────────────────────────────────────────────────────────────────── */

/**
 * @brief Append an audit entry to the policy's circular log.
 *
 * Entries are stored in a fixed-size ring buffer; when full, older
 * entries are silently overwritten. Thread-safe under the policy mutex.
 *
 * @param[in]  policy   Policy to audit (must be non-NULL).
 * @param[in]  type     Audit event type.
 * @param[in]  decision AUDIT_ALLOW / AUDIT_DENY / AUDIT_INFO.
 * @param[in]  capability Capability bitmask relevant to this event.
 * @param[in]  rule_index Rule index that matched, or -1 for global events.
 * @param[in]  fmt      printf-style message template.
 * @param[in]  ...      Format arguments.
 */
void aegis_security_audit_append(aegis_security_policy_t* policy, aegis_security_audit_type_t type,
                                 aegis_security_audit_decision_t decision,
                                 aegis_capability_t capability, int rule_index, const char* fmt,
                                 ...)
{
    if (!policy) {
        return;
    }
    pthread_mutex_lock(&policy->lock);

    aegis_security_audit_entry_t* entry = &policy->audit[policy->audit_head];
    entry->timestamp                    = aegis_security_now_ns();
    entry->type                         = type;
    entry->decision                     = decision;
    entry->capability                   = capability;
    entry->rule_index                   = (uint32_t)rule_index;

    va_list ap;
    va_start(ap, fmt);
    vsnprintf(entry->message, sizeof(entry->message), fmt, ap);
    va_end(ap);

    size_t msg_len = strlen(entry->message);
    if (msg_len > 0 && entry->message[msg_len - 1] == '\n') {
        entry->message[msg_len - 1] = '\0';
    }

    policy->audit_head = (policy->audit_head + 1) % AEGIS_SECURITY_AUDIT_MAX_ENTRIES;
    if (policy->audit_count < AEGIS_SECURITY_AUDIT_MAX_ENTRIES) {
        policy->audit_count++;
    }
    pthread_mutex_unlock(&policy->lock);
}

/* ── Helpers ───────────────────────────────────────────────────────────────── */

static bool pattern_matches(const char* pattern, const char* name)
{
    if (!pattern || !name) {
        return false;
    }
    if (strcmp(pattern, "*") == 0) {
        return true;
    }
    size_t plen = strlen(pattern);
    size_t nlen = strlen(name);
    if (plen > nlen) {
        return false;
    }
    if (strncmp(pattern, name, plen) != 0) {
        return false;
    }
    if (plen > 0 && pattern[plen - 1] == '*') {
        return true;
    }
    return nlen == plen;
}

/* ── Capability string ─────────────────────────────────────────────────────── */

/**
 * @brief Map a capability bitmask to its human-readable name string.
 *
 * Returns a static buffer — not thread-safe for concurrent callers.
 * Unknown combinations are rendered as a comma-separated list of
 * known bits; unknown bits are silently ignored.
 *
 * @param[in] caps Capability flags.
 * @return Static string view; do NOT free.
 */
const char* aegis_security_capabilities_str(aegis_capability_t caps)
{
    static char buf[256];
    buf[0] = '\0';
    if (caps == AEGIS_CAP_NONE) {
        snprintf(buf, sizeof(buf), "NONE");
        return buf;
    }
    size_t off = 0;
#define APPEND_CAP(bit, name)                                               \
    do {                                                                    \
        if (caps & bit) {                                                   \
            if (off)                                                        \
                off += (size_t)snprintf(buf + off, sizeof(buf) - off, ","); \
            off += (size_t)snprintf(buf + off, sizeof(buf) - off, name);    \
        }                                                                   \
    } while (0)
    APPEND_CAP(AEGIS_CAP_READ_FILE, "READ_FILE");
    APPEND_CAP(AEGIS_CAP_WRITE_FILE, "WRITE_FILE");
    APPEND_CAP(AEGIS_CAP_SHELL, "SHELL");
    APPEND_CAP(AEGIS_CAP_NETWORK, "NETWORK");
    APPEND_CAP(AEGIS_CAP_RUN_PROCESS, "RUN_PROCESS");
    APPEND_CAP(AEGIS_CAP_ACCESS_CRED, "ACCESS_CRED");
#undef APPEND_CAP
    return buf;
}

/* ── Policy lifecycle ──────────────────────────────────────────────────────── */

/**
 * @brief Create a new security policy with a recursive mutex.
 *
 * The policy starts empty (no rules) and must be configured with
 * aegis_security_policy_add_rule() before use. The caller owns the
 * policy and must call aegis_security_policy_destroy.
 *
 * @param[out] out Receives the new policy; untouched on failure.
 * @return AEGIS_OK on success, AEGIS_ERR_INVALID for NULL @p out,
 *   AEGIS_ERR_NOMEM on allocation failure.
 */
aegis_status_t aegis_security_policy_create(aegis_security_policy_t** out)
{
    AEGIS_CHECK_OUT(out);
    aegis_security_policy_t* p = calloc(1, sizeof(*p));
    if (!p) {
        return AEGIS_ERR_NOMEM;
    }
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&p->lock, &attr);
    pthread_mutexattr_destroy(&attr);
    *out = p;
    return AEGIS_OK;
}

/**
 * @brief Destroy a security policy, releasing the mutex.
 *
 * Does NOT free any rules — the policy stores rules inline. The caller
 * is responsible for zeroing the policy struct afterwards if it was
 * stack-allocated. NULL is a no-op.
 *
 * @param[in] policy Policy to destroy, or NULL.
 */
void aegis_security_policy_destroy(aegis_security_policy_t* policy)
{
    if (!policy) {
        return;
    }
    pthread_mutex_destroy(&policy->lock);
    free(policy);
}

/* ── Rule management ───────────────────────────────────────────────────────── */

/**
 * @brief Add a rule that maps a tool-name pattern to required capabilities.
 *
 * Patterns are prefix-matching: "read_*" matches "read_file" but not
 * "write_file". "*" matches everything. Rules are evaluated in order;
 * the first match wins. Default-deny: tools without a matching rule
 * are always denied. Returns AEGIS_ERR_BUSY when the rule table is
 * full. Thread-safe.
 *
 * @param[in] policy            Policy to extend.
 * @param[in] tool_pattern      Tool-name glob pattern.
 * @param[in] required_caps     Bitmask of capabilities the tool must declare.
 * @return AEGIS_OK on success, AEGIS_ERR_INVALID for NULL/pattern,
 *   AEGIS_ERR_BUSY when the rule table is full.
 */
aegis_status_t aegis_security_policy_add_rule(aegis_security_policy_t* policy,
                                              const char*              tool_pattern,
                                              aegis_capability_t       required_caps)
{
    if (!policy || !tool_pattern || tool_pattern[0] == '\0') {
        return AEGIS_ERR_INVALID;
    }
    if (policy->n_rules >= AEGIS_SECURITY_MAX_RULES) {
        return AEGIS_ERR_BUSY;
    }

    aegis_security_rule_t* r = &policy->rules[policy->n_rules];
    strncpy(r->pattern, tool_pattern, sizeof(r->pattern) - 1);
    r->pattern[sizeof(r->pattern) - 1] = '\0';
    r->required_caps                   = required_caps;
    policy->n_rules++;

    aegis_security_audit_append(policy, AEGIS_SECURITY_AUDIT_POLICY_EVAL, AEGIS_SECURITY_AUDIT_INFO,
                                required_caps, (int)(policy->n_rules - 1),
                                "Added rule: pattern=%s caps=%s", r->pattern,
                                aegis_security_capabilities_str(required_caps));
    return AEGIS_OK;
}

/**
 * @brief Clear all rules from a policy.
 *
 * The policy remains valid; after clearing, all tools are denied by
 * default. Thread-safe.
 *
 * @param[in] policy Policy to clear, or NULL.
 */
void aegis_security_policy_clear_rules(aegis_security_policy_t* policy)
{
    if (!policy) {
        return;
    }
    policy->n_rules = 0;
}

/**
 * @brief Return the number of rules currently registered.
 *
 * @param[in] policy Policy, or NULL.
 * @return Rule count, or 0 for NULL.
 */
size_t aegis_security_policy_rule_count(const aegis_security_policy_t* policy)
{
    return policy ? policy->n_rules : 0;
}

/* ── Audit access ──────────────────────────────────────────────────────────── */

/**
 * @brief Return the number of audit entries currently stored.
 *
 * @param[in] policy Policy, or NULL.
 * @return Entry count, or 0 for NULL.
 */
size_t aegis_security_audit_count(const aegis_security_policy_t* policy)
{
    return policy ? policy->audit_count : 0;
}

/**
 * @brief Read a specific audit entry by circular-index.
 *
 * The audit buffer is a fixed-size ring; @p idx is interpreted as the
 * logical index (0 = oldest, audit_count-1 = newest). Returns NULL when
 * the policy is empty or @p idx is out of range.
 *
 * @param[in]  policy Security policy containing the audit log.
 * @param[in]  idx    Logical index into the audit entries.
 * @return Pointer to the audit entry, or NULL when absent.
 */
const aegis_security_audit_entry_t* aegis_security_audit_get(const aegis_security_policy_t* policy,
                                                             size_t                         idx)
{
    if (!policy || idx >= policy->audit_count) {
        return NULL;
    }
    size_t count = policy->audit_count;
    if (count == 0) {
        return NULL;
    }
    size_t pos = (policy->audit_head - count + idx + count) % count;
    return &policy->audit[pos];
}

/**
 * @brief Return the most-recent audit entry.
 *
 * Equivalent to audit_get(policy, audit_count-1). Returns NULL when the
 * audit log is empty.
 *
 * @param[in] policy Security policy with an audit log.
 * @return Latest audit entry, or NULL when empty.
 */
const aegis_security_audit_entry_t* aegis_security_audit_latest(
    const aegis_security_policy_t* policy)
{
    if (!policy || policy->audit_count == 0) {
        return NULL;
    }
    size_t last = (policy->audit_head - 1) % AEGIS_SECURITY_AUDIT_MAX_ENTRIES;
    return &policy->audit[last];
}

/* ── Evaluation ─────────────────────────────────────────────────────────────── */

/**
 * @brief Evaluate whether a tool invocation is permitted under the policy.
 *
 * Iterates rules in order; the first rule whose pattern matches the
 * tool name and whose required caps are a superset of the tool's
 * declared caps grants permission. Log the decision into the audit log.
 * Thread-safe (acquires the policy mutex for the duration).
 *
 * @param[in]  policy   Security policy (must be non-NULL).
 * @param[in]  tool_name Tool identifier to evaluate.
 * @param[in]  tool_caps Bitmask of capabilities the tool declares.
 * @param[in]  ctx      Optional context string for audit logging (may be NULL).
 * @return AEGIS_OK when permitted, AEGIS_ERR_PERM when denied,
 *   AEGIS_ERR_INVALID for NULL args.
 */
aegis_status_t aegis_security_evaluate(aegis_security_policy_t* policy, const char* tool_name,
                                       aegis_capability_t tool_caps, const char* ctx)
{
    if (!policy || !tool_name) {
        return AEGIS_ERR_INVALID;
    }
    pthread_mutex_lock(&policy->lock);

    /* Phase 1: Capability check. */
    aegis_security_audit_append(policy, AEGIS_SECURITY_AUDIT_CAP_CHECK, AEGIS_SECURITY_AUDIT_INFO,
                                tool_caps, -1, "Tool '%s' declares capabilities=%s", tool_name,
                                aegis_security_capabilities_str(tool_caps));
    if (ctx) {
        aegis_security_audit_append(policy, AEGIS_SECURITY_AUDIT_CAP_CHECK,
                                    AEGIS_SECURITY_AUDIT_INFO, tool_caps, -1, "Context: %s", ctx);
    }

    /* Phase 2: Rule matching — default deny. */
    bool granted      = false;
    int  matched_rule = -1;
    for (size_t i = 0; i < policy->n_rules; i++) {
        if (!pattern_matches(policy->rules[i].pattern, tool_name)) {
            continue;
        }
        if ((policy->rules[i].required_caps & tool_caps) == tool_caps) {
            granted      = true;
            matched_rule = (int)i;
            break;
        }
    }

    /* Phase 3: Log decision. */
    if (granted) {
        aegis_security_audit_append(policy, AEGIS_SECURITY_AUDIT_PERMISSION,
                                    AEGIS_SECURITY_AUDIT_ALLOW, tool_caps, matched_rule,
                                    "Permission GRANTED for tool '%s' (rule %d: %s)", tool_name,
                                    matched_rule, policy->rules[matched_rule].pattern);
        aegis_security_audit_append(policy, AEGIS_SECURITY_AUDIT_DECISION,
                                    AEGIS_SECURITY_AUDIT_ALLOW, tool_caps, matched_rule,
                                    "Tool '%s' ALLOWED — all checks passed", tool_name);
        pthread_mutex_unlock(&policy->lock);
        return AEGIS_OK;
    } else {
        aegis_security_audit_append(policy, AEGIS_SECURITY_AUDIT_PERMISSION,
                                    AEGIS_SECURITY_AUDIT_DENY, tool_caps, -1,
                                    "Permission DENIED for tool '%s': no matching rule grants %s",
                                    tool_name, aegis_security_capabilities_str(tool_caps));
        aegis_security_audit_append(policy, AEGIS_SECURITY_AUDIT_DECISION,
                                    AEGIS_SECURITY_AUDIT_DENY, tool_caps, -1,
                                    "Tool '%s' DENIED — capability check failed", tool_name);
        pthread_mutex_unlock(&policy->lock);
        return AEGIS_ERR_PERM;
    }
}

/**
 * @brief Predicate wrapper around evaluate(): returns true when the
 *        tool is permitted, false otherwise.
 *
 * No audit logging of the final verdict is emitted (evaluate() still
 * logs internal events). Intended for fast-path checks where the
 * caller does not need a full aegis_status_t return.
 *
 * @param[in]  policy    Security policy.
 * @param[in]  tool_name Tool identifier.
 * @param[in]  tool_caps Capability bitmask.
 * @return true when permitted, false when denied or on NULL args.
 */
bool aegis_security_has_permission(const aegis_security_policy_t* policy, const char* tool_name,
                                   aegis_capability_t tool_caps)
{
    return aegis_security_evaluate((aegis_security_policy_t*)policy, tool_name, tool_caps, NULL) ==
           AEGIS_OK;
}

/* ── Sandbox lifecycle ─────────────────────────────────────────────────────── */

/**
 * @brief Create a new (empty) sandbox.
 *
 * A sandbox has no behavior until a vtable is installed via
 * aegis_security_sandbox_set_vtable(). The caller owns the sandbox
 * and must call aegis_security_sandbox_destroy.
 *
 * @param[out] out Receives the new sandbox; untouched on failure.
 * @return AEGIS_OK on success, AEGIS_ERR_INVALID for NULL @p out,
 *   AEGIS_ERR_NOMEM on allocation failure.
 */
/**
 * @brief Allocate an empty security sandbox and wire the vtable via
 *        aegis_security_sandbox_set_vtable().
 *
 * A sandbox is a per-execution isolation boundary; it must be provided
 * with a vtable before use. The caller owns the result and must destroy
 * it with aegis_security_sandbox_destroy.
 *
 * @param[out] out Receives the new sandbox; untouched on failure.
 * @return AEGIS_OK on success, AEGIS_ERR_INVALID for NULL @p out,
 *   AEGIS_ERR_NOMEM on allocation failure.
 */
aegis_status_t aegis_security_sandbox_create(aegis_security_sandbox_t** out)
{
    AEGIS_CHECK_OUT(out);
    aegis_security_sandbox_t* s = calloc(1, sizeof(*s));
    if (!s) {
        return AEGIS_ERR_NOMEM;
    }
    *out = s;
    return AEGIS_OK;
}

/**
 * @brief Destroy a sandbox, freeing any heap-backed resources.
 *
 * NULL is a no-op. The vtable itself is NOT freed — it is owned by
 * the sandbox implementor.
 *
 * @param[in] sandbox Sandbox to destroy, or NULL.
 */
/**
 * @brief Destroy a security sandbox.
 *
 * The caller must have previously unregistered any vtable hooks via
 * aegis_security_sandbox_set_vtable(NULL) before calling destroy. NULL
 * is a no-op.
 *
 * @param[in] sandbox Sandbox to destroy, or NULL.
 */
void aegis_security_sandbox_destroy(aegis_security_sandbox_t* sandbox)
{
    free(sandbox);
}

/**
 * @brief Attach a vtable to a sandbox.
 *
 * The vtable defines pre_execute/post_execute hooks that the security
 * gate calls around tool execution. The caller retains ownership of
 * the vtable struct. NULL sandbox is a no-op.
 *
 * @param[in] sandbox Sandbox to configure.
 * @param[in] vtable  Vtable pointer (may be NULL to clear).
 */
/**
 * @brief Install or clear the sandbox vtable on a sandbox instance.
 *
 * Vtable methods are invoked by aegis_security_gate() before each tool
 * call. A NULL vtable disables all sandbox checks (still performs
 * policy evaluation). Thread-safe: only the thread that owns the
 * sandbox should call this.
 *
 * @param[in] sandbox Sandbox to configure (must be non-NULL).
 * @param[in] vtable  Vtable struct to bind, or NULL to clear.
 */
void aegis_security_sandbox_set_vtable(aegis_security_sandbox_t*              sandbox,
                                       const aegis_security_sandbox_vtable_t* vtable)
{
    if (!sandbox) {
        return;
    }
    sandbox->vtable = vtable;
}

/**
 * @brief Get the vtable attached to a sandbox.
 *
 * @param[in] sandbox Sandbox to query, or NULL.
 * @return Vtable pointer, or NULL when no vtable is installed.
 */
/**
 * @brief Return the sandbox vtable, or NULL when none is installed.
 *
 * Thread-safe.
 *
 * @param[in] sandbox Sandbox instance, or NULL.
 * @return Pointer to the vtable struct, or NULL when absent or @p sandbox
 *         is NULL.
 */
const aegis_security_sandbox_vtable_t* aegis_security_sandbox_get_vtable(
    const aegis_security_sandbox_t* sandbox)
{
    return sandbox ? sandbox->vtable : NULL;
}

/* ── Integration gate ──────────────────────────────────────────────────────── */

/**
 * @brief Full security gate: evaluate policy then run sandbox pre_execute.
 *
 * First checks whether the tool is permitted under the policy; if
 * permitted and a sandbox with a pre_execute hook is attached, that
 * hook is called before the tool is allowed to run. Audit entries are
 * written for both phases. Thread-safe.
 *
 * @param[in]  policy    Security policy (must be non-NULL).
 * @param[in]  sandbox   Optional sandbox to apply (may be NULL).
 * @param[in]  tool_name Tool identifier.
 * @param[in]  tool_caps Capability bitmask the tool declares.
 * @param[in]  token     Cancellation point, or NULL to ignore.
 * @return AEGIS_OK when fully permitted, AEGIS_ERR_PERM when denied,
 *   AEGIS_ERR_CANCELLED when @p token is tripped, AEGIS_ERR_INVALID for
 *   NULL args, else the sandbox error.
 */
/**
 * @brief Gate a tool invocation through policy evaluation and the
 *        sandbox pre-execute hook.
 *
 * Runs the policy rule engine first; if the tool is denied there the
 * sandbox is never consulted. On policy allow, the sandbox's
 * pre_execute hook (when installed) runs next — it may veto even an
 * allowed tool. Cancellation is checked before both phases. Returns
 * AEGIS_ERR_PERM when policy denies, AEGIS_ERR_CANCELLED when the
 * token is tripped, and AEGIS_OK on full pass-through.
 *
 * @param[in]  policy      Security policy (must be non-NULL).
 * @param[in]  sandbox     Per-execution sandbox (may be NULL — policy-only path).
 * @param[in]  tool_name   Tool identifier (must be non-NULL).
 * @param[in]  tool_caps   Capabilities declared by the tool.
 * @param[in]  token       Cancellation point, or NULL to ignore.
 * @return AEGIS_OK when fully permitted, AEGIS_ERR_PERM on policy denial,
 *   AEGIS_ERR_CANCELLED when @p token is tripped, else the sandbox error.
 */
aegis_status_t aegis_security_gate(aegis_security_policy_t*  policy,
                                   aegis_security_sandbox_t* sandbox, const char* tool_name,
                                   aegis_capability_t                tool_caps,
                                   const aegis_cancellation_token_t* token)
{
    aegis_status_t rc = aegis_security_evaluate(policy, tool_name, tool_caps, NULL);
    if (rc != AEGIS_OK) {
        return rc;
    }

    if (sandbox && sandbox->vtable && sandbox->vtable->pre_execute) {
        rc = sandbox->vtable->pre_execute(sandbox, tool_name, tool_caps, token);
        if (rc != AEGIS_OK) {
            aegis_security_audit_append(policy, AEGIS_SECURITY_AUDIT_SANDBOX,
                                        AEGIS_SECURITY_AUDIT_DENY, tool_caps, -1,
                                        "Sandbox pre_execute rejected: %s", aegis_status_str(rc));
        }
    }
    return rc;
}
