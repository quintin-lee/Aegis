/**
 * @file tool.c
 * @brief Argument-list lifecycle and result payload helpers.
 *
 * Ownership: an argument list owns every entry name and every
 * STRING/BYTES payload. Results own their payload; the set_* helpers
 * replace (freeing) any prior payload so destroy stays idempotent.
 */
#include "tool_internal.h"

#include <stdlib.h>
#include <string.h>

/* ── Value payload helpers ────────────────────────────────────────────── */

static void value_clear(aegis_tool_value_t* v)
{
    if (!v) {
        return;
    }
    if (v->type == AEGIS_TOOL_VAL_STRING) {
        free((void*)v->as.str.ptr);
    } else if (v->type == AEGIS_TOOL_VAL_BYTES) {
        free((void*)v->as.bytes.ptr);
    }
    memset(v, 0, sizeof(*v));
}

/**
 * @brief Clear an argument entry: free its name and value payload.
 *
 * NULL is a no-op. After clearing the entry is zeroed so a subsequent
 * clear() call is safe.
 *
 * @param[in] entry Entry to clear, or NULL.
 */
void aegis_tool_arg_entry_clear(aegis_tool_arg_entry_t* entry)
{
    if (!entry) {
        return;
    }
    free(entry->name);
    entry->name = NULL;
    value_clear(&entry->value);
}

/* ── Argument list lifecycle ──────────────────────────────────────────── */

/**
 * @brief Create an empty argument list.
 *
 * The caller owns the result and must call aegis_tool_args_destroy.
 *
 * @param[out] out Receives the new list; untouched on failure.
 * @return AEGIS_OK on success, AEGIS_ERR_INVALID for NULL @p out,
 *   AEGIS_ERR_NOMEM on allocation failure.
 */
aegis_status_t aegis_tool_args_create(aegis_tool_args_t** out)
{
    if (!out) {
        return AEGIS_ERR_INVALID;
    }
    *out = calloc(1, sizeof(**out));
    return *out ? AEGIS_OK : AEGIS_ERR_NOMEM;
}

/**
 * @brief Destroy an argument list and every owned entry/payload it holds.
 *
 * NULL is a no-op. Each entry's name and STRING/BYTES payload are freed;
 * BOOL/INT/FLOAT payloads are zeroed.
 *
 * @param[in] args Argument list to destroy, or NULL.
 */
void aegis_tool_args_destroy(aegis_tool_args_t* args)
{
    if (!args) {
        return;
    }
    for (size_t i = 0; i < args->len; i++) {
        aegis_tool_arg_entry_clear(&args->items[i]);
    }
    free(args->items);
    free(args);
}

/**
 * @brief Return the number of arguments in the list.
 *
 * @param[in] args Argument list, or NULL.
 * @return Entry count, or 0 for NULL.
 */
size_t aegis_tool_args_count(const aegis_tool_args_t* args)
{
    return args ? args->len : 0;
}

/* ── Lookup (before add_entry: duplicate-name rejection reuses it) ────── */

/**
 * @brief Look up an argument by name.
 *
 * Returns true and sets @p out to a borrowed pointer into the list when
 * found. The pointer is valid until the list is mutated.
 *
 * @param[in]  args Argument list (must be non-NULL).
 * @param[in]  name Name to look up (must be non-NULL).
 * @param[out] out  Receives the value pointer; untouched when not found.
 * @return true when the argument exists, false otherwise.
 */
bool aegis_tool_args_find(const aegis_tool_args_t* args, const char* name,
                          const aegis_tool_value_t** out)
{
    if (!args || !name || !out) {
        return false;
    }
    for (size_t i = 0; i < args->len; i++) {
        if (strcmp(args->items[i].name, name) == 0) {
            *out = &args->items[i].value;
            return true;
        }
    }
    return false;
}

/* ── Add helpers ──────────────────────────────────────────────────────── */

static char* dup_cstr(const char* s)
{
    const size_t n = strlen(s) + 1;
    char*        p = malloc(n);
    if (p) {
        memcpy(p, s, n);
    }
    return p;
}

static aegis_status_t args_reserve_one(aegis_tool_args_t* args)
{
    if (args->len < args->cap) {
        return AEGIS_OK;
    }
    const size_t            next_cap = (args->cap == 0) ? 8 : args->cap * 2;
    aegis_tool_arg_entry_t* grown    = realloc(args->items, next_cap * sizeof(*grown));
    if (!grown) {
        return AEGIS_ERR_NOMEM;
    }
    args->items = grown;
    args->cap   = next_cap;
    return AEGIS_OK;
}

static aegis_status_t add_entry(aegis_tool_args_t* args, const char* name,
                                aegis_tool_value_type_t type, const void* payload, size_t len)
{
    if (!args || !name || name[0] == '\0') {
        return AEGIS_ERR_INVALID;
    }
    /* Duplicate names are ambiguous under schema validation: reject here. */
    const aegis_tool_value_t* existing = NULL;
    if (aegis_tool_args_find(args, name, &existing)) {
        return AEGIS_ERR_BUSY;
    }

    aegis_status_t st = args_reserve_one(args);
    if (st != AEGIS_OK) {
        return st;
    }

    aegis_tool_arg_entry_t e;
    memset(&e, 0, sizeof(e));
    e.name       = dup_cstr(name);
    e.value.type = type;

    if (!e.name) {
        return AEGIS_ERR_NOMEM;
    }

    bool heap_failed = false;
    if (type == AEGIS_TOOL_VAL_STRING) {
        const char*  s    = (const char*)payload;
        const size_t slen = s ? strlen(s) : 0;
        char*        copy = malloc(slen + 1);
        if (!copy) {
            heap_failed = true;
        } else {
            if (slen > 0) {
                memcpy(copy, s, slen);
            }
            copy[slen]         = '\0';
            e.value.as.str.ptr = copy;
            e.value.as.str.len = slen;
        }
    } else if (type == AEGIS_TOOL_VAL_BYTES) {
        uint8_t* copy = NULL;
        if (len > 0) {
            copy = malloc(len);
            if (!copy) {
                heap_failed = true;
            } else {
                memcpy(copy, payload, len);
            }
        }
        if (!heap_failed) {
            e.value.as.bytes.ptr = copy;
            e.value.as.bytes.len = len;
        }
    } else if (type == AEGIS_TOOL_VAL_BOOL) {
        e.value.as.b = *(const bool*)payload;
    } else if (type == AEGIS_TOOL_VAL_INT) {
        e.value.as.i = *(const int64_t*)payload;
    } else { /* AEGIS_TOOL_VAL_FLOAT */
        e.value.as.f = *(const double*)payload;
    }

    if (heap_failed) {
        aegis_tool_arg_entry_clear(&e);
        return AEGIS_ERR_NOMEM;
    }

    args->items[args->len++] = e;
    return AEGIS_OK;
}

/**
 * @brief Append a boolean argument.
 *
 * Duplicate names are rejected with AEGIS_ERR_BUSY.
 *
 * @param[in] args Argument list to extend.
 * @param[in] name Argument name (non-empty).
 * @param[in] v    Boolean value.
 * @return AEGIS_OK on success, AEGIS_ERR_INVALID for bad args,
 *   AEGIS_ERR_BUSY on duplicate name, AEGIS_ERR_NOMEM on allocation failure.
 */
aegis_status_t aegis_tool_args_add_bool(aegis_tool_args_t* args, const char* name, bool v)
{
    return add_entry(args, name, AEGIS_TOOL_VAL_BOOL, &v, 0);
}

/**
 * @brief Append an int64 argument.
 *
 * Duplicate names are rejected with AEGIS_ERR_BUSY.
 *
 * @param[in] args Argument list to extend.
 * @param[in] name Argument name (non-empty).
 * @param[in] v    Integer value.
 * @return AEGIS_OK on success, AEGIS_ERR_INVALID for bad args,
 *   AEGIS_ERR_BUSY on duplicate name, AEGIS_ERR_NOMEM on allocation failure.
 */
aegis_status_t aegis_tool_args_add_int(aegis_tool_args_t* args, const char* name, int64_t v)
{
    return add_entry(args, name, AEGIS_TOOL_VAL_INT, &v, 0);
}

/**
 * @brief Append a double-float argument.
 *
 * Duplicate names are rejected with AEGIS_ERR_BUSY.
 *
 * @param[in] args Argument list to extend.
 * @param[in] name Argument name (non-empty).
 * @param[in] v    Float value.
 * @return AEGIS_OK on success, AEGIS_ERR_INVALID for bad args,
 *   AEGIS_ERR_BUSY on duplicate name, AEGIS_ERR_NOMEM on allocation failure.
 */
aegis_status_t aegis_tool_args_add_float(aegis_tool_args_t* args, const char* name, double v)
{
    return add_entry(args, name, AEGIS_TOOL_VAL_FLOAT, &v, 0);
}

/**
 * @brief Append a string argument (deep-copied).
 *
 * Duplicate names are rejected with AEGIS_ERR_BUSY.
 *
 * @param[in] args Argument list to extend.
 * @param[in] name Argument name (non-empty).
 * @param[in] s    String value (may be NULL — treated as empty).
 * @return AEGIS_OK on success, AEGIS_ERR_INVALID for bad args,
 *   AEGIS_ERR_BUSY on duplicate name, AEGIS_ERR_NOMEM on allocation failure.
 */
aegis_status_t aegis_tool_args_add_string(aegis_tool_args_t* args, const char* name, const char* s)
{
    return add_entry(args, name, AEGIS_TOOL_VAL_STRING, s, 0);
}

/**
 * @brief Append a bytes argument (deep-copied).
 *
 * Duplicate names are rejected with AEGIS_ERR_BUSY.
 *
 * @param[in] args  Argument list to extend.
 * @param[in] name  Argument name (non-empty).
 * @param[in] data  Raw bytes (may be NULL when len == 0).
 * @param[in] len   Byte count.
 * @return AEGIS_OK on success, AEGIS_ERR_INVALID for bad args,
 *   AEGIS_ERR_BUSY on duplicate name, AEGIS_ERR_NOMEM on allocation failure.
 */
aegis_status_t aegis_tool_args_add_bytes(aegis_tool_args_t* args, const char* name,
                                         const void* data, size_t len)
{
    return add_entry(args, name, AEGIS_TOOL_VAL_BYTES, data, len);
}

/* ── Result helpers ───────────────────────────────────────────────────── */

/**
 * @brief Destroy a tool result, freeing its owned payload.
 *
 * NULL is a no-op. The result struct itself is NOT freed — the caller
 * owns it.
 *
 * @param[in] result Result to destroy, or NULL.
 */
void aegis_tool_result_destroy(aegis_tool_result_t* result)
{
    if (!result) {
        return;
    }
    value_clear(&result->value);
}

static char* dup_bytes(const void* data, size_t len)
{
    uint8_t* copy = malloc(len + 1); /* +1 keeps empty payloads distinct. */
    if (!copy) {
        return NULL;
    }
    if (len > 0) {
        memcpy(copy, data, len);
    }
    copy[len] = '\0';
    return (char*)copy;
}

/**
 * @brief Set the result to a string value (deep-copied).
 *
 * Replaces any prior payload. NULL @p s is rejected.
 *
 * @param[in] result Result container to fill.
 * @param[in] s      String value (non-NULL).
 * @return AEGIS_OK on success, AEGIS_ERR_INVALID for NULL args,
 *   AEGIS_ERR_NOMEM on allocation failure.
 */
aegis_status_t aegis_tool_result_set_string(aegis_tool_result_t* result, const char* s)
{
    if (!result || !s) {
        return AEGIS_ERR_INVALID;
    }
    const size_t len  = strlen(s);
    char*        copy = dup_bytes(s, len);
    if (!copy) {
        return AEGIS_ERR_NOMEM;
    }

    value_clear(&result->value);
    result->value.type       = AEGIS_TOOL_VAL_STRING;
    result->value.as.str.ptr = copy;
    result->value.as.str.len = len;
    return AEGIS_OK;
}

/**
 * @brief Set the result to a raw bytes value (deep-copied).
 *
 * Replaces any prior payload. NULL @p data is only accepted when len is 0.
 *
 * @param[in] result Result container to fill.
 * @param[in] data   Bytes to copy (may be NULL when len == 0).
 * @param[in] len    Byte count.
 * @return AEGIS_OK on success, AEGIS_ERR_INVALID for NULL args,
 *   AEGIS_ERR_NOMEM on allocation failure.
 */
aegis_status_t aegis_tool_result_set_bytes(aegis_tool_result_t* result, const void* data,
                                           size_t len)
{
    if (!result || (!data && len > 0)) {
        return AEGIS_ERR_INVALID;
    }
    char* copy = dup_bytes(data, len);
    if (!copy) {
        return AEGIS_ERR_NOMEM;
    }

    value_clear(&result->value);
    result->value.type         = AEGIS_TOOL_VAL_BYTES;
    result->value.as.bytes.ptr = copy;
    result->value.as.bytes.len = len;
    return AEGIS_OK;
}

/**
 * @brief Set the result to a boolean value.
 *
 * Replaces any prior payload.
 *
 * @param[in] result Result container to fill.
 * @param[in] v      Boolean value.
 * @return AEGIS_OK on success, AEGIS_ERR_INVALID for NULL result.
 */
aegis_status_t aegis_tool_result_set_bool(aegis_tool_result_t* result, bool v)
{
    if (!result) {
        return AEGIS_ERR_INVALID;
    }
    value_clear(&result->value);
    result->value.type = AEGIS_TOOL_VAL_BOOL;
    result->value.as.b = v;
    return AEGIS_OK;
}

/**
 * @brief Set the result to an int64 value.
 *
 * Replaces any prior payload.
 *
 * @param[in] result Result container to fill.
 * @param[in] v      Integer value.
 * @return AEGIS_OK on success, AEGIS_ERR_INVALID for NULL result.
 */
aegis_status_t aegis_tool_result_set_int(aegis_tool_result_t* result, int64_t v)
{
    if (!result) {
        return AEGIS_ERR_INVALID;
    }
    value_clear(&result->value);
    result->value.type = AEGIS_TOOL_VAL_INT;
    result->value.as.i = v;
    return AEGIS_OK;
}

/**
 * @brief Set the result to a double-float value.
 *
 * Replaces any prior payload.
 *
 * @param[in] result Result container to fill.
 * @param[in] v      Float value.
 * @return AEGIS_OK on success, AEGIS_ERR_INVALID for NULL result.
 */
aegis_status_t aegis_tool_result_set_float(aegis_tool_result_t* result, double v)
{
    if (!result) {
        return AEGIS_ERR_INVALID;
    }
    value_clear(&result->value);
    result->value.type = AEGIS_TOOL_VAL_FLOAT;
    result->value.as.f = v;
    return AEGIS_OK;
}
