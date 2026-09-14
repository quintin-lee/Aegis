/**
 * @file llm_shared.h
 * @brief Provider-agnostic HTTP/JSON infrastructure shared by the LLM
 * provider backends (OpenAI, Anthropic, ...).
 *
 * Exports three groups:
 *  1. JSON buffer builder (checked_grow / append_raw / append_json_string)
 *  2. curl SSE pending-reassembly skeleton (aegis_sse_state + aegis_sse_on_write)
 *  3. curl progress-callback cancellation cooperation (aegis_sse_progress)
 *
 * Each provider keeps its own wire-format parsing (body building, SSE record
 * interpretation, response decoding) in its own .c file; this header only
 * supplies the transport/serialization plumbing that does not depend on
 * protocol details.
 */
#ifndef AEGIS_LLM_SHARED_H
#define AEGIS_LLM_SHARED_H

#include "aegis/model/model.h"
#include <curl/curl.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── JSON buffer builder ───────────────────────────────────────────────── */

/**
 * Growable JSON body accumulator. Use @ref aegis_json_builder_init, append
 * via the aegis_json_* helpers, then free b->data with free().
 */
typedef struct aegis_json_builder {
    char*  data;
    size_t len;
    size_t cap;
} aegis_json_builder_t;

/** Initialize a fresh builder (zeroed). */
void aegis_json_builder_init(aegis_json_builder_t* b);

/** Free the builder's internal buffer (leaves fields untouched). */
void aegis_json_builder_free(aegis_json_builder_t* b);

/** Append raw bytes. @return 1 on success, 0 on allocation failure. */
int aegis_json_append_raw(aegis_json_builder_t* b, const char* s, size_t n);

/**
 * Append an escaped JSON string (with surrounding quotes) to @p b.
 * @p s may be NULL (encodes as "\"\""). @return 1 on success, 0 on failure.
 */
int aegis_json_append_string(aegis_json_builder_t* b, const char* s);

/* ── curl cancel-cooperation progress callback ─────────────────────────── */

/**
 * Curl XFERINFOFUNCTION that aborts the transfer when @p token is cancelled.
 * Register via CURLOPT_XFERINFOFUNCTION with @p token as WRITEDATA/XFERINFODATA.
 *
 * @param[in] token  Borrowed cancellation token, or NULL to disable.
 * @return The progress-callback function pointer to install.
 */
int aegis_sse_progress(void* token, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal,
                       curl_off_t ulnow);

/* ── SSE pending reassembly skeleton ───────────────────────────────────── */

/**
 * Pending chunk buffer that accumulates curl write callbacks and splits on
 * blank-line (\n\n) record boundaries. Provider code reads completed
 * records from pending + pending_len, advances pending_consumed, and calls
 * @ref aegis_sse_compact to shift the consumed bytes out.
 *
 * Initialize with a zeroed struct before use.
 */
typedef struct {
    char*  pending;
    size_t pending_len;
    size_t pending_cap;
} aegis_sse_state_t;

/** Free the pending buffer (leaves fields untouched). */
void aegis_sse_free(aegis_sse_state_t* s);

/**
 * Curl WRITEFUNCTION: append incoming bytes to the pending buffer and grow
 * it as needed. Does NOT interpret records — the provider decides record
 * semantics. Returns the number of bytes consumed from @p ptr (i.e. total
 * of size*nmemb) on success, 0 to abort the transfer on allocation failure.
 */
size_t aegis_sse_on_write(void* ptr, size_t size, size_t nmemb, void* user);

/**
 * Shift out the already-consumed records from the front of the pending
 * buffer after a provider has processed them. Pass the byte count the
 * provider consumed; the rest of the buffer is memmoved forward.
 */
void aegis_sse_compact(aegis_sse_state_t* s, size_t consumed);

#ifdef __cplusplus
}
#endif

#endif /* AEGIS_LLM_SHARED_H */
