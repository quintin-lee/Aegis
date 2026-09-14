/**
 * @file model.c
 * @brief Model client lifecycle plus complete/stream dispatch: backend
 * callbacks when installed, deterministic mock fallback otherwise
 * (canned text derived from the last user message). Backend struct is
 * copied at creation; destroy and capabilities are NULL-safe.
 */
#define _POSIX_C_SOURCE 200809L
#include "aegis/model/model.h"
#include "aegis/message/message.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

struct aegis_model_client {
    char*                    model;
    aegis_model_capability_t caps;
    aegis_model_backend_t    backend;
};

/**
 * @brief Create a model client that uses the built-in mock backend.
 *
 * The client is configured with the full capability set (text, tool
 * calling, streaming) and no backend callbacks, so complete/stream calls
 * fall back to the deterministic mock response derived from the last
 * user message.
 *
 * @param[in]  model Model name; must be non-NULL.
 * @param[out] out   Receives the new client; untouched on failure.
 * @return AEGIS_OK on success, AEGIS_ERR_INVALID for NULL args,
 *   AEGIS_ERR_NOMEM on allocation failure.
 */
aegis_status_t aegis_model_client_create(const char* model, aegis_model_client_t** out)
{
    if (!model || !out) {
        return AEGIS_ERR_INVALID;
    }
    aegis_model_client_t* c = (aegis_model_client_t*)calloc(1, sizeof(*c));
    if (!c) {
        return AEGIS_ERR_NOMEM;
    }
    c->model = strdup(model);
    if (!c->model) {
        free(c);
        return AEGIS_ERR_NOMEM;
    }
    c->caps = AEGIS_MODEL_CAP_TEXT | AEGIS_MODEL_CAP_TOOL_CALLING | AEGIS_MODEL_CAP_STREAMING;
    *out    = c;
    return AEGIS_OK;
}

/**
 * @brief Create a model client with an explicit backend dispatch table.
 *
 * The backend struct is copied at creation; the caller's struct may be
 * freed afterwards. At least one of @c complete or @c stream must be
 * non-NULL, otherwise the client has no way to respond and creation is
 * rejected. The capability mask is taken from the backend.
 *
 * @param[in]  model   Model name; must be non-NULL.
 * @param[in]  backend Backend dispatch table; must have at least one hook.
 * @param[out] out     Receives the new client; untouched on failure.
 * @return AEGIS_OK on success, AEGIS_ERR_INVALID for NULL args or an
 *   empty backend, AEGIS_ERR_NOMEM on allocation failure.
 */
aegis_status_t aegis_model_client_create_with_backend(const char*                  model,
                                                      const aegis_model_backend_t* backend,
                                                      aegis_model_client_t**       out)
{
    if (!model || !backend || !out || (!backend->complete && !backend->stream)) {
        return AEGIS_ERR_INVALID;
    }
    aegis_model_client_t* c  = NULL;
    aegis_status_t        st = aegis_model_client_create(model, &c);
    if (st != AEGIS_OK) {
        return st;
    }
    c->backend = *backend;

    c->caps = backend->capabilities;
    *out    = c;
    return AEGIS_OK;
}

/**
 * @brief Destroy a model client and free its owned resources.
 *
 * Frees the model name string and the client struct. NULL is a no-op.
 *
 * @param[in] c Client to destroy, or NULL.
 */
void aegis_model_client_destroy(aegis_model_client_t* c)
{
    if (!c) {
        return;
    }
    free(c->model);
    free(c);
}

/**
 * @brief Return the capability mask advertised by a model client.
 *
 * @param[in] c Client to query, or NULL (returns 0).
 * @return Capability bitmask; 0 when the client is NULL.
 */
aegis_model_capability_t aegis_model_capabilities(const aegis_model_client_t* c)
{
    return c ? c->caps : 0;
}

/* Compat helper retained for future provider dispatch. */
static char* build_prompt_from_messages(const aegis_message_list_t* msgs)
{
    if (!msgs) {
        return strdup("");
    }
    size_t total = 0;
    size_t n     = aegis_message_list_count(msgs);
    for (size_t i = 0; i < n; i++) {
        const aegis_message_t* m       = aegis_message_list_at(msgs, i);
        const char*            content = aegis_message_content(m);
        if (content) {
            total += strlen(content) + 16;
        }
    }
    char* out = (char*)malloc(total + 1);
    if (!out) {
        return NULL;
    }
    out[0] = '\0';
    for (size_t i = 0; i < n; i++) {
        const aegis_message_t* m       = aegis_message_list_at(msgs, i);
        const char*            role    = aegis_message_role_str(aegis_message_role(m));
        const char*            content = aegis_message_content(m);
        if (!content) {
            content = "";
        }
        size_t used    = strlen(out);
        int    written = snprintf(out + used, total + 1 - used, "[%s] %s\n", role, content);
        if (written < 0 || (size_t)written >= total + 1 - used) {
            free(out);
            return NULL;
        }
    }
    return out;
}

/**
 * @brief Synchronous model completion: build the prompt from @p req->messages,
 *        call the backend (or mock fallback), and return the response.
 *
 * When no backend.complete callback is installed a deterministic mock is
 * used: the response content is "mock response to: <last_user_message>".
 * Checks @p token for cancellation before any work starts. The caller
 * owns the response and must destroy it with aegis_model_response_destroy.
 *
 * @param[in]  client Model client (must be non-NULL).
 * @param[in]  req    Request carrying the message list (must be non-NULL).
 * @param[in]  token  Cancellation point, or NULL to ignore.
 * @param[out] out    Receives the response; set only on success.
 * @return AEGIS_OK on success, AEGIS_ERR_INVALID for NULL args,
 *   AEGIS_ERR_CANCELLED when @p token is already tripped, else the backend
 *   or mock error.
 */
aegis_status_t aegis_model_complete(aegis_model_client_t* client, const aegis_model_request_t* req,
                                    const aegis_cancellation_token_t* token,
                                    aegis_model_response_t**          out)
{
    if (!client || !req || !out) {
        return AEGIS_ERR_INVALID;
    }
    *out = NULL;
    if (token && aegis_cancellation_token_is_cancelled(token)) {
        return AEGIS_ERR_CANCELLED;
    }
    if (client->backend.complete) {
        return client->backend.complete(client->backend.user, req, token, out);
    }
    (void)build_prompt_from_messages;

    aegis_model_response_t* resp = NULL;
    aegis_status_t          st   = aegis_model_response_create(&resp);
    if (st != AEGIS_OK) {
        return st;
    }

    aegis_message_t* msg = NULL;
    st                   = aegis_message_create(AEGIS_MESSAGE_ASSISTANT, &msg);
    if (st != AEGIS_OK) {
        aegis_model_response_destroy(resp);
        return st;
    }
    const char* last_content = "";
    if (req->messages) {
        size_t n = aegis_message_list_count(req->messages);
        for (size_t i = n; i > 0; i--) {
            const aegis_message_t* m = aegis_message_list_at(req->messages, i - 1);
            if (aegis_message_role(m) == AEGIS_MESSAGE_USER && aegis_message_content(m)) {
                last_content = aegis_message_content(m);
                break;
            }
        }
    }
    char buf[512];
    int  n = snprintf(buf, sizeof(buf), "mock response to: %s", last_content);
    if (n < 0 || (size_t)n >= sizeof(buf)) {
        aegis_message_destroy(msg);
        aegis_model_response_destroy(resp);
        return AEGIS_ERR_NOMEM;
    }
    st = aegis_message_set_content(msg, buf);
    if (st != AEGIS_OK) {
        aegis_message_destroy(msg);
        aegis_model_response_destroy(resp);
        return st;
    }
    resp->message = msg;
    *out          = resp;
    return AEGIS_OK;
}

/**
 * @brief Streaming model completion: drives the backend stream callback
 *        (or a mock echo of the last user message), checking cancellation
 *        at each tick.
 *
 * The callback is invoked with AEGIS_MODEL_STREAM_TEXT_DELTA events for
 * each chunk and AEGIS_MODEL_STREAM_END on success, or
 * AEGIS_MODEL_STREAM_ERROR on failure. The callback owns its output; the
 * caller only passes a pointer through @p user. Cancellation aborts the
 * stream immediately.
 *
 * @param[in]  client Model client (must be non-NULL).
 * @param[in]  req    Request carrying the message list (must be non-NULL).
 * @param[in]  token  Cancellation point, or NULL to ignore.
 * @param[in]  cb     Stream callback invoked for each event (must be non-NULL).
 * @param[in]  user   Opaque pointer forwarded to @p cb.
 * @return AEGIS_OK when the stream completes, AEGIS_ERR_INVALID for NULL
 *   args, AEGIS_ERR_CANCELLED when @p token is already tripped.
 */
aegis_status_t aegis_model_stream(aegis_model_client_t* client, const aegis_model_request_t* req,
                                  const aegis_cancellation_token_t* token,
                                  aegis_model_stream_callback_fn cb, void* user)
{
    if (!client || !req || !cb) {
        return AEGIS_ERR_INVALID;
    }
    if (token && aegis_cancellation_token_is_cancelled(token)) {
        return AEGIS_ERR_CANCELLED;
    }
    if (client->backend.stream) {
        return client->backend.stream(client->backend.user, req, token, cb, user);
    }

    const char* last = "";
    if (req->messages) {
        size_t n = aegis_message_list_count(req->messages);
        for (size_t i = n; i > 0; i--) {
            const aegis_message_t* m = aegis_message_list_at(req->messages, i - 1);
            if (aegis_message_role(m) == AEGIS_MESSAGE_USER && aegis_message_content(m)) {
                last = aegis_message_content(m);
                break;
            }
        }
    }
    char full[512];
    int  n = snprintf(full, sizeof(full), "mock stream for: %s", last);
    if (n < 0 || (size_t)n >= sizeof(full)) {
        return AEGIS_ERR_NOMEM;
    }
    size_t len   = (size_t)n;
    size_t chunk = 8;
    /* Reasoning chunks before the answer, mirroring reasoning-model flow. */
    static const char* rparts[] = {"thinking ", "about it..."};
    for (size_t i = 0; i < sizeof(rparts) / sizeof(rparts[0]); i++) {
        if (token && aegis_cancellation_token_is_cancelled(token)) {
            return AEGIS_ERR_CANCELLED;
        }
        aegis_model_stream_event_t ev = {
            .type  = AEGIS_MODEL_STREAM_REASONING_DELTA,
            .data  = rparts[i],
            .len   = strlen(rparts[i]),
            .index = 0,
        };
        aegis_status_t st = cb(&ev, user);
        if (st != AEGIS_OK) {
            return st;
        }
    }
    for (size_t off = 0; off < len; off += chunk) {
        if (token && aegis_cancellation_token_is_cancelled(token)) {
            return AEGIS_ERR_CANCELLED;
        }
        size_t                     cur = len - off > chunk ? chunk : len - off;
        aegis_model_stream_event_t ev  = {
            .type  = AEGIS_MODEL_STREAM_TEXT_DELTA,
            .data  = full + off,
            .len   = cur,
            .index = 0,
        };
        aegis_status_t st = cb(&ev, user);
        if (st != AEGIS_OK) {
            return st;
        }
    }
    /* Report deterministic usage before END, mirroring real providers. */
    aegis_usage_t usage = {.input_tokens = len, .output_tokens = len, .total_tokens = len * 2};
    aegis_model_stream_event_t uev = {
        .type = AEGIS_MODEL_STREAM_USAGE, .data = &usage, .len = sizeof(usage)};
    cb(&uev, user);
    aegis_model_stream_event_t end = {.type = AEGIS_MODEL_STREAM_END, .data = NULL, .len = 0};
    return cb(&end, user);
}
