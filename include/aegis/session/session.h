#ifndef AEGIS_SESSION_H
#define AEGIS_SESSION_H

#include "aegis/message/message.h"
#include "aegis/model/model.h"
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

/**
 * @brief Create a new session with a fresh UUID id/branch and an empty
 *        message history. Optionally associates @p project_root (may be
 *        NULL). Caller owns the session and must destroy it.
 */
aegis_status_t aegis_session_create(const char* project_root, aegis_session_t** out);
/**
 * @brief Free a session and all messages it owns. NULL is a no-op.
 */
void aegis_session_destroy(aegis_session_t* sess);

/* ── Identity ────────────────────────────────────────────────────────── */

/**
 * Borrow the session UUID (36-char canonical form). Valid until
 * destruction.
 */
const char* aegis_session_id(const aegis_session_t* sess);
/**
 * Return the wall-clock creation timestamp in milliseconds since epoch.
 * Refreshed on append and compact.
 */
uint64_t aegis_session_created_at(const aegis_session_t* sess);
/**
 * Return the last-update timestamp (ms since epoch). Refreshed on every
 * message append and compaction.
 */
uint64_t aegis_session_updated_at(const aegis_session_t* sess);

/* ── Messages ────────────────────────────────────────────────────────── */

/**
 * Append a deep-copy of @p msg into the session history. The caller
 * retains ownership of @p msg; the session maintains its own copy.
 * Returns AEGIS_ERR_INVALID for NULL sess/msg.
 */
aegis_status_t aegis_session_append_message(aegis_session_t* sess, const aegis_message_t* msg);
/**
 * Return the total number of messages in the history. NULL-safe.
 */
size_t aegis_session_message_count(const aegis_session_t* sess);
/**
 * Borrow the message at zero-based index @p idx. Returns NULL when
 * index is out of range or sess is NULL. Valid until the next mutation.
 */
const aegis_message_t* aegis_session_message_at(const aegis_session_t* sess, size_t idx);
/**
 * Borrow the owned message list. Valid until destruction or compaction.
 */
const aegis_message_list_t* aegis_session_messages(const aegis_session_t* sess);

/**
 * @brief Drop the oldest messages while retaining the most recent
 *        conversation, adjusting the cut so tool-call pairs are never
 *        split. Requesting >= current count is a no-op success.
 */
aegis_status_t aegis_session_compact(aegis_session_t* sess, size_t keep_messages);
/**
 * @brief Compact the session, optionally replacing the dropped prefix with a
 *        single LLM-generated summary message.
 *
 * Mirrors aegis_session_compact() exactly (tool-pair-preserving truncation to
 * the newest @p keep messages). When @p model is non-NULL, @p token is not
 * cancelled, and there are dropped messages, a one-shot aegis_model_complete()
 * call summarizes the dropped prefix; the summary is prepended to the retained
 * list as a single assistant message. On any model failure, NULL model, or
 * pre-cancelled token the function degrades to plain truncation.
 *
 * @param[in]  s           Session (non-NULL).
 * @param[in]  keep        Number of newest messages to retain.
 * @param[in]  model       Model client used for summarization; NULL disables it.
 * @param[in]  token       Cancellation token; pre-cancelled skips summarization.
 * @param[out] out_summary Receives the prepended summary message (owned by the
 *                         session's message list) on success, or NULL when no
 *                         summary was generated. Initialized to NULL on entry.
 * @return AEGIS_OK on success (including the truncation fallback),
 *   AEGIS_ERR_INVALID on NULL @p s, AEGIS_ERR_NOMEM on allocation failure.
 */
aegis_status_t aegis_session_compact_with_summary(aegis_session_t* s, size_t keep,
                                                  aegis_model_client_t*             model,
                                                  const aegis_cancellation_token_t* token,
                                                  aegis_message_t**                 out_summary);

/* ── Persistence (JSONL) ─────────────────────────────────────────────── */

/**
 * @brief Persist the session as append-only JSONL (one object per line)
 *        to @p path via tmp+rename for atomicity. Returns AEGIS_ERR_INVALID
 *        for NULL sess/path; AEGIS_ERR_NOT_FOUND is never returned (the
 *        file is always created).
 */
aegis_status_t aegis_session_save(const aegis_session_t* sess, const char* path);
/**
 * @brief Load a session previously written by aegis_session_save().
 *        Returns AEGIS_ERR_NOT_FOUND when the file does not exist;
 *        AEGIS_ERR_INVALID for NULL path/out or corrupt header.
 */
aegis_status_t aegis_session_load(const char* path, aegis_session_t** out);

/* ── Branch / Fork ───────────────────────────────────────────────────── */

/**
 * @brief Deep-copy the source session's history into a new session that
 *        shares the same project root but gets a fresh UUID branch_id.
 *        The parent link is recorded so fork chains can be followed.
 */
aegis_status_t aegis_session_fork(const aegis_session_t* src, aegis_session_t** out);
/**
 * Borrow the branch UUID (fresh at fork time; empty-string-root for
 * root sessions). Valid until destruction.
 */
const char* aegis_session_branch_id(const aegis_session_t* sess);
/**
 * Borrow the id of the parent session this one was forked from, or NULL
 * when there is no parent (root session) or sess is NULL.
 */
const char* aegis_session_parent_id(const aegis_session_t* sess);

#ifdef __cplusplus
}
#endif

#endif /* AEGIS_SESSION_H */
