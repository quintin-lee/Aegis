/**
 * @file mcp_json.h
 * @brief General-purpose JSON value DOM + recursive-descent parser.
 *
 * Extracted from the agent loop's static tool-args parser so both the loop
 * and the MCP client can share one JSON value model. Supports arbitrary
 * nesting of objects, arrays, strings, integers, floats, booleans and null.
 */
#ifndef AEGIS_MCP_JSON_H
#define AEGIS_MCP_JSON_H

#include "aegis/types.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Discriminator for a JSON value. */
typedef enum aegis_json_type {
    AEGIS_JSON_NULL,
    AEGIS_JSON_BOOL,
    AEGIS_JSON_INT,
    AEGIS_JSON_FLOAT,
    AEGIS_JSON_STRING,
    AEGIS_JSON_ARRAY,
    AEGIS_JSON_OBJECT,
} aegis_json_type_t;

typedef struct aegis_json_value aegis_json_value_t;
struct aegis_json_value {
    aegis_json_type_t type;
    union {
        bool    b;
        int64_t i;
        double  f;
        char*   str; /* NUL-terminated, owned */
        struct {
            aegis_json_value_t** items;
            size_t               count;
        } arr;
        struct {
            char**               keys;
            aegis_json_value_t** vals;
            size_t               count;
        } obj;
    };
};

/**
 * @brief Parse a full JSON document into an owned DOM tree.
 *
 * Accepts objects, arrays, strings, numbers, booleans and null with
 * arbitrary nesting.
 *
 * @param[in]  json NUL-terminated JSON text.
 * @param[out] out  Receives the owned tree on success; caller frees with
 *   @ref aegis_json_value_destroy. Set to NULL on failure.
 * @return AEGIS_OK on success, AEGIS_ERR_INVALID on malformed input,
 *   AEGIS_ERR_NOMEM on allocation failure.
 */
aegis_status_t aegis_json_parse(const char* json, aegis_json_value_t** out);

/** Recursively free a DOM tree. Safe with NULL. */
void aegis_json_value_destroy(aegis_json_value_t* v);

/* ── DOM construction (for building JSON-RPC request params) ──────────── */

/**
 * @brief Create a new empty JSON object (or array when @p is_array).
 *
 * @param[out] out    Receives the owned value; set to NULL on failure.
 * @param[in]  is_array TRUE for an array, FALSE for an object.
 * @return AEGIS_OK, AEGIS_ERR_INVALID (NULL out), AEGIS_ERR_NOMEM.
 */
aegis_status_t aegis_json_new_object(aegis_json_value_t** out, bool is_array);

/**
 * @brief Create a new JSON string value from a NUL-terminated source.
 *
 * The value owns a copy of @p src.
 *
 * @param[in]  src  Source string (NULL yields the empty string "").
 * @param[out] out  Receives the owned string value; NULL on failure.
 * @return AEGIS_OK, AEGIS_ERR_INVALID (NULL out), AEGIS_ERR_NOMEM.
 */
aegis_status_t aegis_json_new_string(const char* src, aegis_json_value_t** out);

/**
 * @brief Insert a key/value pair into a JSON object.
 *
 * The key string and the value are deep-copied; @p key must be a string
 * value, @p obj must be an object. The object grows its internal arrays.
 *
 * @param[in] obj    Target object (non-NULL, type OBJECT).
 * @param[in] key    String value to use as the key (type STRING).
 * @param[in] val    Value to insert (may be any type; copied).
 * @return AEGIS_OK, AEGIS_ERR_INVALID (bad arg types), AEGIS_ERR_NOMEM.
 */
aegis_status_t aegis_json_object_insert(aegis_json_value_t* obj, const aegis_json_value_t* key,
                                        const aegis_json_value_t* val);

/**
 * @brief Serialize a DOM value to a NUL-terminated JSON string.
 *
 * Produces compact JSON (no extra whitespace). Strings are escaped with the
 * same set of escapes the parser accepts. Integers print as int64, floats in
 * a round-trip-safe form. The result is heap-owned; the caller must free() it.
 *
 * @param[in]  v    DOM value to serialize (NULL produces "null").
 * @param[out] out  Receives the malloc'd JSON text on success.
 * @return AEGIS_OK on success, AEGIS_ERR_NOMEM on allocation failure.
 */
aegis_status_t aegis_json_serialize(const aegis_json_value_t* v, char** out);

/**
 * @brief Create an owned, empty JSON object (or array when @p is_array).
 *
 * @param[out] out    Receives the owned value (calloc'd; zero members).
 * @param[in]  is_array true for an array, false for an object.
 * @return AEGIS_OK on success, AEGIS_ERR_NOMEM on allocation failure.
 */
aegis_status_t aegis_json_new_object(aegis_json_value_t** out, bool is_array);

/**
 * @brief Create an owned JSON string value from @p src (NULL -> "").
 *
 * @param[in]  src  String payload to copy (may be NULL).
 * @param[out] out  Receives the owned value.
 * @return AEGIS_OK on success, AEGIS_ERR_NOMEM on allocation failure.
 */
aegis_status_t aegis_json_new_string(const char* src, aegis_json_value_t** out);

/**
 * @brief Append a key/value pair to an owned JSON object.
 *
 * @p key must be a STRING value; @p val is deep-copied so the caller may
 * free its temporary afterwards.
 *
 * @param[in] obj  Target object.
 * @param[in] key  String value to use as the key.
 * @param[in] val  Value to insert (deep-copied).
 * @return AEGIS_OK on success, AEGIS_ERR_INVALID if @p obj is not an object
 *   or @p key is not a string, AEGIS_ERR_NOMEM on allocation failure.
 */
aegis_status_t aegis_json_object_insert(aegis_json_value_t* obj, const aegis_json_value_t* key,
                                        const aegis_json_value_t* val);

/** Object lookup by key. NULL if not found or @p obj is not an object. */
const aegis_json_value_t* aegis_json_object_get(const aegis_json_value_t* obj, const char* key);
/** Array element. NULL if out of range or @p arr is not an array. */
const aegis_json_value_t* aegis_json_array_at(const aegis_json_value_t* arr, size_t i);
/** Number of array elements (0 unless @p arr is an array). */
size_t aegis_json_array_len(const aegis_json_value_t* arr);
/** String payload (NULL unless @p v is a string). */
const char* aegis_json_string(const aegis_json_value_t* v);
/** Integer payload (0 unless @p v is an int). */
int64_t aegis_json_int(const aegis_json_value_t* v);
/** Boolean payload (false unless @p v is a bool). */
bool aegis_json_bool(const aegis_json_value_t* v);
/** Float payload (0.0 unless @p v is a float or int). */
double aegis_json_float(const aegis_json_value_t* v);

#ifdef __cplusplus
}
#endif

#endif /* AEGIS_MCP_JSON_H */
