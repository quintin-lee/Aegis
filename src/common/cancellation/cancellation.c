/**
 * @file cancellation.c
 * @brief Cooperative cancellation token implementation.
 *
 * Flags use __atomic builtins so is_cancelled() stays lock-free even
 * when polled at very high frequency inside tight work loops.
 */
#include "cancellation_internal.h"
#include "aegis/common/time.h"

#include <stdlib.h>

aegis_status_t aegis_cancellation_token_create(aegis_cancellation_token_t** out)
{
    if (!out) {
        return AEGIS_ERR_INVALID;
    }
    aegis_cancellation_token_t* tok = calloc(1, sizeof(*tok));
    if (!tok) {
        return AEGIS_ERR_NOMEM;
    }
    *out = tok;
    return AEGIS_OK;
}

void aegis_cancellation_token_request_cancel(aegis_cancellation_token_t* token)
{
    aegis_cancel_request(token, AEGIS_CANCEL_USER);
}

void aegis_cancellation_token_destroy(aegis_cancellation_token_t* token)
{
    free(token);
}

void aegis_cancel_request(aegis_cancellation_token_t* token, int32_t flag)
{
    if (!token) {
        return;
    }
    __atomic_fetch_or(&token->cancel_flags, flag, __ATOMIC_SEQ_CST);
}

int32_t aegis_cancel_flags(const aegis_cancellation_token_t* token)
{
    if (!token) {
        return AEGIS_CANCEL_NONE;
    }
    return __atomic_load_n(&token->cancel_flags, __ATOMIC_SEQ_CST);
}

bool aegis_cancellation_token_is_cancelled(const aegis_cancellation_token_t* token)
{
    if (!token) {
        return false;
    }
    if (__atomic_load_n(&token->cancel_flags, __ATOMIC_SEQ_CST) != AEGIS_CANCEL_NONE) {
        return true;
    }
    const int64_t deadline = token->deadline_ns;
    return deadline != 0 && aegis_mono_now() > deadline;
}
