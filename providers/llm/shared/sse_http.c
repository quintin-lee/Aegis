/**
 * @file sse_http.c
 * @brief Shared curl SSE pending-reassembly + cancellation-cooperation
 * infrastructure (provider-agnostic transport plumbing).
 *
 * The OpenAI and Anthropic backends each have a different SSE record
 * grammar (OpenAI: bare `data:` lines; Anthropic: `event:` + `data:`
 * pairs), so record interpretation stays in each provider. What is shared
 * here is the byte-accumulation buffer that reassembles chunked curl
 * callbacks into complete `\n\n`-terminated records, and the progress
 * callback that aborts the transfer when a cancellation token fires.
 */
#include "llm_shared.h"
#include "aegis/common/cancellation/cancellation.h"
#include <stdlib.h>
#include <string.h>

void aegis_sse_free(aegis_sse_state_t* s)
{
    if (s) {
        free(s->pending);
    }
}

size_t aegis_sse_on_write(void* ptr, size_t size, size_t nmemb, void* user)
{
    aegis_sse_state_t* s     = user;
    size_t            total  = size * nmemb;
    if (!total || s->pending_len > SIZE_MAX - total - 1) {
        return 0;
    }
    if (s->pending_len + total + 1 > s->pending_cap) {
        size_t cap = s->pending_cap ? s->pending_cap * 2 : 4096;
        while (cap < s->pending_len + total + 1) {
            if (cap > SIZE_MAX / 2) {
                return 0;
            }
            cap *= 2;
        }
        char* p = realloc(s->pending, cap);
        if (!p) {
            return 0;
        }
        s->pending     = p;
        s->pending_cap = cap;
    }
    memcpy(s->pending + s->pending_len, ptr, total);
    s->pending_len += total;
    s->pending[s->pending_len] = '\0';
    return total;
}

void aegis_sse_compact(aegis_sse_state_t* s, size_t consumed)
{
    if (consumed == 0) {
        return;
    }
    /* The caller is responsible for having processed [0, consumed). Shift
     * the remainder forward and reset the tail NUL. */
    if (consumed >= s->pending_len) {
        s->pending_len = 0;
        s->pending[0]  = '\0';
        return;
    }
    memmove(s->pending, s->pending + consumed, s->pending_len - consumed);
    s->pending_len -= consumed;
    s->pending[s->pending_len] = '\0';
}

int aegis_sse_progress(void* user, curl_off_t dltotal, curl_off_t dlnow,
                       curl_off_t ultotal, curl_off_t ulnow)
{
    (void)dltotal;
    (void)dlnow;
    (void)ultotal;
    (void)ulnow;
    const aegis_cancellation_token_t* token = user;
    if (token && aegis_cancellation_token_is_cancelled(token)) {
        return 1; /* non-zero aborts the transfer */
    }
    return 0;
}
