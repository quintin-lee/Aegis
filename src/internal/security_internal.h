/**
 * @file security_internal.h
 * @warning This header must NOT be included from public code.
 */
#ifndef AEGIS_INTERNAL_SECURITY_H
#define AEGIS_INTERNAL_SECURITY_H

#include "aegis/security/security.h"
#include "aegis/executor/cancellation.h"


#include <stdint.h>
#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>

/* ── Rule ──────────────────────────────────────────────────────────────────── */

#define AEGIS_SECURITY_MAX_RULES 256
#define AEGIS_SECURITY_PATTERN_MAX 128

typedef struct aegis_security_rule {
    char       pattern[AEGIS_SECURITY_PATTERN_MAX];
    aegis_capability_t required_caps;
} aegis_security_rule_t;

/* ── Policy ─────────────────────────────────────────────────────────────────── */

struct aegis_security_policy {
    aegis_security_rule_t       rules[AEGIS_SECURITY_MAX_RULES];
    size_t                      n_rules;
    aegis_security_audit_entry_t audit[AEGIS_SECURITY_AUDIT_MAX_ENTRIES];
    size_t                      audit_head;    /**< Next write position.     */
    size_t                      audit_count;   /**< Total entries written.   */
    aegis_security_sandbox_t*   sandbox;       /**< Attached sandbox (may be NULL). */
    pthread_mutex_t             lock;          /**< Guards audit + rules.    */
};

/* ── Sandbox ────────────────────────────────────────────────────────────────── */

struct aegis_security_sandbox {
    const aegis_security_sandbox_vtable_t* vtable;
    void*                                  user_data;
};

/* ── Audit helpers ─────────────────────────────────────────────────────────── */

/** Append an entry to the policy's audit log (ring buffer). */
void aegis_security_audit_append(aegis_security_policy_t* policy,
                                  aegis_security_audit_type_t type,
                                  aegis_security_audit_decision_t decision,
                                  aegis_capability_t capability,
                                  int rule_index,
                                  const char* fmt, ...);

/** Get current monotonic time in nanoseconds. */
uint64_t aegis_security_now_ns(void);

#endif /* AEGIS_INTERNAL_SECURITY_H */
