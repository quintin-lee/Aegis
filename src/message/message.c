/**
 * @file message.c
 * @brief Message + message-list implementation: heap-owned strings,
 * deep-copy clone/append, NULL-tolerant accessors. Internal layout is
 * private; all access goes through the message.h API.
 */
#define _POSIX_C_SOURCE 200809L
#include "aegis/message/message.h"
#include "aegis/common/uuid.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

// Internal layout
struct aegis_message {
    char*                id;
    aegis_message_role_t role;
    uint64_t             timestamp;  // ms since epoch
    char*                content;
    char*                reasoning;
    char*                tool_call_id;  // for TOOL role: which call this result corresponds to
    char*                parent_id;

    // tool_calls attached to assistant message
    aegis_tool_call_t** tool_calls;
    size_t              tool_call_count;
    size_t              tool_call_cap;
};

struct aegis_message_list {
    aegis_message_t** msgs;
    size_t            count;
    size_t            cap;
};

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
static char* gen_id(void)
{
    aegis_uuid_t u = aegis_uuid_generate();
    char         buf[37];
    aegis_uuid_format(&u, buf, sizeof(buf));
    return strdup(buf);
}

/**
 * @brief Allocate a new message with a fresh UUID, current timestamp and
 *        the given role.
 *
 * All string fields start NULL; the id is always allocated. The caller
 * owns the returned message and must destroy it with aegis_message_destroy.
 *
 * @param[in]  role Message role (system/user/assistant/tool/event/summary).
 * @param[out] out  Receives the new message; untouched on failure.
 * @return AEGIS_OK on success, AEGIS_ERR_INVALID for NULL @p out,
 *   AEGIS_ERR_NOMEM on allocation failure.
 */
aegis_status_t aegis_message_create(aegis_message_role_t role, aegis_message_t** out)
{
    if (!out) {
        return AEGIS_ERR_INVALID;
    }
    aegis_message_t* m = (aegis_message_t*)calloc(1, sizeof(*m));
    if (!m) {
        return AEGIS_ERR_NOMEM;
    }
    m->role      = role;
    m->timestamp = now_ms();
    m->id        = gen_id();
    if (!m->id) {
        free(m);
        return AEGIS_ERR_NOMEM;
    }
    *out = m;
    return AEGIS_OK;
}

/**
 * @brief Free a message and all its owned heap strings and tool-calls.
 *
 * NULL is a no-op. Tool-call objects owned by the message are destroyed;
 * the tool_calls array itself is freed but the individual calls are NOT
 * freed (they are refcounted elsewhere via their own destroy path).
 *
 * @param[in] m Message to destroy, or NULL.
 */
void aegis_message_destroy(aegis_message_t* m)
{
    if (!m) {
        return;
    }
    free(m->id);
    free(m->content);
    free(m->reasoning);
    free(m->tool_call_id);
    free(m->parent_id);
    for (size_t i = 0; i < m->tool_call_count; i++) {
        aegis_tool_call_destroy(m->tool_calls[i]);
    }
    free(m->tool_calls);
    free(m);
}

/**
 * @brief Deep-copy a message, allocating a fresh UUID but preserving the
 *        original content, reasoning and parent-id.
 *
 * Tool calls are recursively cloned. The caller owns the resulting copy.
 *
 * @param[in]  src Message to clone (must be non-NULL).
 * @param[out] out Receives the new copy; untouched on failure.
 * @return AEGIS_OK on success, AEGIS_ERR_INVALID for NULL src/out,
 *   AEGIS_ERR_NOMEM on allocation failure.
 */
aegis_status_t aegis_message_clone(const aegis_message_t* src, aegis_message_t** out)
{
    if (!src || !out) {
        return AEGIS_ERR_INVALID;
    }
    aegis_message_t* dst = NULL;
    aegis_status_t   st  = aegis_message_create(src->role, &dst);
    if (st != AEGIS_OK) {
        return st;
    }
    free(dst->id);
    dst->id = src->id ? strdup(src->id) : NULL;
    if (src->id && !dst->id) {
        aegis_message_destroy(dst);
        return AEGIS_ERR_NOMEM;
    }
    dst->timestamp = src->timestamp;
    if (src->content) {
        dst->content = strdup(src->content);
        if (!dst->content) {
            aegis_message_destroy(dst);
            return AEGIS_ERR_NOMEM;
        }
    }
    if (src->reasoning) {
        dst->reasoning = strdup(src->reasoning);
        if (!dst->reasoning) {
            aegis_message_destroy(dst);
            return AEGIS_ERR_NOMEM;
        }
    }
    if (src->tool_call_id) {
        dst->tool_call_id = strdup(src->tool_call_id);
        if (!dst->tool_call_id) {
            aegis_message_destroy(dst);
            return AEGIS_ERR_NOMEM;
        }
    }
    if (src->parent_id) {
        dst->parent_id = strdup(src->parent_id);
        if (!dst->parent_id) {
            aegis_message_destroy(dst);
            return AEGIS_ERR_NOMEM;
        }
    }
    for (size_t i = 0; i < src->tool_call_count; i++) {
        aegis_tool_call_t* c = NULL;
        st                   = aegis_tool_call_clone(src->tool_calls[i], &c);
        if (st != AEGIS_OK) {
            aegis_message_destroy(dst);
            return st;
        }
        st = aegis_message_add_tool_call(dst, c);
        aegis_tool_call_destroy(c);  // add clones
        if (st != AEGIS_OK) {
            aegis_message_destroy(dst);
            return st;
        }
    }
    *out = dst;
    return AEGIS_OK;
}

/* ── accessors ────────────────────────────────────────────────────────────── */

/**
 * @brief Borrow the message id string.
 *
 * @param[in] m Message, or NULL.
 * @return Id text, or NULL for NULL message.
 */
const char* aegis_message_id(const aegis_message_t* m)
{
    return m ? m->id : NULL;
}
/**
 * @brief Borrow the message role.
 *
 * Returns AEGIS_MESSAGE_USER when @p m is NULL as a safe default.
 *
 * @param[in] m Message, or NULL.
 * @return Role enum.
 */
aegis_message_role_t aegis_message_role(const aegis_message_t* m)
{
    return m ? m->role : AEGIS_MESSAGE_USER;
}
/**
 * @brief Return the message timestamp in wall-clock milliseconds.
 *
 * @param[in] m Message, or NULL.
 * @return Timestamp, or 0 for NULL.
 */
uint64_t aegis_message_timestamp(const aegis_message_t* m)
{
    return m ? m->timestamp : 0;
}
/**
 * @brief Borrow the message content string.
 *
 * @param[in] m Message, or NULL.
 * @return Content text, or NULL for NULL / empty content.
 */
const char* aegis_message_content(const aegis_message_t* m)
{
    return m ? m->content : NULL;
}
/**
 * @brief Borrow the reasoning (thought-chain) text.
 *
 * Only set on assistant messages that carried a reasoning block. Returns
 * NULL when absent or on NULL input.
 *
 * @param[in] m Message, or NULL.
 * @return Reasoning text, or NULL.
 */
const char* aegis_message_reasoning(const aegis_message_t* m)
{
    return m ? m->reasoning : NULL;
}
/**
 * @brief Borrow the tool-call id associated with a TOOL message.
 *
 * For a message whose role is AEGIS_MESSAGE_TOOL, this is the call_id
 * of the tool invocation this result belongs to. NULL on other roles.
 *
 * @param[in] m Message, or NULL.
 * @return Tool-call id text, or NULL.
 */
const char* aegis_message_tool_call_id(const aegis_message_t* m)
{
    return m ? m->tool_call_id : NULL;
}
/**
 * @brief Borrow the parent message id for chain-of-thought linking.
 *
 * @param[in] m Message, or NULL.
 * @return Parent id text, or NULL when there is no parent.
 */
const char* aegis_message_parent_id(const aegis_message_t* m)
{
    return m ? m->parent_id : NULL;
}
/**
 * @brief Return the number of tool calls attached to this message.
 *
 * @param[in] m Message, or NULL.
 * @return Tool-call count, or 0 for NULL.
 */
size_t aegis_message_tool_call_count(const aegis_message_t* m)
{
    return m ? m->tool_call_count : 0;
}
/**
 * @brief Borrow the tool call at the given index.
 *
 * Out-of-range returns NULL. Valid until the message is mutated or
 * destroyed.
 *
 * @param[in] m  Message to query.
 * @param[in] idx Zero-based index.
 * @return Tool call pointer, or NULL for out-of-range / NULL message.
 */
const aegis_tool_call_t* aegis_message_tool_call_at(const aegis_message_t* m, size_t idx)
{
    if (!m || idx >= m->tool_call_count) {
        return NULL;
    }
    return m->tool_calls[idx];
}

static aegis_status_t set_str(char** dst, const char* src)
{
    char* n = NULL;
    if (src) {
        n = strdup(src);
        if (!n) {
            return AEGIS_ERR_NOMEM;
        }
    }
    free(*dst);
    *dst = n;
    return AEGIS_OK;
}

/**
 * @brief Set the message id, replacing any prior value.
 *
 * Empty string is rejected. Caller supplies the id text which is
 * deep-copied; the caller retains ownership of the source string.
 *
 * @param[in] m  Message to update.
 * @param[in] id New id text.
 * @return AEGIS_OK on success, AEGIS_ERR_INVALID for NULL m/id or empty id,
 *   AEGIS_ERR_NOMEM on allocation failure.
 */
aegis_status_t aegis_message_set_id(aegis_message_t* m, const char* id)
{
    if (!m || !id || !id[0]) {
        return AEGIS_ERR_INVALID;
    }
    return set_str(&m->id, id);
}

/**
 * @brief Set the message content, replacing any prior value.
 *
 * NULL content is accepted and clears the field. Deep-copied.
 *
 * @param[in] m   Message to update.
 * @param[in] txt New content text, or NULL to clear.
 * @return AEGIS_OK on success, AEGIS_ERR_INVALID for NULL m,
 *   AEGIS_ERR_NOMEM on allocation failure.
 */
aegis_status_t aegis_message_set_content(aegis_message_t* m, const char* txt)
{
    if (!m) {
        return AEGIS_ERR_INVALID;
    }
    return set_str(&m->content, txt);
}
/**
 * @brief Set the reasoning (thought-chain) text, replacing any prior value.
 *
 * NULL clears the field. Deep-copied.
 *
 * @param[in] m  Message to update.
 * @param[in] r  New reasoning text, or NULL to clear.
 * @return AEGIS_OK on success, AEGIS_ERR_INVALID for NULL m,
 *   AEGIS_ERR_NOMEM on allocation failure.
 */
aegis_status_t aegis_message_set_reasoning(aegis_message_t* m, const char* r)
{
    if (!m) {
        return AEGIS_ERR_INVALID;
    }
    return set_str(&m->reasoning, r);
}
/**
 * @brief Set the tool-call id for a TOOL-role message.
 *
 * Links this result message back to its invoking tool call. NULL clears.
 * Deep-copied.
 *
 * @param[in] m  Message to update.
 * @param[in] id Tool-call id text, or NULL to clear.
 * @return AEGIS_OK on success, AEGIS_ERR_INVALID for NULL m,
 *   AEGIS_ERR_NOMEM on allocation failure.
 */
aegis_status_t aegis_message_set_tool_call_id(aegis_message_t* m, const char* id)
{
    if (!m) {
        return AEGIS_ERR_INVALID;
    }
    return set_str(&m->tool_call_id, id);
}
/**
 * @brief Set the parent message id for chain-of-thought linking.
 *
 * NULL clears the field. Deep-copied.
 *
 * @param[in] m     Message to update.
 * @param[in] pid   Parent id text, or NULL to clear.
 * @return AEGIS_OK on success, AEGIS_ERR_INVALID for NULL m,
 *   AEGIS_ERR_NOMEM on allocation failure.
 */
aegis_status_t aegis_message_set_parent_id(aegis_message_t* m, const char* pid)
{
    if (!m) {
        return AEGIS_ERR_INVALID;
    }
    return set_str(&m->parent_id, pid);
}

/**
 * @brief Add a cloned tool call to an assistant message.
 *
 * The call is deep-copied (including its nested JSON arguments) and appended
 * to the message's owned list. Returns AEGIS_ERR_NOMEM if the list cannot
 * grow. Not thread-safe.
 *
 * @param[in] m    Assistant message to extend.
 * @param[in] call Tool call to clone and append (owned by caller afterwards).
 * @return AEGIS_OK on success, AEGIS_ERR_INVALID for NULL args,
 *   AEGIS_ERR_NOMEM on allocation failure.
 */
aegis_status_t aegis_message_add_tool_call(aegis_message_t* m, const aegis_tool_call_t* call)
{
    if (!m || !call) {
        return AEGIS_ERR_INVALID;
    }
    if (m->tool_call_count >= m->tool_call_cap) {
        size_t              ncap = m->tool_call_cap ? m->tool_call_cap * 2 : 4;
        aegis_tool_call_t** n    = (aegis_tool_call_t**)realloc(m->tool_calls, ncap * sizeof(*n));
        if (!n) {
            return AEGIS_ERR_NOMEM;
        }
        m->tool_calls    = n;
        m->tool_call_cap = ncap;
    }
    aegis_tool_call_t* c  = NULL;
    aegis_status_t     st = aegis_tool_call_clone(call, &c);
    if (st != AEGIS_OK) {
        return st;
    }
    m->tool_calls[m->tool_call_count++] = c;
    return AEGIS_OK;
}

/* ── list ───────────────────────────────────────────────────────────────── */

/* ── list ───────────────────────────────────────────────────────────────── */

/**
 * @brief Create an empty message list.
 *
 * The caller owns the result and must call aegis_message_list_destroy.
 *
 * @param[out] out Receives the new list; untouched on failure.
 * @return AEGIS_OK on success, AEGIS_ERR_INVALID for NULL @p out,
 *   AEGIS_ERR_NOMEM on allocation failure.
 */
aegis_status_t aegis_message_list_create(aegis_message_list_t** out)
{
    if (!out) {
        return AEGIS_ERR_INVALID;
    }
    aegis_message_list_t* l = (aegis_message_list_t*)calloc(1, sizeof(*l));
    if (!l) {
        return AEGIS_ERR_NOMEM;
    }
    *out = l;
    return AEGIS_OK;
}

/**
 * @brief Destroy a message list and every owned message it holds.
 *
 * Each message is deep-destroyed (freed along with its strings and
 * nested tool calls). NULL is a no-op.
 *
 * @param[in] l List to destroy, or NULL.
 */
void aegis_message_list_destroy(aegis_message_list_t* l)
{
    if (!l) {
        return;
    }
    for (size_t i = 0; i < l->count; i++) {
        aegis_message_destroy(l->msgs[i]);
    }
    free(l->msgs);
    free(l);
}

/**
 * @brief Deep-copy a message list, cloning every contained message.
 *
 * The caller owns the clone and must call aegis_message_list_destroy.
 *
 * @param[in]  src Source list (must be non-NULL).
 * @param[out] out Receives the new copy; untouched on failure.
 * @return AEGIS_OK on success, AEGIS_ERR_INVALID for NULL src/out,
 *   AEGIS_ERR_NOMEM on allocation failure.
 */
aegis_status_t aegis_message_list_clone(const aegis_message_list_t* src, aegis_message_list_t** out)
{
    if (!src || !out) {
        return AEGIS_ERR_INVALID;
    }
    aegis_message_list_t* dst = NULL;
    aegis_status_t        st  = aegis_message_list_create(&dst);
    if (st != AEGIS_OK) {
        return st;
    }
    for (size_t i = 0; i < src->count; i++) {
        st = aegis_message_list_append(dst, src->msgs[i]);
        if (st != AEGIS_OK) {
            aegis_message_list_destroy(dst);
            return st;
        }
    }
    *out = dst;
    return AEGIS_OK;
}

/**
 * @brief Return the number of messages in the list.
 *
 * @param[in] l List, or NULL.
 * @return Message count, or 0 for NULL.
 */
size_t aegis_message_list_count(const aegis_message_list_t* l)
{
    return l ? l->count : 0;
}
/**
 * @brief Borrow the message at a zero-based index.
 *
 * Out-of-range returns NULL. Valid until the list is mutated or destroyed.
 *
 * @param[in] l  List to query.
 * @param[in] idx Zero-based position.
 * @return Message pointer, or NULL for NULL list / out-of-range.
 */
const aegis_message_t* aegis_message_list_at(const aegis_message_list_t* l, size_t idx)
{
    if (!l || idx >= l->count) {
        return NULL;
    }
    return l->msgs[idx];
}

static aegis_status_t list_reserve(aegis_message_list_t* l, size_t need)
{
    if (l->count + need <= l->cap) {
        return AEGIS_OK;
    }
    size_t ncap = l->cap ? l->cap * 2 : 8;
    while (ncap < l->count + need) {
        ncap *= 2;
    }
    aegis_message_t** n = (aegis_message_t**)realloc(l->msgs, ncap * sizeof(*n));
    if (!n) {
        return AEGIS_ERR_NOMEM;
    }
    l->msgs = n;
    l->cap  = ncap;
    return AEGIS_OK;
}

/**
 * @brief Append a cloned message to the end of the list.
 *
 * The message is deep-copied before insertion; the original is untouched.
 * Returns AEGIS_ERR_NOMEM if the list cannot grow.
 *
 * @param[in] l    List to extend (must be non-NULL).
 * @param[in] msg  Message to append (must be non-NULL).
 * @return AEGIS_OK on success, AEGIS_ERR_INVALID for NULL args,
 *   AEGIS_ERR_NOMEM on allocation failure.
 */
aegis_status_t aegis_message_list_append(aegis_message_list_t* l, const aegis_message_t* msg)
{
    if (!l || !msg) {
        return AEGIS_ERR_INVALID;
    }
    aegis_status_t st = list_reserve(l, 1);
    if (st != AEGIS_OK) {
        return st;
    }
    aegis_message_t* c = NULL;
    st                 = aegis_message_clone(msg, &c);
    if (st != AEGIS_OK) {
        return st;
    }
    l->msgs[l->count++] = c;
    return AEGIS_OK;
}

/**
 * @brief Prepend a cloned message to the front of the list.
 *
 * Same ownership rules as append() but inserts at index 0, shifting
 * existing entries forward.
 *
 * @param[in] l    List to extend.
 * @param[in] msg  Message to prepend.
 * @return AEGIS_OK on success, AEGIS_ERR_INVALID for NULL args,
 *   AEGIS_ERR_NOMEM on allocation failure.
 */
aegis_status_t aegis_message_list_prepend(aegis_message_list_t* l, const aegis_message_t* msg)
{
    if (!l || !msg) {
        return AEGIS_ERR_INVALID;
    }
    aegis_status_t st = list_reserve(l, 1);
    if (st != AEGIS_OK) {
        return st;
    }
    aegis_message_t* c = NULL;
    st                 = aegis_message_clone(msg, &c);
    if (st != AEGIS_OK) {
        return st;
    }
    memmove(&l->msgs[1], &l->msgs[0], l->count * sizeof(*l->msgs));
    l->msgs[0] = c;
    l->count++;
    return AEGIS_OK;
}
