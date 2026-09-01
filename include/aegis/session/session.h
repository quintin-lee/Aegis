#ifndef AEGIS_SESSION_H
#define AEGIS_SESSION_H

#include "aegis/message/message.h"
#include "aegis/types.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file session.h
 * @brief Session — conversation, history, persistence, branch.
 */

typedef struct aegis_session aegis_session_t;

/* ── Lifecycle ───────────────────────────────────────────────────────── */

aegis_status_t aegis_session_create(const char* project_root, aegis_session_t** out);
void           aegis_session_destroy(aegis_session_t* sess);

/* ── Identity ────────────────────────────────────────────────────────── */

const char* aegis_session_id(const aegis_session_t* sess);
uint64_t    aegis_session_created_at(const aegis_session_t* sess);
uint64_t    aegis_session_updated_at(const aegis_session_t* sess);

/* ── Messages ────────────────────────────────────────────────────────── */

aegis_status_t aegis_session_append_message(aegis_session_t* sess, const aegis_message_t* msg);
size_t         aegis_session_message_count(const aegis_session_t* sess);
const aegis_message_t*      aegis_session_message_at(const aegis_session_t* sess, size_t idx);
const aegis_message_list_t* aegis_session_messages(const aegis_session_t* sess);

/** Drop the oldest messages while retaining the most recent conversation. */
aegis_status_t aegis_session_compact(aegis_session_t* sess, size_t keep_messages);

/* ── Persistence (JSONL) ─────────────────────────────────────────────── */

aegis_status_t aegis_session_save(const aegis_session_t* sess, const char* path);
aegis_status_t aegis_session_load(const char* path, aegis_session_t** out);

/* ── Branch / Fork ───────────────────────────────────────────────────── */

aegis_status_t aegis_session_fork(const aegis_session_t* src, aegis_session_t** out);
const char*    aegis_session_branch_id(const aegis_session_t* sess);
const char*    aegis_session_parent_id(const aegis_session_t* sess);

#ifdef __cplusplus
}
#endif

#endif /* AEGIS_SESSION_H */
