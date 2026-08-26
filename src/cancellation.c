/**
 * @file cancellation.c
 * @brief Cooperative cancellation token implementation.
 *
 * Flags use __atomic builtins so is_cancelled() stays lock-free even
 * when polled at very high frequency inside tight work loops.
 */
#include "cancellation_internal.h"
#include "aegis/common/time.h"

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
