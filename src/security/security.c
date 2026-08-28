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

void aegis_security_policy_destroy(aegis_security_policy_t* policy)
{
    if (!policy) {
        return;
    }
    pthread_mutex_destroy(&policy->lock);
    free(policy);
}

/* ── Rule management ───────────────────────────────────────────────────────── */

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

void aegis_security_policy_clear_rules(aegis_security_policy_t* policy)
{
    if (!policy) {
        return;
    }
    policy->n_rules = 0;
}

size_t aegis_security_policy_rule_count(const aegis_security_policy_t* policy)
{
    return policy ? policy->n_rules : 0;
}

/* ── Audit access ──────────────────────────────────────────────────────────── */

size_t aegis_security_audit_count(const aegis_security_policy_t* policy)
{
    return policy ? policy->audit_count : 0;
}

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

bool aegis_security_has_permission(const aegis_security_policy_t* policy, const char* tool_name,
                                   aegis_capability_t tool_caps)
{
    return aegis_security_evaluate((aegis_security_policy_t*)policy, tool_name, tool_caps, NULL) ==
           AEGIS_OK;
}

/* ── Sandbox lifecycle ─────────────────────────────────────────────────────── */

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

void aegis_security_sandbox_destroy(aegis_security_sandbox_t* sandbox)
{
    free(sandbox);
}

void aegis_security_sandbox_set_vtable(aegis_security_sandbox_t*              sandbox,
                                       const aegis_security_sandbox_vtable_t* vtable)
{
    if (!sandbox) {
        return;
    }
    sandbox->vtable = vtable;
}

const aegis_security_sandbox_vtable_t* aegis_security_sandbox_get_vtable(
    const aegis_security_sandbox_t* sandbox)
{
    return sandbox ? sandbox->vtable : NULL;
}

/* ── Integration gate ──────────────────────────────────────────────────────── */

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
