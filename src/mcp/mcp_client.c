/**
 * @file mcp_client.c
 * @brief MCP client implementation: subprocess lifecycle, tool discovery,
 *        remote tool calls, and aegis tool-registry registration.
 */
#include "mcp_client.h"

#include "aegis/common/error.h"
#include "mcp_stdio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct aegis_mcp_client {
    aegis_mcp_stdio_t* stdio;
    /* Discovered tools (owned by this client). */
    aegis_mcp_tool_info_t* tools;
    size_t                 tool_count;
    /* DOM holding every tool's inputSchema (tool infos borrow from it). */
    aegis_json_value_t* schema_dom;
    /* Per-tool owned registration blobs, parallel to `tools` (NULL slots when
     * a tool was not registered via aegis_mcp_register_tools). */
    void** blobs;
};

/** Per-registered-tool owned state, referenced via the def's `user` field. */
typedef struct {
    aegis_mcp_client_t*      client;
    int                      index;
    aegis_tool_param_spec_t* params; /* owned array (NULL when param_count == 0) */
    size_t                   param_count;
} mcp_tool_blob_t;

/* ── Tool discovery ───────────────────────────────────────────────────── */

static aegis_status_t fetch_tools(aegis_mcp_client_t* c)
{
    if (c->tools != NULL) {
        return AEGIS_OK; /* already discovered */
    }
    aegis_json_value_t* result = NULL;
    aegis_status_t      st     = aegis_mcp_stdio_request(c->stdio, /*id*/ 1, "tools/list",
                                                         /*params*/ NULL, &result);
    if (st != AEGIS_OK) {
        return st;
    }
    /* result.tools is an array of {name, description, inputSchema}. */
    const aegis_json_value_t* tools = aegis_json_object_get(result, "tools");
    if (tools == NULL || tools->type != AEGIS_JSON_ARRAY) {
        aegis_json_value_destroy(result);
        return AEGIS_ERR_PROVIDER;
    }
    size_t                 n   = aegis_json_array_len(tools);
    aegis_mcp_tool_info_t* arr = calloc(n > 0 ? n : 1, sizeof(aegis_mcp_tool_info_t));
    if (arr == NULL) {
        aegis_json_value_destroy(result);
        return AEGIS_ERR_NOMEM;
    }
    for (size_t i = 0; i < n; ++i) {
        const aegis_json_value_t* t      = aegis_json_array_at(tools, i);
        const aegis_json_value_t* name   = aegis_json_object_get(t, "name");
        const aegis_json_value_t* desc   = aegis_json_object_get(t, "description");
        const aegis_json_value_t* schema = aegis_json_object_get(t, "inputSchema");
        snprintf(arr[i].name, sizeof(arr[i].name), "%s",
                 aegis_json_string(name) ? aegis_json_string(name) : "");
        snprintf(arr[i].description, sizeof(arr[i].description), "%s",
                 aegis_json_string(desc) ? aegis_json_string(desc) : "");
        arr[i].input_schema = (aegis_json_value_t*)schema; /* borrows from `result` DOM */
    }
    c->tools      = arr;
    c->tool_count = n;
    /* Parallel per-tool blob slots (NULL until aegis_mcp_register_tools). */
    c->blobs = calloc(n > 0 ? n : 1, sizeof(void*));
    if (c->blobs == NULL) {
        aegis_json_value_destroy(result);
        free(c->tools);
        c->tools = NULL;
        return AEGIS_ERR_NOMEM;
    }
    /* Keep `result` alive only until copy; tool infos copy name/desc but
     * input_schema points into `result`, so store it alongside. */
    c->schema_dom = result;
    return AEGIS_OK;
}

/* ── Registration blob helpers ───────────────────────────────────────── */

/**
 * Map a JSON Schema "type" string to an aegis tool value type.
 * Unknown types fall back to STRING (MCP schemas are descriptive; aegis
 * treats anything not explicitly typed as text).
 */
static aegis_tool_value_type_t json_schema_type_to_val(const char* type)
{
    if (type == NULL) {
        return AEGIS_TOOL_VAL_STRING;
    }
    if (strcmp(type, "string") == 0) {
        return AEGIS_TOOL_VAL_STRING;
    }
    if (strcmp(type, "integer") == 0 || strcmp(type, "number") == 0) {
        /* integer stays INT; number maps to FLOAT. */
        return strcmp(type, "integer") == 0 ? AEGIS_TOOL_VAL_INT : AEGIS_TOOL_VAL_FLOAT;
    }
    if (strcmp(type, "boolean") == 0) {
        return AEGIS_TOOL_VAL_BOOL;
    }
    return AEGIS_TOOL_VAL_STRING;
}

/**
 * Build an owned aegis_tool_param_spec_t array from a tool's inputSchema DOM.
 *
 * Walks inputSchema.properties (each property's "type" -> aegis value type,
 * "description" -> spec description) and marks members listed in
 * inputSchema.required. The returned array (and its strings) are heap-owned
 * and freed by the caller via the mcp_tool_blob_t.
 */
static aegis_tool_param_spec_t* build_param_specs(const aegis_json_value_t* schema,
                                                  size_t*                   out_count)
{
    *out_count = 0;
    if (schema == NULL) {
        return NULL;
    }
    const aegis_json_value_t* props = aegis_json_object_get(schema, "properties");
    if (props == NULL || props->type != AEGIS_JSON_OBJECT || props->obj.count == 0) {
        return NULL;
    }
    const aegis_json_value_t* required = aegis_json_object_get(schema, "required");

    aegis_tool_param_spec_t* specs = calloc(props->obj.count, sizeof(aegis_tool_param_spec_t));
    if (specs == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < props->obj.count; ++i) {
        const char*               key  = props->obj.keys[i];
        const aegis_json_value_t* pv   = props->obj.vals[i];
        const aegis_json_value_t* type = aegis_json_object_get(pv, "type");
        const aegis_json_value_t* desc = aegis_json_object_get(pv, "description");

        specs[i].name        = strdup(key);
        specs[i].type        = json_schema_type_to_val(aegis_json_string(type));
        specs[i].description = aegis_json_string(desc) ? strdup(aegis_json_string(desc)) : NULL;
        specs[i].required    = false;
        if (required != NULL && required->type == AEGIS_JSON_ARRAY) {
            for (size_t r = 0; r < aegis_json_array_len(required); ++r) {
                if (strcmp(aegis_json_string(aegis_json_array_at(required, r)), key) == 0) {
                    specs[i].required = true;
                    break;
                }
            }
        }
    }
    *out_count = props->obj.count;
    return specs;
}

/* ── Tool execute trampoline ──────────────────────────────────────────── */

/**
 * Convert an aegis tool-args list into a JSON-RPC "arguments" object.
 *
 * Enumerates the owning tool's parameter specs (aegis_tool_args_t exposes no
 * public name iterator), looks each name up in @p args, and emits {name: value}
 * for every spec that has a value present. STRING/BYTES values become JSON
 * strings; others map numerically. Returns an owned object DOM (NULL on NOMEM).
 */
static aegis_json_value_t* args_to_json_object(const mcp_tool_blob_t*   blob,
                                               const aegis_tool_args_t* args)
{
    aegis_json_value_t* obj = NULL;
    aegis_status_t      st  = aegis_json_new_object(&obj, /*is_array*/ false);
    if (st != AEGIS_OK) {
        return NULL;
    }
    if (blob == NULL || blob->params == NULL || args == NULL) {
        return obj; /* empty object */
    }
    for (size_t i = 0; i < blob->param_count; ++i) {
        const aegis_tool_param_spec_t* spec = &blob->params[i];
        const aegis_tool_value_t*      v    = NULL;
        if (!aegis_tool_args_find(args, spec->name, &v) || v == NULL) {
            continue; /* arg not supplied */
        }
        aegis_json_value_t key = {.type = AEGIS_JSON_STRING, .str = strdup(spec->name)};
        if (!key.str) {
            aegis_json_value_destroy(obj);
            return NULL;
        }
        aegis_json_value_t val;
        memset(&val, 0, sizeof(val));
        switch (v->type) {
        case AEGIS_TOOL_VAL_BOOL:
            val = (aegis_json_value_t){.type = AEGIS_JSON_BOOL, .b = v->as.b};
            break;
        case AEGIS_TOOL_VAL_INT:
            val = (aegis_json_value_t){.type = AEGIS_JSON_INT, .i = v->as.i};
            break;
        case AEGIS_TOOL_VAL_FLOAT:
            val = (aegis_json_value_t){.type = AEGIS_JSON_FLOAT, .f = v->as.f};
            break;
        case AEGIS_TOOL_VAL_STRING:
            val = (aegis_json_value_t){.type = AEGIS_JSON_STRING,
                                       .str  = v->as.str.ptr ? strdup(v->as.str.ptr) : strdup("")};
            break;
        case AEGIS_TOOL_VAL_BYTES:
            val = (aegis_json_value_t){
                .type = AEGIS_JSON_STRING,
                .str  = v->as.bytes.ptr ? strdup((const char*)v->as.bytes.ptr) : strdup("")};
            break;
        default:
            val = (aegis_json_value_t){.type = AEGIS_JSON_NULL};
            break;
        }
        if (val.type == AEGIS_JSON_STRING && !val.str) {
            free(key.str);
            aegis_json_value_destroy(obj);
            return NULL;
        }
        st = aegis_json_object_insert(obj, &key, &val);
        free(key.str); /* object_insert deep-copies the key */
        if (st != AEGIS_OK) {
            aegis_json_value_destroy(obj);
            return NULL;
        }
    }
    return obj;
}

/**
 * Locate the per-tool blob (with param specs) for a tool by name.
 *
 * Returns NULL when the tool is not registered (no blob yet), in which case
 * callers treat the arguments object as empty.
 */
static mcp_tool_blob_t* find_blob_by_name(aegis_mcp_client_t* c, const char* name)
{
    if (c == NULL || name == NULL || c->tools == NULL || c->blobs == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < c->tool_count; ++i) {
        if (strcmp(c->tools[i].name, name) == 0) {
            return c->blobs[i];
        }
    }
    return NULL;
}

/**
 * aegis tool execute hook for a remote MCP tool.
 *
 * Builds the JSON-RPC "arguments" object from the validated aegis args, sends
 * a "tools/call", concatenates any "text" content blocks of the result, and
 * stores the text in @p out. A remote tool that reports isError still yields
 * its text so the agent can surface the failure; only transport failures
 * return a non-OK status.
 *
 * @return AEGIS_OK when the call round-tripped (payload set or empty), or the
 *   transport error from aegis_mcp_stdio_request.
 */
static aegis_status_t mcp_tool_execute(void* user, const aegis_tool_args_t* args,
                                       const aegis_cancellation_token_t* token,
                                       aegis_tool_result_t*              out)
{
    (void)token; /* stdio request blocks; cancellation is checked by the caller */
    mcp_tool_blob_t*    blob = user;
    aegis_mcp_client_t* c    = blob->client;

    aegis_json_value_t* arguments = args_to_json_object(blob, args);
    aegis_json_value_t* params    = NULL;
    aegis_status_t      st        = aegis_json_new_object(&params, false);
    if (st != AEGIS_OK) {
        aegis_json_value_destroy(arguments);
        return st;
    }
    /* params = { "name": <tool name>, "arguments": <obj> } */
    aegis_json_value_t* name_key = NULL;
    st                           = aegis_json_new_string("name", &name_key);
    if (st != AEGIS_OK) {
        aegis_json_value_destroy(arguments);
        aegis_json_value_destroy(params);
        return st;
    }
    aegis_json_value_t* name_val = NULL;
    st                           = aegis_json_new_string(c->tools[blob->index].name, &name_val);
    if (st != AEGIS_OK) {
        aegis_json_value_destroy(name_key);
        aegis_json_value_destroy(arguments);
        aegis_json_value_destroy(params);
        return st;
    }
    st = aegis_json_object_insert(params, name_key, name_val);
    aegis_json_value_destroy(name_key);
    aegis_json_value_destroy(name_val);
    if (st != AEGIS_OK) {
        aegis_json_value_destroy(arguments);
        aegis_json_value_destroy(params);
        return st;
    }
    aegis_json_value_t* args_key = NULL;
    st                           = aegis_json_new_string("arguments", &args_key);
    if (st != AEGIS_OK) {
        aegis_json_value_destroy(arguments);
        aegis_json_value_destroy(params);
        return st;
    }
    st = aegis_json_object_insert(params, args_key, arguments);
    aegis_json_value_destroy(args_key);
    aegis_json_value_destroy(arguments);
    if (st != AEGIS_OK) {
        aegis_json_value_destroy(params);
        return st;
    }

    aegis_json_value_t* result = NULL;
    st = aegis_mcp_stdio_request(c->stdio, /*id*/ 2, "tools/call", params, &result);
    aegis_json_value_destroy(params);
    if (st != AEGIS_OK) {
        return st;
    }

    /* Concatenate result.content[].text into a single string. */
    const aegis_json_value_t* content  = aegis_json_object_get(result, "content");
    char*                     text     = NULL;
    size_t                    text_len = 0;
    if (content != NULL && content->type == AEGIS_JSON_ARRAY) {
        for (size_t i = 0; i < aegis_json_array_len(content); ++i) {
            const aegis_json_value_t* blk = aegis_json_array_at(content, i);
            const char*               txt = NULL;
            if (blk != NULL) {
                const aegis_json_value_t* t = aegis_json_object_get(blk, "text");
                txt                         = aegis_json_string(t);
            }
            if (txt == NULL) {
                continue;
            }
            size_t nl = strlen(txt);
            char*  nt = realloc(text, text_len + nl + 1);
            if (nt == NULL) {
                free(text);
                aegis_json_value_destroy(result);
                return AEGIS_ERR_NOMEM;
            }
            text = nt;
            memcpy(text + text_len, txt, nl + 1);
            text_len = text_len + nl;
        }
    }
    aegis_json_value_destroy(result);
    aegis_status_t rst = aegis_tool_result_set_string(out, text ? text : "");
    free(text);
    return rst == AEGIS_OK ? AEGIS_OK : AEGIS_ERR_NOMEM;
}

/* ── Public API ───────────────────────────────────────────────────────── */

aegis_status_t aegis_mcp_client_create(const char* server_cmd, const char* const* server_argv,
                                       aegis_mcp_client_t** out)
{
    if (server_cmd == NULL || out == NULL) {
        return AEGIS_ERR_INVALID;
    }
    *out                  = NULL;
    aegis_mcp_client_t* c = calloc(1, sizeof(*c));
    if (c == NULL) {
        return AEGIS_ERR_NOMEM;
    }
    aegis_status_t st = aegis_mcp_stdio_spawn(server_cmd, server_argv, &c->stdio);
    if (st != AEGIS_OK) {
        free(c);
        return st;
    }
    *out = c;
    return AEGIS_OK;
}

void aegis_mcp_client_destroy(aegis_mcp_client_t* c)
{
    if (c == NULL) {
        return;
    }
    if (c->tools != NULL) {
        /* Free owned per-tool blob state. input_schema borrows from
         * c->schema_dom (freed below), so it is not freed here. */
        for (size_t i = 0; i < c->tool_count; ++i) {
            mcp_tool_blob_t* blob = c->blobs[i];
            if (blob != NULL) {
                free(blob->params);
                free(blob);
            }
        }
        free(c->blobs);
        free(c->tools);
    }
    aegis_json_value_destroy(c->schema_dom);
    aegis_mcp_stdio_destroy(c->stdio);
    free(c);
}

aegis_status_t aegis_mcp_list_tools(aegis_mcp_client_t* c, size_t* out_count,
                                    aegis_mcp_tool_info_t** out_tools)
{
    if (c == NULL || out_tools == NULL) {
        return AEGIS_ERR_INVALID;
    }
    aegis_status_t st = fetch_tools(c);
    if (st != AEGIS_OK) {
        return st;
    }
    if (out_count != NULL) {
        *out_count = c->tool_count;
    }
    *out_tools = c->tools;
    return AEGIS_OK;
}

aegis_status_t aegis_mcp_call_tool(aegis_mcp_client_t* c, const char* name,
                                   const aegis_tool_args_t* args, aegis_tool_result_t* out)
{
    if (c == NULL || name == NULL || out == NULL) {
        return AEGIS_ERR_INVALID;
    }
    aegis_json_value_t* params = NULL;
    aegis_status_t      st     = aegis_json_new_object(&params, false);
    if (st != AEGIS_OK) {
        return st;
    }
    /* params = { "name": <tool name>, "arguments": <obj> } */
    aegis_json_value_t* name_key = NULL;
    st                           = aegis_json_new_string("name", &name_key);
    if (st == AEGIS_OK) {
        aegis_json_value_t* name_val = NULL;
        st                           = aegis_json_new_string(name, &name_val);
        if (st == AEGIS_OK) {
            st = aegis_json_object_insert(params, name_key, name_val);
        }
        aegis_json_value_destroy(name_val);
    }
    aegis_json_value_destroy(name_key);
    if (st != AEGIS_OK) {
        aegis_json_value_destroy(params);
        return st;
    }
    /* Build the arguments object from the tool's registered blob (param specs);
     * fall back to an empty object when the tool is not yet registered. */
    mcp_tool_blob_t*    blob      = find_blob_by_name(c, name);
    aegis_json_value_t* arguments = args_to_json_object(blob, args);
    if (arguments == NULL) {
        aegis_json_value_destroy(params);
        return AEGIS_ERR_NOMEM;
    }
    aegis_json_value_t* args_key = NULL;
    st                           = aegis_json_new_string("arguments", &args_key);
    if (st != AEGIS_OK) {
        aegis_json_value_destroy(arguments);
        aegis_json_value_destroy(params);
        return st;
    }
    st = aegis_json_object_insert(params, args_key, arguments);
    aegis_json_value_destroy(args_key);
    aegis_json_value_destroy(arguments);
    if (st != AEGIS_OK) {
        aegis_json_value_destroy(params);
        return st;
    }

    aegis_json_value_t* result = NULL;
    st = aegis_mcp_stdio_request(c->stdio, /*id*/ 2, "tools/call", params, &result);
    aegis_json_value_destroy(params);
    if (st != AEGIS_OK) {
        return st;
    }
    /* Mirror mcp_tool_execute content-text concatenation. */
    const aegis_json_value_t* content  = aegis_json_object_get(result, "content");
    char*                     text     = NULL;
    size_t                    text_len = 0;
    if (content != NULL && content->type == AEGIS_JSON_ARRAY) {
        for (size_t i = 0; i < aegis_json_array_len(content); ++i) {
            const aegis_json_value_t* blk = aegis_json_array_at(content, i);
            const char*               txt = NULL;
            if (blk != NULL) {
                const aegis_json_value_t* t = aegis_json_object_get(blk, "text");
                txt                         = aegis_json_string(t);
            }
            if (txt == NULL) {
                continue;
            }
            size_t nl = strlen(txt);
            char*  nt = realloc(text, text_len + nl + 1);
            if (nt == NULL) {
                free(text);
                aegis_json_value_destroy(result);
                return AEGIS_ERR_NOMEM;
            }
            text = nt;
            memcpy(text + text_len, txt, nl + 1);
            text_len = text_len + nl;
        }
    }
    aegis_json_value_destroy(result);
    aegis_status_t rst = aegis_tool_result_set_string(out, text ? text : "");
    free(text);
    return rst == AEGIS_OK ? AEGIS_OK : AEGIS_ERR_NOMEM;
}

aegis_status_t aegis_mcp_register_tools(aegis_mcp_client_t* c, aegis_tool_registry_t* reg)
{
    if (c == NULL || reg == NULL) {
        return AEGIS_ERR_INVALID;
    }
    aegis_status_t st = fetch_tools(c);
    if (st != AEGIS_OK) {
        return st;
    }
    for (size_t i = 0; i < c->tool_count; ++i) {
        aegis_mcp_tool_info_t* info = &c->tools[i];
        /* Attach a per-tool owned blob so the execute trampoline can reach
         * the client + param specs + its own tool index. */
        mcp_tool_blob_t* blob = calloc(1, sizeof(*blob));
        if (blob == NULL) {
            return AEGIS_ERR_NOMEM;
        }
        blob->client = c;
        blob->index  = (int)i;
        blob->params = build_param_specs(info->input_schema, &blob->param_count);

        aegis_tool_def_t def   = {0};
        def.name               = info->name; /* owned by the client */
        def.description        = info->description;
        def.schema.params      = blob->params;
        def.schema.param_count = blob->param_count;
        def.capabilities       = 0;
        def.execute            = mcp_tool_execute;
        def.cancel             = NULL;
        def.user               = blob;

        st = aegis_tool_registry_register(reg, &def);
        if (st != AEGIS_OK) {
            free(blob->params);
            free(blob);
            return st;
        }
        c->blobs[i] = blob;
    }
    return AEGIS_OK;
}
