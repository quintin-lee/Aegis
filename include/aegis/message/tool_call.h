#ifndef AEGIS_MESSAGE_TOOL_CALL_H
#define AEGIS_MESSAGE_TOOL_CALL_H

#include "aegis/types.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file tool_call.h
 * @brief First-class Tool Call model.
 */

typedef struct aegis_tool_call aegis_tool_call_t;

/* ── Lifecycle ────────────────────────────────────────────────────────── */

aegis_status_t aegis_tool_call_create(aegis_tool_call_t** out);
void           aegis_tool_call_destroy(aegis_tool_call_t* call);
aegis_status_t aegis_tool_call_clone(const aegis_tool_call_t* src, aegis_tool_call_t** out);

/* ── Accessors ────────────────────────────────────────────────────────── */

const char* aegis_tool_call_id(const aegis_tool_call_t* call);
const char* aegis_tool_call_name(const aegis_tool_call_t* call);
const char* aegis_tool_call_arguments(const aegis_tool_call_t* call);
int         aegis_tool_call_index(const aegis_tool_call_t* call);

/* ── Mutators ─────────────────────────────────────────────────────────── */

aegis_status_t aegis_tool_call_set_id(aegis_tool_call_t* call, const char* id);
aegis_status_t aegis_tool_call_set_name(aegis_tool_call_t* call, const char* name);
aegis_status_t aegis_tool_call_set_arguments(aegis_tool_call_t* call, const char* json);
aegis_status_t aegis_tool_call_set_index(aegis_tool_call_t* call, int index);

#ifdef __cplusplus
}
#endif

#endif /* AEGIS_MESSAGE_TOOL_CALL_H */
