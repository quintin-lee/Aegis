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
#include <math.h>
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
    const char* p   = c->p;
    const char* end = c->end;
    size_t      cap = 32, len = 0;
    char*       s = malloc(cap);
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
    char**               keys  = NULL;
    aegis_json_value_t** vals  = NULL;
    size_t               count = 0, cap = 0;
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
            cap       = cap ? cap * 2 : 8;
            char** nk = realloc(keys, cap * sizeof(*keys));
            if (!nk) {
                aegis_json_value_destroy(val);
                free(key);
                goto fail;
            }
            keys                    = nk;
            aegis_json_value_t** nv = realloc(vals, cap * sizeof(*vals));
            if (!nv) {
                aegis_json_value_destroy(val);
                free(key);
                goto fail;
            }
            vals = nv;
        }
        keys[count] = key;
        vals[count] = val;
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
    v->obj.keys  = keys;
    v->obj.vals  = vals;
    v->obj.count = count;
    *out         = v;
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
            cap                     = cap ? cap * 2 : 8;
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
    errno          = 0;
    char*  num_end = NULL;
    double num     = strtod(c->p, &num_end);
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
    c->p                  = num_end;
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
    *out                    = NULL;
    parse_ctx_t         ctx = {.p = json, .end = json + strlen(json)};
    aegis_json_value_t* v   = NULL;
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

/* ── DOM construction ────────────────────────────────────────────────── */

/** Deep-copy a DOM value. Returns a new owned value, or NULL on NOMEM. */
static aegis_json_value_t* copy_value(const aegis_json_value_t* src)
{
    if (!src) {
        return NULL;
    }
    aegis_json_value_t* v = new_value(src->type);
    if (!v) {
        return NULL;
    }
    switch (src->type) {
    case AEGIS_JSON_NULL:
    case AEGIS_JSON_BOOL:
        v->b = src->b;
        break;
    case AEGIS_JSON_INT:
        v->i = src->i;
        break;
    case AEGIS_JSON_FLOAT:
        v->f = src->f;
        break;
    case AEGIS_JSON_STRING:
        v->str = strdup(src->str ? src->str : "");
        if (!v->str) {
            aegis_json_value_destroy(v);
            return NULL;
        }
        break;
    case AEGIS_JSON_ARRAY: {
        if (src->arr.count) {
            v->arr.items = calloc(src->arr.count, sizeof(*v->arr.items));
            if (!v->arr.items) {
                aegis_json_value_destroy(v);
                return NULL;
            }
            for (size_t i = 0; i < src->arr.count; ++i) {
                v->arr.items[i] = copy_value(src->arr.items[i]);
                if (!v->arr.items[i]) {
                    for (size_t j = 0; j <= i; ++j) {
                        aegis_json_value_destroy(v->arr.items[j]);
                    }
                    free(v->arr.items);
                    aegis_json_value_destroy(v);
                    return NULL;
                }
            }
        }
        v->arr.count = src->arr.count;
        break;
    }
    case AEGIS_JSON_OBJECT: {
        if (src->obj.count) {
            v->obj.keys = calloc(src->obj.count, sizeof(*v->obj.keys));
            v->obj.vals = calloc(src->obj.count, sizeof(*v->obj.vals));
            if (!v->obj.keys || !v->obj.vals) {
                free(v->obj.keys);
                free(v->obj.vals);
                aegis_json_value_destroy(v);
                return NULL;
            }
            for (size_t i = 0; i < src->obj.count; ++i) {
                v->obj.keys[i] = strdup(src->obj.keys[i]);
                v->obj.vals[i] = copy_value(src->obj.vals[i]);
                if (!v->obj.keys[i] || !v->obj.vals[i]) {
                    for (size_t j = 0; j <= i; ++j) {
                        free(v->obj.keys[j]);
                        aegis_json_value_destroy(v->obj.vals[j]);
                    }
                    free(v->obj.keys);
                    free(v->obj.vals);
                    aegis_json_value_destroy(v);
                    return NULL;
                }
            }
        }
        v->obj.count = src->obj.count;
        break;
    }
    default:
        break;
    }
    return v;
}

/** Grow the object's keys/vals arrays to fit one more entry. */
static int obj_grow(aegis_json_value_t* o)
{
    size_t cap = o->obj.count ? o->obj.count : 4;
    cap        = o->obj.count >= cap ? cap * 2 : cap;
    char** nk  = realloc(o->obj.keys, cap * sizeof(*nk));
    if (!nk) {
        return 0;
    }
    o->obj.keys             = nk;
    aegis_json_value_t** nv = realloc(o->obj.vals, cap * sizeof(*nv));
    if (!nv) {
        return 0;
    }
    o->obj.vals = nv;
    return 1;
}

aegis_status_t aegis_json_new_object(aegis_json_value_t** out, bool is_array)
{
    if (!out) {
        return AEGIS_ERR_INVALID;
    }
    *out = new_value(is_array ? AEGIS_JSON_ARRAY : AEGIS_JSON_OBJECT);
    return *out ? AEGIS_OK : AEGIS_ERR_NOMEM;
}

aegis_status_t aegis_json_new_string(const char* src, aegis_json_value_t** out)
{
    if (!out) {
        return AEGIS_ERR_INVALID;
    }
    aegis_json_value_t* v = new_value(AEGIS_JSON_STRING);
    if (!v) {
        return AEGIS_ERR_NOMEM;
    }
    v->str = strdup(src ? src : "");
    if (!v->str) {
        free(v);
        return AEGIS_ERR_NOMEM;
    }
    *out = v;
    return AEGIS_OK;
}

aegis_status_t aegis_json_object_insert(aegis_json_value_t* obj, const aegis_json_value_t* key,
                                        const aegis_json_value_t* val)
{
    if (!obj || !key || !val) {
        return AEGIS_ERR_INVALID;
    }
    if (obj->type != AEGIS_JSON_OBJECT || key->type != AEGIS_JSON_STRING) {
        return AEGIS_ERR_INVALID;
    }
    if (!obj_grow(obj)) {
        return AEGIS_ERR_NOMEM;
    }
    aegis_json_value_t* vcopy = copy_value(val);
    if (!vcopy) {
        return AEGIS_ERR_NOMEM;
    }
    char* kcopy = key->str ? strdup(key->str) : strdup("");
    if (!kcopy) {
        aegis_json_value_destroy(vcopy);
        return AEGIS_ERR_NOMEM;
    }
    obj->obj.keys[obj->obj.count] = kcopy;
    obj->obj.vals[obj->obj.count] = vcopy;
    ++obj->obj.count;
    return AEGIS_OK;
}

/* ── Serialization ───────────────────────────────────────────────────── */

typedef struct {
    char*  data;
    size_t len;
    size_t cap;
} ser_buf_t;

static int ser_grow(ser_buf_t* b, size_t extra)
{
    if (b->len + extra + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 64;
        while (cap < b->len + extra + 1) {
            cap *= 2;
        }
        char* p = realloc(b->data, cap);
        if (!p) {
            return 0;
        }
        b->data = p;
        b->cap  = cap;
    }
    return 1;
}

static int ser_append(ser_buf_t* b, const char* s, size_t n)
{
    if (!ser_grow(b, n)) {
        return 0;
    }
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
    return 1;
}

/* Serialize a single escaped JSON string body (no surrounding quotes) into b.
 * Handles the same escapes as the parser: " \ \\ and \n \r \t. */
static int ser_string_body(ser_buf_t* b, const char* s)
{
    if (!s) {
        s = "";
    }
    for (const unsigned char* p = (const unsigned char*)s; *p; ++p) {
        char   esc[7];
        size_t n = 1;
        switch (*p) {
        case '"':
            esc[0] = '\\';
            esc[1] = '"';
            n      = 2;
            break;
        case '\\':
            esc[0] = '\\';
            esc[1] = '\\';
            n      = 2;
            break;
        case '\n':
            esc[0] = '\\';
            esc[1] = 'n';
            n      = 2;
            break;
        case '\r':
            esc[0] = '\\';
            esc[1] = 'r';
            n      = 2;
            break;
        case '\t':
            esc[0] = '\\';
            esc[1] = 't';
            n      = 2;
            break;
        default:
            if (*p < 0x20) {
                int written = snprintf(esc, sizeof(esc), "\\u%04x", *p);
                if (written != 6) {
                    return 0;
                }
                n = 6;
            } else {
                esc[0] = (char)*p;
            }
            break;
        }
        if (!ser_append(b, esc, n)) {
            return 0;
        }
    }
    return 1;
}

static int ser_value(const aegis_json_value_t* v, ser_buf_t* b)
{
    if (!v) {
        return ser_append(b, "null", 4);
    }
    switch (v->type) {
    case AEGIS_JSON_NULL:
        return ser_append(b, "null", 4);
    case AEGIS_JSON_BOOL:
        return ser_append(b, v->b ? "true" : "false", v->b ? 4 : 5);
    case AEGIS_JSON_INT: {
        char tmp[32];
        int  k = snprintf(tmp, sizeof(tmp), "%lld", (long long)v->i);
        return (k > 0 && (size_t)k < sizeof(tmp)) ? ser_append(b, tmp, (size_t)k) : 0;
    }
    case AEGIS_JSON_FLOAT: {
        char tmp[32];
        int  k = snprintf(tmp, sizeof(tmp), "%g", v->f);
        /* %g can emit "inf"/"nan"; not valid JSON. Guard: if no digit, emit 0. */
        if (k <= 0 || (size_t)k >= sizeof(tmp)) {
            return 0;
        }
        int has_digit = 0;
        for (const char* q = tmp; *q; ++q) {
            if (isdigit((unsigned char)*q)) {
                has_digit = 1;
                break;
            }
        }
        if (!has_digit) {
            return ser_append(b, "0", 1);
        }
        return ser_append(b, tmp, (size_t)k);
    }
    case AEGIS_JSON_STRING:
        if (!ser_append(b, "\"", 1)) {
            return 0;
        }
        if (!ser_string_body(b, v->str)) {
            return 0;
        }
        return ser_append(b, "\"", 1);
    case AEGIS_JSON_ARRAY: {
        if (!ser_append(b, "[", 1)) {
            return 0;
        }
        for (size_t i = 0; i < v->arr.count; ++i) {
            if (i && !ser_append(b, ",", 1)) {
                return 0;
            }
            if (!ser_value(v->arr.items[i], b)) {
                return 0;
            }
        }
        return ser_append(b, "]", 1);
    }
    case AEGIS_JSON_OBJECT: {
        if (!ser_append(b, "{", 1)) {
            return 0;
        }
        for (size_t i = 0; i < v->obj.count; ++i) {
            if (i && !ser_append(b, ",", 1)) {
                return 0;
            }
            if (!ser_append(b, "\"", 1) || !ser_string_body(b, v->obj.keys[i]) ||
                !ser_append(b, "\":", 2)) {
                return 0;
            }
            if (!ser_value(v->obj.vals[i], b)) {
                return 0;
            }
        }
        return ser_append(b, "}", 1);
    }
    default:
        return 0;
    }
}

aegis_status_t aegis_json_serialize(const aegis_json_value_t* v, char** out)
{
    if (!out) {
        return AEGIS_ERR_INVALID;
    }
    *out        = NULL;
    ser_buf_t b = {0};
    if (!ser_value(v, &b)) {
        free(b.data);
        return AEGIS_ERR_NOMEM;
    }
    *out = b.data;
    return AEGIS_OK;
}
