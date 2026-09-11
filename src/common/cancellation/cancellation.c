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

/**
 * @brief Create a fresh, un-cancelled token.
 *
 * @param[out] out Receives the handle (ownership: transferred).
 * @return AEGIS_OK on success, AEGIS_ERR_INVALID on NULL out, AEGIS_ERR_NOMEM on failure.
 */
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

/**
 * @brief Request cancellation with the user-origin flag.
 *
 * Convenience wrapper over aegis_cancel_request(token, AEGIS_CANCEL_USER).
 *
 * @param token Token to cancel (borrowed).
 */
void aegis_cancellation_token_request_cancel(aegis_cancellation_token_t* token)
{
    aegis_cancel_request(token, AEGIS_CANCEL_USER);
}

/**
 * @brief Destroy a token (plain free; flags need no teardown).
 *
 * Safe to call with NULL (no-op via free).
 *
 * @param token Token to destroy (ownership: consumed).
 */
void aegis_cancellation_token_destroy(aegis_cancellation_token_t* token)
{
    free(token);
}

/**
 * @brief Atomically OR @p flag into the token (lock-free, SEQ_CST).
 *
 * Flags accumulate monotonically — cancellation is never revoked.
 * No-op for NULL tokens.
 *
 * @param token Token to update (borrowed).
 * @param flag  Flag bits to set.
 */
void aegis_cancel_request(aegis_cancellation_token_t* token, int32_t flag)
{
    if (!token) {
        return;
    }
    __atomic_fetch_or(&token->cancel_flags, flag, __ATOMIC_SEQ_CST);
}

/**
 * @brief Atomically snapshot the current flag bits (lock-free, SEQ_CST).
 *
 * @param token Token to read (borrowed).
 * @return Flag bits, or AEGIS_CANCEL_NONE for NULL input.
 */
int32_t aegis_cancel_flags(const aegis_cancellation_token_t* token)
{
    if (!token) {
        return AEGIS_CANCEL_NONE;
    }
    return __atomic_load_n(&token->cancel_flags, __ATOMIC_SEQ_CST);
}

/**
 * @brief Poll whether cancellation was requested or the deadline passed.
 *
 * Lock-free: flag check is atomic and the deadline is an immutable int64,
 * so this stays cheap inside tight work loops. A NULL token is never cancelled.
 *
 * @param token Token to poll (borrowed).
 * @return true when any flag is set or a non-zero deadline has passed.
 */
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
