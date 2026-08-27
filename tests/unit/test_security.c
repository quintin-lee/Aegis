/**
 * @file test_security.c
 * @brief Unit tests for the Security module.
 */
#include "aegis/security.h"
#include "aegis/cancellation.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void expect_ok(aegis_status_t rc, const char* msg)
{
    assert(rc == AEGIS_OK);
    (void)msg;
}

/* ── Capability string ─────────────────────────────────────────────────────── */

static void test_capabilities_str(void)
{
    assert(strcmp(aegis_security_capabilities_str(AEGIS_CAP_NONE), "NONE") == 0);
    const char* s = aegis_security_capabilities_str(AEGIS_CAP_SHELL);
    assert(strstr(s, "SHELL") != NULL);
    const char* multi = aegis_security_capabilities_str(
        AEGIS_CAP_SHELL | AEGIS_CAP_NETWORK);
    assert(strstr(multi, "SHELL") != NULL);
    assert(strstr(multi, "NETWORK") != NULL);
}

/* ── Policy lifecycle ──────────────────────────────────────────────────────── */

static void test_policy_create_destroy(void)
{
    aegis_security_policy_t* p = NULL;
    expect_ok(aegis_security_policy_create(&p), "create");
    assert(p != NULL);
    assert(aegis_security_policy_rule_count(p) == 0);
    assert(aegis_security_audit_count(p) == 0);
    aegis_security_policy_destroy(p);
    aegis_security_policy_destroy(NULL);
}

static void test_policy_create_null_out(void)
{
    assert(aegis_security_policy_create(NULL) == AEGIS_ERR_INVALID);
}

/* ── Rule management ───────────────────────────────────────────────────────── */

static void test_add_rule(void)
{
    aegis_security_policy_t* p = NULL;
    expect_ok(aegis_security_policy_create(&p), "create");

    expect_ok(aegis_security_policy_add_rule(p, "read_file", AEGIS_CAP_READ_FILE), "add rule");
    assert(aegis_security_policy_rule_count(p) == 1);

    expect_ok(aegis_security_policy_add_rule(p, "*", AEGIS_CAP_SHELL), "add wildcard");
    assert(aegis_security_policy_rule_count(p) == 2);

    aegis_security_policy_clear_rules(p);
    assert(aegis_security_policy_rule_count(p) == 0);

    aegis_security_policy_destroy(p);
}

static void test_add_rule_invalid(void)
{
    aegis_security_policy_t* p = NULL;
    expect_ok(aegis_security_policy_create(&p), "create");

    assert(aegis_security_policy_add_rule(NULL, "x", AEGIS_CAP_NONE) == AEGIS_ERR_INVALID);
    assert(aegis_security_policy_add_rule(p, "", AEGIS_CAP_NONE) == AEGIS_ERR_INVALID);
    assert(aegis_security_policy_add_rule(p, NULL, AEGIS_CAP_NONE) == AEGIS_ERR_INVALID);

    aegis_security_policy_destroy(p);
}

/* ── Evaluation: allowed ───────────────────────────────────────────────────── */

static void test_evaluate_allowed(void)
{
    aegis_security_policy_t* p = NULL;
    expect_ok(aegis_security_policy_create(&p), "create");
    aegis_security_policy_add_rule(p, "read_file", AEGIS_CAP_READ_FILE);
    aegis_security_policy_add_rule(p, "execute_cmd", AEGIS_CAP_SHELL);

    assert(aegis_security_evaluate(p, "read_file", AEGIS_CAP_READ_FILE, "context1") == AEGIS_OK);
    assert(aegis_security_evaluate(p, "execute_cmd", AEGIS_CAP_SHELL, NULL) == AEGIS_OK);
    /* Default deny: tool requiring MORE than rule grants is denied. */
    assert(aegis_security_evaluate(p, "read_file",
                                    AEGIS_CAP_READ_FILE | AEGIS_CAP_WRITE_FILE,
                                    NULL) == AEGIS_ERR_PERM);

    aegis_security_policy_destroy(p);
}

static void test_evaluate_default_deny(void)
{
    aegis_security_policy_t* p = NULL;
    expect_ok(aegis_security_policy_create(&p), "create");
    assert(aegis_security_evaluate(p, "anything", AEGIS_CAP_SHELL, NULL) == AEGIS_ERR_PERM);
    assert(aegis_security_evaluate(p, "read_file", AEGIS_CAP_READ_FILE, NULL) == AEGIS_ERR_PERM);

    aegis_security_policy_destroy(p);
}

static void test_evaluate_partial_match(void)
{
    aegis_security_policy_t* p = NULL;
    expect_ok(aegis_security_policy_create(&p), "create");
    aegis_security_policy_add_rule(p, "read_file", AEGIS_CAP_READ_FILE);

    assert(aegis_security_evaluate(p, "read_file", AEGIS_CAP_READ_FILE, NULL) == AEGIS_OK);
    assert(aegis_security_evaluate(p, "read_file",
                                    AEGIS_CAP_READ_FILE | AEGIS_CAP_NETWORK,
                                    NULL) == AEGIS_ERR_PERM);

    aegis_security_policy_destroy(p);
}

static void test_evaluate_wildcard(void)
{
    aegis_security_policy_t* p = NULL;
    expect_ok(aegis_security_policy_create(&p), "create");
    aegis_security_policy_add_rule(p, "*", AEGIS_CAP_READ_FILE);

    assert(aegis_security_evaluate(p, "foo", AEGIS_CAP_READ_FILE, NULL) == AEGIS_OK);
    assert(aegis_security_evaluate(p, "bar", AEGIS_CAP_READ_FILE, NULL) == AEGIS_OK);
    assert(aegis_security_evaluate(p, "baz", AEGIS_CAP_SHELL, NULL) == AEGIS_ERR_PERM);

    aegis_security_policy_destroy(p);
}

static void test_evaluate_invalid_args(void)
{
    aegis_security_policy_t* p = NULL;
    expect_ok(aegis_security_policy_create(&p), "create");

    assert(aegis_security_evaluate(NULL, "x", AEGIS_CAP_NONE, NULL) == AEGIS_ERR_INVALID);
    assert(aegis_security_evaluate(p, NULL, AEGIS_CAP_NONE, NULL) == AEGIS_ERR_INVALID);

    aegis_security_policy_destroy(p);
}

/* ── has_permission ────────────────────────────────────────────────────────── */

static void test_has_permission(void)
{
    aegis_security_policy_t* p = NULL;
    expect_ok(aegis_security_policy_create(&p), "create");
    aegis_security_policy_add_rule(p, "read_file", AEGIS_CAP_READ_FILE);

    assert(aegis_security_has_permission(p, "read_file", AEGIS_CAP_READ_FILE) == true);
    assert(aegis_security_has_permission(p, "read_file", AEGIS_CAP_SHELL) == false);
    assert(aegis_security_has_permission(p, "unknown", AEGIS_CAP_READ_FILE) == false);

    aegis_security_policy_destroy(p);
}

static void test_has_permission_null(void)
{
    assert(aegis_security_has_permission(NULL, "x", AEGIS_CAP_NONE) == false);
}

/* ── Audit log ─────────────────────────────────────────────────────────────── */

static void test_audit_log_records_decisions(void)
{
    aegis_security_policy_t* p = NULL;
    expect_ok(aegis_security_policy_create(&p), "create");
    aegis_security_policy_add_rule(p, "read_file", AEGIS_CAP_READ_FILE);

    aegis_security_evaluate(p, "read_file", AEGIS_CAP_READ_FILE, "test_ctx");
    aegis_security_evaluate(p, "denied_tool", AEGIS_CAP_SHELL, NULL);

    size_t count = aegis_security_audit_count(p);
    assert(count >= 2);

    const aegis_security_audit_entry_t* latest = aegis_security_audit_latest(p);
    assert(latest != NULL);
    assert(latest->type == AEGIS_SECURITY_AUDIT_DECISION);

    /* Verify audit log has entries of expected types. */
    bool found_cap_check = false, found_decision = false, found_deny = false;
    for (size_t i = 0; i < count; i++) {
        const aegis_security_audit_entry_t* e = aegis_security_audit_get(p, i);
        assert(e != NULL);
        if (e->type == AEGIS_SECURITY_AUDIT_CAP_CHECK) found_cap_check = true;
        if (e->type == AEGIS_SECURITY_AUDIT_DECISION) {
            found_decision = true;
            if (e->decision == AEGIS_SECURITY_AUDIT_DENY) found_deny = true;
        }
    }
    assert(found_cap_check);
    assert(found_decision);
    assert(found_deny);

    bool found_deny = false;
    for (size_t i = 0; i < count; i++) {
        const aegis_security_audit_entry_t* e = aegis_security_audit_get(p, i);
        if (e && e->decision == AEGIS_SECURITY_AUDIT_DENY) {
            found_deny = true;

        }
    }
    assert(found_deny);

    aegis_security_policy_destroy(p);
}

static void test_audit_ring_buffer(void)
{
    aegis_security_policy_t* p = NULL;
    expect_ok(aegis_security_policy_create(&p), "create");

    for (size_t i = 0; i < AEGIS_SECURITY_AUDIT_MAX_ENTRIES + 10; i++) {
        aegis_security_evaluate(p, "read_file", AEGIS_CAP_READ_FILE, NULL);
    }
    assert(aegis_security_audit_count(p) == AEGIS_SECURITY_AUDIT_MAX_ENTRIES);

    aegis_security_policy_destroy(p);
}

/* ── Sandbox ───────────────────────────────────────────────────────────────── */

static aegis_status_t mock_pre_execute(aegis_security_sandbox_t* sandbox,
                                        const char* tool_name,
                                        aegis_capability_t tool_caps,
                                        const aegis_cancellation_token_t* token)
{
    (void)sandbox;
    (void)tool_name;
    (void)token;
    if (tool_caps & AEGIS_CAP_SHELL) return AEGIS_ERR_PERM;
    return AEGIS_OK;
}

static void mock_post_execute(aegis_security_sandbox_t* sandbox, const void* result)
{
    (void)sandbox;
    (void)result;
}

static const aegis_security_sandbox_vtable_t mock_vtable = {
    .pre_execute  = mock_pre_execute,
    .post_execute = mock_post_execute,
};

static void test_sandbox_lifecycle(void)
{
    aegis_security_sandbox_t* s = NULL;
    expect_ok(aegis_security_sandbox_create(&s), "create");
    assert(s != NULL);
    assert(aegis_security_sandbox_get_vtable(s) == NULL);

    aegis_security_sandbox_set_vtable(s, &mock_vtable);
    assert(aegis_security_sandbox_get_vtable(s) == &mock_vtable);

    aegis_security_sandbox_destroy(s);
    aegis_security_sandbox_destroy(NULL);
}

static void test_sandbox_pre_execute_deny(void)
{
    aegis_security_policy_t* p = NULL;
    aegis_security_sandbox_t* s = NULL;
    expect_ok(aegis_security_policy_create(&p), "create");
    expect_ok(aegis_security_sandbox_create(&s), "sandbox");
    aegis_security_policy_add_rule(p, "read_file", AEGIS_CAP_READ_FILE);
    aegis_security_policy_add_rule(p, "exec_cmd", AEGIS_CAP_SHELL);
    aegis_security_sandbox_set_vtable(s, &mock_vtable);

    aegis_cancellation_token_t* token = NULL;
    expect_ok(aegis_cancellation_token_create(&token), "token");
    assert(aegis_security_gate(p, s, "read_file", AEGIS_CAP_READ_FILE, token) == AEGIS_OK);
    assert(aegis_security_gate(p, s, "exec_cmd", AEGIS_CAP_SHELL, token) == AEGIS_ERR_PERM);

    aegis_cancellation_token_destroy(token);
    aegis_security_sandbox_destroy(s);
    aegis_security_policy_destroy(p);
}

static void test_gate_no_sandbox(void)
{
    aegis_security_policy_t* p = NULL;
    expect_ok(aegis_security_policy_create(&p), "create");
    aegis_security_policy_add_rule(p, "*", AEGIS_CAP_READ_FILE);

    aegis_cancellation_token_t* token = NULL;
    expect_ok(aegis_cancellation_token_create(&token), "token");
    /* NULL sandbox should be a no-op. */
    assert(aegis_security_gate(p, NULL, "read_file", AEGIS_CAP_READ_FILE, token) == AEGIS_OK);

    aegis_cancellation_token_destroy(token);
    aegis_security_policy_destroy(p);
}

/* ── Null safety ───────────────────────────────────────────────────────────── */

static void test_null_safety(void)
{
    assert(aegis_security_audit_count(NULL) == 0);
    assert(aegis_security_audit_get(NULL, 0) == NULL);
    assert(aegis_security_audit_latest(NULL) == NULL);
    assert(aegis_security_policy_rule_count(NULL) == 0);
    assert(aegis_security_sandbox_get_vtable(NULL) == NULL);
    aegis_security_sandbox_set_vtable(NULL, &mock_vtable);
    aegis_security_policy_clear_rules(NULL);
    const char* caps = aegis_security_capabilities_str(AEGIS_CAP_NONE);
    assert(caps != NULL);
    (void)caps;
}

/* ── Main ──────────────────────────────────────────────────────────────────── */

int main(void)
{
    test_null_safety();
    test_capabilities_str();
    test_policy_create_destroy();
    test_policy_create_null_out();
    test_add_rule();
    test_add_rule_invalid();
    test_evaluate_allowed();
    test_evaluate_default_deny();
    test_evaluate_partial_match();
    test_evaluate_wildcard();
    test_evaluate_invalid_args();
    test_has_permission();
    test_has_permission_null();
    test_audit_log_records_decisions();
    test_audit_ring_buffer();
    test_sandbox_lifecycle();
    test_sandbox_pre_execute_deny();
    test_gate_no_sandbox();

    printf("security: all tests passed\n");
    return 0;
}
