#ifndef AEGIS_CANCELLATION_H
#define AEGIS_CANCELLATION_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file cancellation.h
 * @brief Cooperative cancellation token.
 *
 * A token is the cancellation channel handed to executing work. Work
 * polls aegis_cancellation_token_is_cancelled() at safe points and
 * returns promptly once it observes cancellation; the runtime never
 * force-kills threads (see AGENTS.md §14).
 *
 * Two independent sources can trip a token:
 *   - explicit caller cancellation (aegis_executor_cancel);
 *   - executor shutdown, which cancels all in-flight work.
 * Additionally, a job armed with a timeout observes cancellation
 * automatically once its deadline has passed.
 *
 * Thread safety: is_cancelled() is lock-free and may be called from
 * any thread at any frequency.
 */

/** Opaque cooperative-cancellation token. */
typedef struct aegis_cancellation_token aegis_cancellation_token_t;

/**
 * @brief Query whether the token has been cancelled or expired.
 *
 * @param token Token handle (borrowed; NULL reads as "not cancelled"
 *              so optional-token call sites stay branch-free).
 * @return true once cancellation was requested, shutdown tripped the
 *         token, or an armed deadline has passed.
 */
bool aegis_cancellation_token_is_cancelled(const aegis_cancellation_token_t* token);

#ifdef __cplusplus
}
#endif

#endif /* AEGIS_CANCELLATION_H */
