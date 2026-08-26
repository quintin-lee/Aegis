/**
 * @file tool_schema.c
 * @brief Schema validation for tool arguments.
 *
 * Validation order (first failure wins):
 *   1. unknown argument names,
 *   2. exact type match (no coercion),
 *   3. required-parameter presence.
 */
#include "internal/tool_internal.h"

#include <string.h>

aegis_status_t aegis_tool_validate_args(const aegis_tool_schema_t* schema,
                                        const aegis_tool_args_t*   args)
{
    static const aegis_tool_schema_t empty_schema = {NULL, 0};
    if (!schema) {
        schema = &empty_schema;
    }
    const size_t arg_count = aegis_tool_args_count(args);

    /* 1+2: every supplied argument must be declared with the same type. */
    for (size_t i = 0; i < arg_count; i++) {
        const char* name     = args->items[i].name;
        bool        declared = false;
        for (size_t p = 0; p < schema->param_count; p++) {
            const aegis_tool_param_spec_t* spec = &schema->params[p];
            if (!spec->name || strcmp(spec->name, name) != 0) {
                continue;
            }
            declared = true;
            if (args->items[i].value.type != spec->type) {
                return AEGIS_ERR_INVALID;
            }
            break;
        }
        if (!declared) {
            return AEGIS_ERR_INVALID;
        }
    }

    /* 3: every required parameter must be present. */
    for (size_t p = 0; p < schema->param_count; p++) {
        const aegis_tool_param_spec_t* spec = &schema->params[p];
        if (!spec->required || !spec->name) {
            continue;
        }
        const aegis_tool_value_t* found = NULL;
        if (!aegis_tool_args_find(args, spec->name, &found)) {
            return AEGIS_ERR_INVALID;
        }
    }

    return AEGIS_OK;
}
