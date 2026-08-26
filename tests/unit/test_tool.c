/**
 * @file test_tool.c
 * @brief Unit tests for the Tool ABI: typed values, argument lists,
 *        schema validation, results, and the registry.
 */
#include "aegis/tool.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Argument lists ─────────────────────────────────────────────────── */

static void test_args_lifecycle(void)
{
    aegis_tool_args_t* args = NULL;

    assert(aegis_tool_args_create(NULL) == AEGIS_ERR_INVALID);
    assert(aegis_tool_args_create(&args) == AEGIS_OK);
    assert(aegis_tool_args_count(args) == 0);
    assert(aegis_tool_args_count(NULL) == 0);

    const aegis_tool_value_t* v = NULL;
    assert(!aegis_tool_args_find(args, "missing", &v));
    assert(!aegis_tool_args_find(args, NULL, &v));
    assert(!aegis_tool_args_find(NULL, "x", &v));

    /* All five value types. */
    char text[] = "editable-source";
    uint8_t blob[] = {1, 2, 3};
    assert(aegis_tool_args_add_bool(args, "b", true) == AEGIS_OK);
    assert(aegis_tool_args_add_int(args, "i", -42) == AEGIS_OK);
    assert(aegis_tool_args_add_float(args, "f", 2.5) == AEGIS_OK);
    assert(aegis_tool_args_add_string(args, "s", text) == AEGIS_OK);
    assert(aegis_tool_args_add_bytes(args, "y", blob, sizeof(blob)) == AEGIS_OK);
    assert(aegis_tool_args_count(args) == 5);

    assert(aegis_tool_args_find(args, "b", &v) && v->type == AEGIS_TOOL_VAL_BOOL && v->as.b == true);
    assert(aegis_tool_args_find(args, "i", &v) && v->type == AEGIS_TOOL_VAL_INT && v->as.i == -42);
    assert(aegis_tool_args_find(args, "f", &v) && v->type == AEGIS_TOOL_VAL_FLOAT && v->as.f == 2.5);
    assert(aegis_tool_args_find(args, "s", &v) && v->type == AEGIS_TOOL_VAL_STRING &&
           v->as.str.len == strlen("editable-source") &&
           strcmp(v->as.str.ptr, "editable-source") == 0);
    assert(aegis_tool_args_find(args, "y", &v) && v->type == AEGIS_TOOL_VAL_BYTES &&
           v->as.bytes.len == 3 && ((const uint8_t*)v->as.bytes.ptr)[2] == 3);

    /* Deep copy: mutating the source must not affect stored payload. */
    text[0] = 'X';
    assert(aegis_tool_args_find(args, "s", &v));
    assert(v->as.str.ptr[0] == 'e');
    blob[0] = 0xFF;
    assert(aegis_tool_args_find(args, "y", &v));
    assert(((const uint8_t*)v->as.bytes.ptr)[0] == 1);

    /* Duplicate name rejected. */
    assert(aegis_tool_args_add_int(args, "b", 7) == AEGIS_ERR_BUSY);
    assert(aegis_tool_args_count(args) == 5);

    /* NULL-name / NULL-arg rejections. */
    assert(aegis_tool_args_add_bool(args, NULL, true) == AEGIS_ERR_INVALID);
    assert(aegis_tool_args_add_bool(NULL, "x", true) == AEGIS_ERR_INVALID);

    /* NULL string payload stores the empty string (documented semantic). */
    assert(aegis_tool_args_add_string(args, "nul", NULL) == AEGIS_OK);
    assert(aegis_tool_args_find(args, "nul", &v) && v->as.str.len == 0 &&
           v->as.str.ptr[0] == '\0');

    /* Empty payloads allowed. */
    assert(aegis_tool_args_add_bytes(args, "empty", NULL, 0) == AEGIS_OK);
    assert(aegis_tool_args_find(args, "empty", &v) && v->as.bytes.len == 0);

    aegis_tool_args_destroy(args);
    aegis_tool_args_destroy(NULL); /* no-op */
}

/* ── Results ────────────────────────────────────────────────────────── */

static void test_result_helpers(void)
{
    aegis_tool_result_t r;
    memset(&r, 0, sizeof(r));

    /* Zeroed result is safe to destroy. */
    aegis_tool_result_destroy(&r);

    assert(aegis_tool_result_set_bool(&r, true) == AEGIS_OK);
    assert(r.value.type == AEGIS_TOOL_VAL_BOOL && r.value.as.b);
    aegis_tool_result_destroy(&r);

    assert(aegis_tool_result_set_int(&r, INT64_MIN) == AEGIS_OK);
    assert(r.value.type == AEGIS_TOOL_VAL_INT && r.value.as.i == INT64_MIN);

    assert(aegis_tool_result_set_float(&r, -0.5) == AEGIS_OK);
    assert(r.value.type == AEGIS_TOOL_VAL_FLOAT && r.value.as.f == -0.5);

    /* set replaces prior payload (int -> string): ASan verifies no leak/UAF. */
    assert(aegis_tool_result_set_string(&r, "payload") == AEGIS_OK);
    assert(r.value.type == AEGIS_TOOL_VAL_STRING && r.value.as.str.len == 7 &&
           strcmp(r.value.as.str.ptr, "payload") == 0);

    assert(aegis_tool_result_set_bytes(&r, "\x01\x00\x02", 3) == AEGIS_OK);
    assert(r.value.type == AEGIS_TOOL_VAL_BYTES && r.value.as.bytes.len == 3);

    assert(aegis_tool_result_set_string(NULL, "x") == AEGIS_ERR_NOMEM ||
           aegis_tool_result_set_string(NULL, "x") != AEGIS_OK); /* never crash */

    aegis_tool_result_destroy(&r);
    aegis_tool_result_destroy(NULL); /* no-op */
}

/* ── Schema validation ──────────────────────────────────────────────── */

static void test_validate_args(void)
{
    static const aegis_tool_param_spec_t params[] = {
        {"a", AEGIS_TOOL_VAL_INT, true, "first addend"},
        {"b", AEGIS_TOOL_VAL_INT, true, NULL},
        {"tag", AEGIS_TOOL_VAL_STRING, false, NULL},
    };
    static const aegis_tool_schema_t schema = {params, 3};

    /* NULL schema + empty args is valid. */
    assert(aegis_tool_validate_args(NULL, NULL) == AEGIS_OK);

    aegis_tool_args_t* args = NULL;
    assert(aegis_tool_args_create(&args) == AEGIS_OK);

    /* Missing required. */
    assert(aegis_tool_validate_args(&schema, args) == AEGIS_ERR_INVALID);
    assert(aegis_tool_args_add_int(args, "a", 1) == AEGIS_OK);
    assert(aegis_tool_validate_args(&schema, args) == AEGIS_ERR_INVALID);

    /* Unknown argument name. */
    assert(aegis_tool_args_add_int(args, "zzz", 5) == AEGIS_OK);
    assert(aegis_tool_validate_args(&schema, args) == AEGIS_ERR_INVALID);
    assert(aegis_tool_args_count(args) == 2); /* find by walking: remove via rebuild below */

    /* Rebuild clean list. */
    aegis_tool_args_destroy(args);
    args = NULL;
    assert(aegis_tool_args_create(&args) == AEGIS_OK);
    assert(aegis_tool_args_add_int(args, "a", 1) == AEGIS_OK);
    assert(aegis_tool_args_add_int(args, "b", 2) == AEGIS_OK);
    assert(aegis_tool_validate_args(&schema, args) == AEGIS_OK);

    /* Optional absent stays valid; optional present with right type too. */
    assert(aegis_tool_args_add_string(args, "tag", "x") == AEGIS_OK);
    assert(aegis_tool_validate_args(&schema, args) == AEGIS_OK);

    /* No coercion: FLOAT does not satisfy INT and vice versa. */
    assert(aegis_tool_args_add_float(args, "extra-f", 1.0) == AEGIS_OK);
    assert(aegis_tool_validate_args(&schema, args) == AEGIS_ERR_INVALID);

    aegis_tool_args_destroy(args);

    /* Wrong type on required param. */
    aegis_tool_args_t* wrong = NULL;
    assert(aegis_tool_args_create(&wrong) == AEGIS_OK);
    assert(aegis_tool_args_add_float(wrong, "a", 1.5) == AEGIS_OK);
    assert(aegis_tool_args_add_int(wrong, "b", 2) == AEGIS_OK);
    assert(aegis_tool_validate_args(&schema, wrong) == AEGIS_ERR_INVALID);
    aegis_tool_args_destroy(wrong);
}

/* ── Registry ───────────────────────────────────────────────────────── */

static aegis_status_t noop_execute(void* user, const aegis_tool_args_t* args,
                                   const aegis_cancellation_token_t* token,
                                   aegis_tool_result_t* out)
{
    (void)user;
    (void)args;
    (void)token;
    (void)out;
    return AEGIS_OK;
}

static void test_registry(void)
{
    aegis_tool_registry_t* reg = NULL;

    assert(aegis_tool_registry_create(NULL) == AEGIS_ERR_INVALID);
    assert(aegis_tool_registry_create(&reg) == AEGIS_OK);
    assert(aegis_tool_registry_count(reg) == 0);
    assert(aegis_tool_registry_count(NULL) == 0);

    aegis_tool_def_t def;
    memset(&def, 0, sizeof(def));
    def.name         = "noop";
    def.description  = "does nothing";
    def.capabilities = AEGIS_CAP_NONE;
    def.execute      = noop_execute;

    /* Validation before registration. */
    assert(aegis_tool_registry_register(reg, NULL) == AEGIS_ERR_INVALID);
    assert(aegis_tool_registry_register(NULL, &def) == AEGIS_ERR_INVALID);
    {
        aegis_tool_def_t bad = def;
        bad.name             = "";
        assert(aegis_tool_registry_register(reg, &bad) == AEGIS_ERR_INVALID);
        bad.name   = "ok-name";
        bad.execute = NULL;
        assert(aegis_tool_registry_register(reg, &bad) == AEGIS_ERR_INVALID);
    }
    assert(aegis_tool_registry_count(reg) == 0);

    assert(aegis_tool_registry_register(reg, &def) == AEGIS_OK);
    assert(aegis_tool_registry_count(reg) == 1);

    /* Duplicate name. */
    assert(aegis_tool_registry_register(reg, &def) == AEGIS_ERR_BUSY);
    assert(aegis_tool_registry_count(reg) == 1);

    /* Find returns a copy of the def. */
    aegis_tool_def_t found;
    memset(&found, 0xAB, sizeof(found));
    assert(aegis_tool_registry_find(reg, "noop", &found) == AEGIS_OK);
    assert(strcmp(found.name, "noop") == 0);
    assert(found.description && strcmp(found.description, "does nothing") == 0);
    assert(found.execute == noop_execute);
    assert(found.capabilities == AEGIS_CAP_NONE);
    assert(aegis_tool_registry_find(reg, "nope", &found) == AEGIS_ERR_NOT_FOUND);
    assert(aegis_tool_registry_find(reg, NULL, &found) == AEGIS_ERR_INVALID);
    assert(aegis_tool_registry_find(NULL, "noop", &found) == AEGIS_ERR_INVALID);

    /* Several more registrations; destroy frees every owned copy
     * (ASan validates). Names use static storage because stored defs
     * borrow the name pointer. */
    {
        static char bulk_names[40][32];
        for (int i = 0; i < 40; i++) {
            aegis_tool_def_t d = def;
            snprintf(bulk_names[i], sizeof(bulk_names[i]), "bulk-%d", i);
            d.name = bulk_names[i];
            assert(aegis_tool_registry_register(reg, &d) == AEGIS_OK);
        }
        assert(aegis_tool_registry_count(reg) == 41);
        aegis_tool_def_t one;
        assert(aegis_tool_registry_find(reg, "bulk-17", &one) == AEGIS_OK);
        assert(one.execute == noop_execute);
    }

    aegis_tool_registry_destroy(reg);
    aegis_tool_registry_destroy(NULL); /* no-op */
}

int main(void)
{
    test_args_lifecycle();
    test_result_helpers();
    test_validate_args();
    test_registry();
    printf("test_tool: all cases passed\n");
    return 0;
}
