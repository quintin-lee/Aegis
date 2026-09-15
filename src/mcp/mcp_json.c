/**
 * @file mcp_json.c
 * @brief General-purpose JSON value DOM + recursive-descent parser.
 *
 * Ported from the agent loop's static tool-args parser. The parser is a
 * full recursive-descent (nested objects/arrays/scalars). String escaping
 * matches the original: \n \r \t \" \\ \/; \u is rejected.
 */
#define _POSIX_C_SOURCE 200809L
#include "mcp_json.h"
#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>

typedef struct {
    const char* p;
    const char* end;
} parse_ctx_t;

static int skip_ws(parse_ctx_t* c)
{
    while (c->p < c->end && isspace((unsigned char)*c->p)) {
        ++c->p;
    }
    return c->p < c->end;
}

static aegis_json_value_t* new_value(aegis_json_type_t type)
{
    aegis_json_value_t* v = calloc(1, sizeof(*v));
    if (v) {
        v->type = type;
    }
    return v;
}

/* Parse a JSON string (opening quote consumed by caller). Returns owned
 * NUL-terminated char*, or NULL on error. Advances *c past the closing quote. */
static char* parse_string_raw(parse_ctx_t* c)
{
    const char* p = c->p;
    const char* end = c->end;
    size_t cap = 32, len = 0;
    char*  s = malloc(cap);
    if (!s) {
        return NULL;
    }
    while (p < end && *p != '"') {
        unsigned char ch = (unsigned char)*p++;
        if (ch == '\\') {
            if (p >= end) {
                free(s);
                return NULL;
            }
            ch = (unsigned char)*p++;
            if (ch == 'n') {
                ch = '\n';
            } else if (ch == 'r') {
                ch = '\r';
            } else if (ch == 't') {
                ch = '\t';
            } else if (ch != '"' && ch != '\\' && ch != '/') {
                free(s);
                return NULL;
            }
        }
        if (len + 1 >= cap) {
            cap *= 2;
            char* n = realloc(s, cap);
            if (!n) {
                free(s);
                return NULL;
            }
            s = n;
        }
        s[len++] = (char)ch;
    }
    if (p >= end || *p != '"') {
        free(s);
        return NULL;
    }
    ++p;
    s[len] = '\0';
    c->p   = p;
    return s;
}

/* Forward decl. */
static int parse_value(parse_ctx_t* c, aegis_json_value_t** out);

static int parse_object(parse_ctx_t* c, aegis_json_value_t** out)
{
    /* Opening '{' consumed by caller. */
    aegis_json_value_t* v = new_value(AEGIS_JSON_OBJECT);
    if (!v) {
        return 0;
    }
    skip_ws(c);
    if (c->p < c->end && *c->p == '}') {
        ++c->p;
        *out = v;
        return 1;
    }
    char** keys = NULL;
    aegis_json_value_t** vals = NULL;
    size_t count = 0, cap = 0;
    for (;;) {
        skip_ws(c);
        if (c->p >= c->end || *c->p != '"') {
            goto fail;
        }
        ++c->p;
        char* key = parse_string_raw(c);
        if (!key) {
            goto fail;
        }
        skip_ws(c);
        if (c->p >= c->end || *c->p != ':') {
            free(key);
            goto fail;
        }
        ++c->p;
        aegis_json_value_t* val = NULL;
        if (!parse_value(c, &val)) {
            free(key);
            goto fail;
        }
        if (count + 1 > cap) {
            cap = cap ? cap * 2 : 8;
            char** nk = realloc(keys, cap * sizeof(*keys));
            if (!nk) {
                aegis_json_value_destroy(val);
                free(key);
                goto fail;
            }
            keys = nk;
            aegis_json_value_t** nv = realloc(vals, cap * sizeof(*vals));
            if (!nv) {
                aegis_json_value_destroy(val);
                free(key);
                goto fail;
            }
            vals = nv;
        }
        keys[count]  = key;
        vals[count]  = val;
        ++count;
        skip_ws(c);
        if (c->p >= c->end) {
            goto fail;
        }
        if (*c->p == ',') {
            ++c->p;
            continue;
        }
        if (*c->p == '}') {
            ++c->p;
            break;
        }
        goto fail;
    }
    v->obj.keys   = keys;
    v->obj.vals   = vals;
    v->obj.count  = count;
    *out          = v;
    return 1;
fail:
    for (size_t i = 0; i < count; ++i) {
        free(keys[i]);
        aegis_json_value_destroy(vals[i]);
    }
    free(keys);
    free(vals);
    aegis_json_value_destroy(v);
    return 0;
}

static int parse_array(parse_ctx_t* c, aegis_json_value_t** out)
{
    /* Opening '[' consumed by caller. */
    aegis_json_value_t* v = new_value(AEGIS_JSON_ARRAY);
    if (!v) {
        return 0;
    }
    skip_ws(c);
    if (c->p < c->end && *c->p == ']') {
        ++c->p;
        *out = v;
        return 1;
    }
    aegis_json_value_t** items = NULL;
    size_t               count = 0, cap = 0;
    for (;;) {
        aegis_json_value_t* el = NULL;
        if (!parse_value(c, &el)) {
            goto fail;
        }
        if (count + 1 > cap) {
            cap = cap ? cap * 2 : 8;
            aegis_json_value_t** ni = realloc(items, cap * sizeof(*ni));
            if (!ni) {
                aegis_json_value_destroy(el);
                goto fail;
            }
            items = ni;
        }
        items[count] = el;
        ++count;
        skip_ws(c);
        if (c->p >= c->end) {
            goto fail;
        }
        if (*c->p == ',') {
            ++c->p;
            continue;
        }
        if (*c->p == ']') {
            ++c->p;
            break;
        }
        goto fail;
    }
    v->arr.items = items;
    v->arr.count = count;
    *out         = v;
    return 1;
fail:
    for (size_t i = 0; i < count; ++i) {
        aegis_json_value_destroy(items[i]);
    }
    free(items);
    aegis_json_value_destroy(v);
    return 0;
}

static int parse_value(parse_ctx_t* c, aegis_json_value_t** out)
{
    skip_ws(c);
    if (c->p >= c->end) {
        return 0;
    }
    char ch = *c->p;
    if (ch == '{') {
        ++c->p;
        return parse_object(c, out);
    }
    if (ch == '[') {
        ++c->p;
        return parse_array(c, out);
    }
    if (ch == '"') {
        ++c->p;
        char* s = parse_string_raw(c);
        if (!s) {
            return 0;
        }
        aegis_json_value_t* v = new_value(AEGIS_JSON_STRING);
        if (!v) {
            free(s);
            return 0;
        }
        v->str = s;
        *out   = v;
        return 1;
    }
    if (ch == 't' && c->end - c->p >= 4 && strncmp(c->p, "true", 4) == 0) {
        c->p += 4;
        aegis_json_value_t* v = new_value(AEGIS_JSON_BOOL);
        if (!v) {
            return 0;
        }
        v->b = true;
        *out = v;
        return 1;
    }
    if (ch == 'f' && c->end - c->p >= 5 && strncmp(c->p, "false", 5) == 0) {
        c->p += 5;
        aegis_json_value_t* v = new_value(AEGIS_JSON_BOOL);
        if (!v) {
            return 0;
        }
        v->b = false;
        *out = v;
        return 1;
    }
    if (ch == 'n' && c->end - c->p >= 4 && strncmp(c->p, "null", 4) == 0) {
        c->p += 4;
        aegis_json_value_t* v = new_value(AEGIS_JSON_NULL);
        if (!v) {
            return 0;
        }
        *out = v;
        return 1;
    }
    /* Number: parse via strtod; tag INT when the literal has no '.'/'e'/'E'. */
    errno = 0;
    char* num_end = NULL;
    double num    = strtod(c->p, &num_end);
    if (num_end == c->p || errno == ERANGE || num != num) {
        return 0;
    }
    bool is_float = false;
    for (const char* q = c->p; q < num_end; ++q) {
        if (*q == '.' || *q == 'e' || *q == 'E') {
            is_float = true;
            break;
        }
    }
    c->p = num_end;
    aegis_json_value_t* v = new_value(is_float ? AEGIS_JSON_FLOAT : AEGIS_JSON_INT);
    if (!v) {
        return 0;
    }
    if (is_float) {
        v->f = num;
    } else {
        if (num < (double)LLONG_MIN || num > (double)LLONG_MAX) {
            /* Out of int64 range: keep as float instead of clamping. */
            v->type = AEGIS_JSON_FLOAT;
            v->f    = num;
        } else {
            v->i = (int64_t)num;
        }
    }
    *out = v;
    return 1;
}

aegis_status_t aegis_json_parse(const char* json, aegis_json_value_t** out)
{
    if (!json || !out) {
        return AEGIS_ERR_INVALID;
    }
    *out = NULL;
    parse_ctx_t ctx = {.p = json, .end = json + strlen(json)};
    aegis_json_value_t* v = NULL;
    if (!parse_value(&ctx, &v)) {
        return AEGIS_ERR_INVALID;
    }
    /* Reject trailing garbage. */
    skip_ws(&ctx);
    if (ctx.p != ctx.end) {
        aegis_json_value_destroy(v);
        return AEGIS_ERR_INVALID;
    }
    *out = v;
    return AEGIS_OK;
}

void aegis_json_value_destroy(aegis_json_value_t* v)
{
    if (!v) {
        return;
    }
    switch (v->type) {
    case AEGIS_JSON_STRING:
        free(v->str);
        break;
    case AEGIS_JSON_ARRAY:
        for (size_t i = 0; i < v->arr.count; ++i) {
            aegis_json_value_destroy(v->arr.items[i]);
        }
        free(v->arr.items);
        break;
    case AEGIS_JSON_OBJECT:
        for (size_t i = 0; i < v->obj.count; ++i) {
            free(v->obj.keys[i]);
            aegis_json_value_destroy(v->obj.vals[i]);
        }
        free(v->obj.keys);
        free(v->obj.vals);
        break;
    default:
        break;
    }
    free(v);
}

const aegis_json_value_t* aegis_json_object_get(const aegis_json_value_t* obj, const char* key)
{
    if (!obj || obj->type != AEGIS_JSON_OBJECT || !key) {
        return NULL;
    }
    for (size_t i = 0; i < obj->obj.count; ++i) {
        if (strcmp(obj->obj.keys[i], key) == 0) {
            return obj->obj.vals[i];
        }
    }
    return NULL;
}

const aegis_json_value_t* aegis_json_array_at(const aegis_json_value_t* arr, size_t i)
{
    if (!arr || arr->type != AEGIS_JSON_ARRAY || i >= arr->arr.count) {
        return NULL;
    }
    return arr->arr.items[i];
}

size_t aegis_json_array_len(const aegis_json_value_t* arr)
{
    return (arr && arr->type == AEGIS_JSON_ARRAY) ? arr->arr.count : 0;
}

const char* aegis_json_string(const aegis_json_value_t* v)
{
    return (v && v->type == AEGIS_JSON_STRING) ? v->str : NULL;
}

int64_t aegis_json_int(const aegis_json_value_t* v)
{
    if (!v) {
        return 0;
    }
    if (v->type == AEGIS_JSON_INT) {
        return v->i;
    }
    if (v->type == AEGIS_JSON_FLOAT) {
        return (int64_t)v->f;
    }
    return 0;
}

bool aegis_json_bool(const aegis_json_value_t* v)
{
    return v && v->type == AEGIS_JSON_BOOL && v->b;
}

double aegis_json_float(const aegis_json_value_t* v)
{
    if (!v) {
        return 0.0;
    }
    if (v->type == AEGIS_JSON_FLOAT) {
        return v->f;
    }
    if (v->type == AEGIS_JSON_INT) {
        return (double)v->i;
    }
    return 0.0;
}
