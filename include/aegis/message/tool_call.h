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

/**
 * @brief Allocate an empty tool-call record. All fields default to NULL
 *        (except index which defaults to -1). Caller owns the result
 *        and must call aegis_tool_call_destroy.
 */
aegis_status_t aegis_tool_call_create(aegis_tool_call_t** out);
/**
 * @brief Free a tool-call record and every owned string it holds.
 *        NULL is a no-op.
 */
void aegis_tool_call_destroy(aegis_tool_call_t* call);
/**
 * @brief Deep-copy a tool call, allocating fresh strings for id, name
 *        and JSON arguments. Caller owns the clone and must destroy it.
 */
aegis_status_t aegis_tool_call_clone(const aegis_tool_call_t* src, aegis_tool_call_t** out);

/* ── Accessors ────────────────────────────────────────────────────────── */

/** Borrow the call-id string (NULL when unset or call is NULL). */
const char* aegis_tool_call_id(const aegis_tool_call_t* call);
/** Borrow the tool-name string (NULL when unset or call is NULL). */
const char* aegis_tool_call_name(const aegis_tool_call_t* call);
/**
 * Borrow the JSON-arguments string (NULL when unset or call is NULL).
 * Caller must not modify the returned buffer.
 */
const char* aegis_tool_call_arguments(const aegis_tool_call_t* call);
/**
 * Return the positional index of this call (-1 when unset or call is
 * NULL — used by the tool-result message to match back to the caller).
 */
int aegis_tool_call_index(const aegis_tool_call_t* call);

/* ── Mutators ─────────────────────────────────────────────────────────── */

/**
 * Replace the owned call-id with a copy of @p id (or clear with NULL).
 * Returns AEGIS_ERR_INVALID for NULL call; AEGIS_ERR_NOMEM on allocation
 * failure.
 */
aegis_status_t aegis_tool_call_set_id(aegis_tool_call_t* call, const char* id);
/**
 * Replace the owned tool-name with a copy of @p name (or clear with
 * NULL). Returns AEGIS_ERR_INVALID for NULL call.
 */
aegis_status_t aegis_tool_call_set_name(aegis_tool_call_t* call, const char* name);
/**
 * Replace the owned JSON-arguments string with a copy of @p json (or
 * clear with NULL). Returns AEGIS_ERR_INVALID for NULL call.
 */
aegis_status_t aegis_tool_call_set_arguments(aegis_tool_call_t* call, const char* json);
/**
 * Set the positional index. No allocation; safe on NULL call.
 */
aegis_status_t aegis_tool_call_set_index(aegis_tool_call_t* call, int index);

#ifdef __cplusplus
}
#endif

#endif /* AEGIS_MESSAGE_TOOL_CALL_H */
