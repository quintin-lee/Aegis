/**
 * @file memory_mock.c
 * @brief Mock storage backend for unit testing.
 *
 * Stores key/value blobs in a simple linear list. No external
 * dependencies. Suitable for unit tests only.
 */
#define _POSIX_C_SOURCE 200809L
#include "aegis/provider/memory_mock.h"
#include "aegis/status.h"

#include <stdlib.h>
#include <string.h>

struct mock_entry {
    void*  key;
    size_t key_len;
    void*  value;
    size_t value_len;
};

struct mock_ctx {
    struct mock_entry* entries;
    size_t             count;
    size_t             cap;
};

/**
 * @brief Store a blob under a key, replacing any existing entry.
 *
 * Linear scan for a matching key (length + memcmp); a hit frees the old value
 * and stores a fresh copy, a miss appends a key/value copy pair, growing the
 * entry array (16, doubling) as needed.
 *
 * @param[in] ctx       Mock context created by aegis_mock_storage_create().
 * @param[in] key       Key bytes; must be non-NULL.
 * @param[in] key_len   Key length in bytes.
 * @param[in] value     Value bytes; must be non-NULL.
 * @param[in] value_len Value length in bytes.
 * @param[in] token     Unused; present for the storage-ops ABI.
 *
 * @return AEGIS_OK stored; INVALID for NULL ctx/key/value, NOMEM on alloc
 *         failure.
 */
static aegis_status_t mock_put(void* ctx, const void* key, size_t key_len, const void* value,
                               size_t value_len, const aegis_cancellation_token_t* token)
{
    (void)token;
    struct mock_ctx* m = (struct mock_ctx*)ctx;
    if (!m || !key || !value) {
        return AEGIS_ERR_INVALID;
    }
    /* Update existing entry if key matches. */
    for (size_t i = 0; i < m->count; i++) {
        if (m->entries[i].key_len == key_len && memcmp(m->entries[i].key, key, key_len) == 0) {
            free(m->entries[i].value);
            m->entries[i].value = malloc(value_len);
            if (!m->entries[i].value) {
                return AEGIS_ERR_NOMEM;
            }
            memcpy(m->entries[i].value, value, value_len);
            m->entries[i].value_len = value_len;
            return AEGIS_OK;
        }
    }
    /* Append new entry. */
    if (m->count >= m->cap) {
        size_t             new_cap = (m->cap == 0) ? 16 : m->cap * 2;
        struct mock_entry* next    = realloc(m->entries, sizeof(*next) * new_cap);
        if (!next) {
            return AEGIS_ERR_NOMEM;
        }
        m->entries = next;
        m->cap     = new_cap;
    }
    m->entries[m->count].key   = malloc(key_len);
    m->entries[m->count].value = malloc(value_len);
    if (!m->entries[m->count].key || !m->entries[m->count].value) {
        free(m->entries[m->count].key);
        free(m->entries[m->count].value);
        return AEGIS_ERR_NOMEM;
    }
    memcpy(m->entries[m->count].key, key, key_len);
    memcpy(m->entries[m->count].value, value, value_len);
    m->entries[m->count].key_len   = key_len;
    m->entries[m->count].value_len = value_len;
    m->count++;
    return AEGIS_OK;
}

/**
 * @brief Fetch a copy of the blob stored under a key.
 *
 * Linear scan for a matching key; on a hit out receives a freshly allocated
 * copy of the value. out is zeroed up front so failure leaves no garbage.
 *
 * @param[in]  ctx      Mock context; must be non-NULL.
 * @param[in]  key      Key bytes; must be non-NULL.
 * @param[in]  key_len  Key length in bytes.
 * @param[in]  token    Unused; present for the storage-ops ABI.
 * @param[out] out      Receives the blob copy on success; must be non-NULL.
 *
 * @return AEGIS_OK with out filled (caller frees out->data); INVALID for NULL
 *         ctx/key/out, NOT_FOUND for a missing key, NOMEM on alloc failure.
 *
 * Ownership: caller owns out->data and must free() it.
 */
static aegis_status_t mock_get(void* ctx, const void* key, size_t key_len,
                               const aegis_cancellation_token_t* token, aegis_storage_blob_t* out)
{
    (void)token;
    struct mock_ctx* m = (struct mock_ctx*)ctx;
    if (!m || !key || !out) {
        return AEGIS_ERR_INVALID;
    }
    out->data = NULL;
    out->len  = 0;
    for (size_t i = 0; i < m->count; i++) {
        if (m->entries[i].key_len == key_len && memcmp(m->entries[i].key, key, key_len) == 0) {
            out->data = malloc(m->entries[i].value_len);
            if (!out->data) {
                return AEGIS_ERR_NOMEM;
            }
            memcpy(out->data, m->entries[i].value, m->entries[i].value_len);
            out->len = m->entries[i].value_len;
            return AEGIS_OK;
        }
    }
    return AEGIS_ERR_NOT_FOUND;
}

/**
 * @brief Delete the entry stored under a key.
 *
 * Linear scan for a matching key; on a hit frees key/value and shifts the
 * tail down to keep the array dense.
 *
 * @param[in] ctx      Mock context; must be non-NULL.
 * @param[in] key      Key bytes; must be non-NULL.
 * @param[in] key_len  Key length in bytes.
 * @param[in] token    Unused; present for the storage-ops ABI.
 *
 * @return AEGIS_OK deleted; INVALID for NULL ctx/key, NOT_FOUND when absent.
 */
static aegis_status_t mock_del(void* ctx, const void* key, size_t key_len,
                               const aegis_cancellation_token_t* token)
{
    (void)token;
    struct mock_ctx* m = (struct mock_ctx*)ctx;
    if (!m || !key) {
        return AEGIS_ERR_INVALID;
    }
    for (size_t i = 0; i < m->count; i++) {
        if (m->entries[i].key_len == key_len && memcmp(m->entries[i].key, key, key_len) == 0) {
            free(m->entries[i].key);
            free(m->entries[i].value);
            /* Shift remaining entries down. */
            for (size_t j = i; j + 1 < m->count; j++) {
                m->entries[j] = m->entries[j + 1];
            }
            m->count--;
            return AEGIS_OK;
        }
    }
    return AEGIS_ERR_NOT_FOUND;
}

static aegis_storage_ops_t s_mock_ops = {
    .put = mock_put,
    .get = mock_get,
    .del = mock_del,
};

/**
 * @brief Create an empty mock storage context.
 *
 * Allocates the context plus an initial 16-entry array; unit-test only, no
 * external dependencies.
 *
 * @param[out] out  Receives the opaque context on success; untouched on failure.
 *
 * @return AEGIS_OK on success; AEGIS_ERR_INVALID/NOMEM otherwise.
 *
 * Ownership: caller owns *out and must call aegis_mock_storage_destroy().
 */
aegis_status_t aegis_mock_storage_create(void** out)
{
    if (!out) {
        return AEGIS_ERR_INVALID;
    }
    struct mock_ctx* m = calloc(1, sizeof(*m));
    if (!m) {
        return AEGIS_ERR_NOMEM;
    }
    m->cap     = 16;
    m->entries = malloc(sizeof(*m->entries) * m->cap);
    if (!m->entries) {
        free(m);
        return AEGIS_ERR_NOMEM;
    }
    *out = m;
    return AEGIS_OK;
}

/**
 * @brief Destroy a mock storage context and all stored blobs.
 *
 * Frees every key/value pair, the entry array, and the context. NULL is a
 * no-op. Must not be called while a blob borrowed from this store is in use.
 *
 * @param[in] ctx  Context to destroy; NULL is accepted.
 */
void aegis_mock_storage_destroy(void* ctx)
{
    if (!ctx) {
        return;
    }
    struct mock_ctx* m = (struct mock_ctx*)ctx;
    for (size_t i = 0; i < m->count; i++) {
        free(m->entries[i].key);
        free(m->entries[i].value);
    }
    free(m->entries);
    free(m);
}

/**
 * @brief Fill the storage-ops vtable bound to a mock context.
 *
 * Copies the static put/get/del function pointers into *ops and binds ctx as
 * the callback context, so the mock plugs into any storage consumer.
 *
 * @param[in]  ctx  Opaque mock context to bind; passed through untouched.
 * @param[out] ops  Vtable to fill; NULL is a no-op, otherwise must be non-NULL.
 */
void aegis_mock_storage_ops(void* ctx, aegis_storage_ops_t* ops)
{
    if (!ops) {
        return;
    }
    *ops     = s_mock_ops;
    ops->ctx = ctx;
}
