/**
 * @file cancellation_internal.h
 * @brief Internal layout and mutation API for cancellation tokens.
 *
 * NOT part of the public API. Included by executor.c only.
 *
 * Memory model:
 *   - cancel_flags is atomic; writers may be any thread, readers any
 *     thread (SEQ_CST via __atomic builtins).
 *   - deadline_ns is written by the owning worker BEFORE the token is
 *     handed to the work function and never mutated afterwards for
 *     that attempt; the queue/executor mutex establishes the
 *     happens-before edge, so a plain read is race-free.
 */
#ifndef AEGIS_CANCELLATION_INTERNAL_H
#define AEGIS_CANCELLATION_INTERNAL_H

#include "aegis/common/cancellation/cancellation.h"
#include <stdint.h>

/** Cancellation reason flags. */
enum {
    AEGIS_CANCEL_NONE     = 0,
    AEGIS_CANCEL_USER     = 1, /**< aegis_executor_cancel(). */
    AEGIS_CANCEL_SHUTDOWN = 2, /**< executor shutdown / destroy. */
};

struct aegis_cancellation_token {
    int32_t cancel_flags; /**< AEGIS_CANCEL_* bits (atomic access). */
    int64_t deadline_ns;  /**< Monotonic deadline, 0 = none. Immutable per attempt. */
};

/**
 * @brief Request cancellation with the given reason flag.
 *
 * Idempotent; flags accumulate. Lock-free.
 */
void aegis_cancel_request(aegis_cancellation_token_t* token, int32_t flag);

/**
 * @brief Read the current reason flags (never includes expiry).
 */
int32_t aegis_cancel_flags(const aegis_cancellation_token_t* token);

#endif /* AEGIS_CANCELLATION_INTERNAL_H */
