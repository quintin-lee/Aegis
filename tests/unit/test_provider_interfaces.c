/**
 * @file test_provider_interfaces.c
 * @brief Unit tests: mock LLM / embedding / storage providers dispatched
 *        end-to-end through the registry, including gate and error paths.
 */
#include "aegis/cancellation.h"
#include "aegis/embedding.h"
#include "aegis/llm.h"
#include "aegis/storage.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Mock LLM: echoes prompt bytes uppercased-ish (prefix tag) ────────────── */

typedef struct mock_llm_state {
    int      calls;
    uint32_t last_max_tokens;
} mock_llm_state_t;

static aegis_status_t mock_llm_complete(void* ctx,
                                        const aegis_llm_request_t* req,
                                        const aegis_cancellation_token_t* token,
                                        aegis_llm_response_t* out)
{
    (void)token;
    mock_llm_state_t* st = ctx;
    st->calls++;
    st->last_max_tokens = req->max_tokens;

    size_t  len  = req->prompt_len + 4;
    char*   buf  = malloc(len);
    if (!buf) {
        return AEGIS_ERR_NOMEM;
    }
    memcpy(buf, "llm:", 4);
    if (req->prompt_len > 0) {
        memcpy(buf + 4, req->prompt, req->prompt_len);
    }
    out->data = buf;
    out->len  = len;
    return AEGIS_OK;
}

static const aegis_llm_ops_t k_mock_llm_ops = {NULL, mock_llm_complete};

static aegis_status_t failing_llm_complete(void* ctx,
                                           const aegis_llm_request_t* req,
                                           const aegis_cancellation_token_t* token,
                                           aegis_llm_response_t* out)
{
    (void)ctx;
    (void)req;
    (void)token;
    (void)out; /* Stays zeroed per contract. */
    return AEGIS_ERR_PROVIDER;
}

/* ── Mock embedding: dim-3 vector {n, n*2, n*3}, n = first byte or 1 ──────── */

static aegis_status_t mock_embed(void* ctx,
                                 const char* text,
                                 size_t text_len,
                                 const aegis_cancellation_token_t* token,
                                 aegis_embedding_result_t* out)
{
    (void)ctx;
    (void)token;
    float base = (text_len > 0) ? (float)(unsigned char)text[0] : 1.0f;
    if (base == 0.0f) {
        base = 1.0f;
    }
    float* v = malloc(3 * sizeof(float));
    if (!v) {
        return AEGIS_ERR_NOMEM;
    }
    v[0] = base;
    v[1] = base * 2.0f;
    v[2] = base * 3.0f;
    out->vector = v;
    out->dim    = 3;
    return AEGIS_OK;
}

static const aegis_embedding_ops_t k_mock_embed_ops = {NULL, mock_embed};

/* ── Mock storage: fixed 16-slot table of malloc'd key/value copies ───────── */

#define MOCK_STORE_SLOTS 16

typedef struct mock_store {
    void*  keys[MOCK_STORE_SLOTS];
    size_t key_lens[MOCK_STORE_SLOTS];
    void*  vals[MOCK_STORE_SLOTS];
    size_t val_lens[MOCK_STORE_SLOTS];
} mock_store_t;

static long store_find(mock_store_t* s, const void* key, size_t key_len)
{
    for (int i = 0; i < MOCK_STORE_SLOTS; i++) {
        if (s->keys[i] && s->key_lens[i] == key_len && memcmp(s->keys[i], key, key_len) == 0) {
            return i;
        }
    }
    return -1;
}

static aegis_status_t store_put(void* ctx,
                                const void* key, size_t key_len,
                                const void* value, size_t value_len,
                                const aegis_cancellation_token_t* token)
{
    (void)token;
    mock_store_t* s  = ctx;
    long          at = store_find(s, key, key_len);
    if (at < 0) {
        for (int i = 0; i < MOCK_STORE_SLOTS; i++) {
            if (!s->keys[i]) {
                at = i;
                break;
            }
        }
        if (at < 0) {
            return AEGIS_ERR_BUSY; /* Table full. */
        }
        s->keys[at] = malloc(key_len);
        if (!s->keys[at]) {
            return AEGIS_ERR_NOMEM;
        }
        memcpy(s->keys[at], key, key_len);
        s->key_lens[at] = key_len;
    } else {
        free(s->vals[at]); /* Overwrite semantics. */
        s->vals[at] = NULL;
    }
    if (value_len > 0) {
        s->vals[at] = malloc(value_len);
        if (!s->vals[at]) {
            return AEGIS_ERR_NOMEM;
        }
        memcpy(s->vals[at], value, value_len);
    }
    s->val_lens[at] = value_len;
    return AEGIS_OK;
}

static aegis_status_t store_get(void* ctx,
                                const void* key, size_t key_len,
                                const aegis_cancellation_token_t* token,
                                aegis_storage_blob_t* out)
{
    (void)token;
    mock_store_t* s  = ctx;
    long          at = store_find(s, key, key_len);
    if (at < 0) {
        return AEGIS_ERR_NOT_FOUND;
    }
    if (s->val_lens[at] > 0) {
        out->data = malloc(s->val_lens[at]);
        if (!out->data) {
            return AEGIS_ERR_NOMEM;
        }
        memcpy(out->data, s->vals[at], s->val_lens[at]);
    }
    out->len = s->val_lens[at];
    return AEGIS_OK;
}

static aegis_status_t store_del(void* ctx,
                                const void* key, size_t key_len,
                                const aegis_cancellation_token_t* token)
{
    (void)token;
    mock_store_t* s  = ctx;
    long          at = store_find(s, key, key_len);
    if (at < 0) {
        return AEGIS_ERR_NOT_FOUND;
    }
    free(s->keys[at]);
    free(s->vals[at]);
    s->keys[at]     = NULL;
    s->vals[at]     = NULL;
    s->key_lens[at] = 0;
    s->val_lens[at] = 0;
    return AEGIS_OK;
}

static const aegis_storage_ops_t k_mock_store_ops = {NULL, store_put, store_get, store_del};

/* Mutable ops instances wired per-test: the registry borrows def.user and
 * never owns it, so these live at file scope for the whole process. */
static aegis_llm_ops_t       s_llm_ops;
static aegis_embedding_ops_t s_embed_ops;
static aegis_storage_ops_t   s_store_ops;

/* Frees every key/value copy still held by the mock table. */
static void mock_store_destroy(mock_store_t* s)
{
    for (int i = 0; i < MOCK_STORE_SLOTS; i++) {
        free(s->keys[i]);
        free(s->vals[i]);
        s->keys[i]     = NULL;
        s->vals[i]     = NULL;
        s->key_lens[i] = 0;
        s->val_lens[i] = 0;
    }
}

/* ── Registration helper ──────────────────────────────────────────────────── */

static aegis_provider_registry_t* make_reg_with_mocks(mock_llm_state_t* llm_st,
                                                      mock_store_t*     store)
{
    aegis_provider_registry_t* reg = NULL;
    assert(aegis_provider_registry_create(&reg) == AEGIS_OK);

    /* Wire per-test ctx into the file-scope ops instances. */
    s_llm_ops     = k_mock_llm_ops;
    s_llm_ops.ctx = llm_st;
    s_store_ops     = k_mock_store_ops;
    s_store_ops.ctx = store;
    s_embed_ops     = k_mock_embed_ops;
    s_embed_ops.ctx = NULL;

    aegis_provider_def_t d;
    memset(&d, 0, sizeof(d));
    d.abi_version  = AEGIS_PROVIDER_ABI_VERSION;
    d.thread_model = AEGIS_PROVIDER_THREAD_SAFE;

    d.name         = "mock-llm";
    d.kind         = AEGIS_PROVIDER_LLM;
    d.user         = &s_llm_ops;
    assert(aegis_provider_register(reg, &d) == AEGIS_OK);

    d.name = "mock-embed";
    d.kind = AEGIS_PROVIDER_EMBEDDING;
    d.user = &s_embed_ops;
    assert(aegis_provider_register(reg, &d) == AEGIS_OK);

    d.name         = "mock-store";
    d.kind         = AEGIS_PROVIDER_STORAGE;
    d.user         = &s_store_ops;
    assert(aegis_provider_register(reg, &d) == AEGIS_OK);

    d.name         = "fail-llm";
    d.kind         = AEGIS_PROVIDER_LLM;
    /* Non-const: def.user is a plain void* (borrowed, registry never writes). */
    static aegis_llm_ops_t fail_ops = {NULL, failing_llm_complete};
    d.user         = &fail_ops;
    assert(aegis_provider_register(reg, &d) == AEGIS_OK);

    assert(aegis_provider_init(reg, "mock-llm") == AEGIS_OK);
    assert(aegis_provider_init(reg, "mock-embed") == AEGIS_OK);
    assert(aegis_provider_init(reg, "mock-store") == AEGIS_OK);
    assert(aegis_provider_init(reg, "fail-llm") == AEGIS_OK);
    return reg;
}

/* ── Tests ────────────────────────────────────────────────────────────────── */

static void test_llm_happy_path_and_gates(void)
{
    mock_llm_state_t llm_st = {0};
    mock_store_t     store  = {0};
    aegis_provider_registry_t* reg = make_reg_with_mocks(&llm_st, &store);

    aegis_llm_response_t resp;
    aegis_llm_request_t  req = {.prompt = "hello", .prompt_len = 5, .max_tokens = 42};
    assert(aegis_llm_complete(reg, "mock-llm", &req, NULL, &resp) == AEGIS_OK);
    assert(resp.len == 9 && memcmp(resp.data, "llm:hello", 9) == 0);
    assert(llm_st.calls == 1 && llm_st.last_max_tokens == 42);
    aegis_llm_response_destroy(&resp); /* Frees + zeroes... */
    aegis_llm_response_destroy(&resp); /* ...idempotently. */
    assert(resp.data == NULL && resp.len == 0);

    /* Wrong kind. */
    assert(aegis_llm_complete(reg, "mock-store", &req, NULL, &resp) == AEGIS_ERR_INVALID);
    /* Unknown name. */
    assert(aegis_llm_complete(reg, "nope", &req, NULL, &resp) == AEGIS_ERR_NOT_FOUND);
    /* Invalid args. */
    assert(aegis_llm_complete(NULL, "mock-llm", &req, NULL, &resp) == AEGIS_ERR_INVALID);
    assert(aegis_llm_complete(reg, "mock-llm", &req, NULL, NULL) == AEGIS_ERR_INVALID);
    /* Failure propagation verbatim; response stays zeroed. */
    assert(aegis_llm_complete(reg, "fail-llm", &req, NULL, &resp) == AEGIS_ERR_PROVIDER);
    assert(resp.data == NULL && resp.len == 0);

    /* Uninitialized -> PERM. */
    assert(aegis_provider_shutdown(reg, "mock-llm") == AEGIS_OK);
    assert(aegis_llm_complete(reg, "mock-llm", &req, NULL, &resp) == AEGIS_ERR_PERM);

    /* Pre-cancelled token short-circuits without invoking fn. */
    assert(aegis_provider_init(reg, "mock-llm") == AEGIS_OK);
    aegis_cancellation_token_t* tok = NULL;
    assert(aegis_cancellation_token_create(&tok) == AEGIS_OK);
    aegis_cancellation_token_request_cancel(tok);
    int calls_before = llm_st.calls;
    assert(aegis_llm_complete(reg, "mock-llm", &req, tok, &resp) == AEGIS_ERR_CANCELLED);
    assert(llm_st.calls == calls_before);
    /* Same gate on storage/embedding dispatch. */
    aegis_storage_blob_t blob;
    assert(aegis_storage_get(reg, "mock-store", "k", 1, tok, &blob) == AEGIS_ERR_CANCELLED);
    aegis_embedding_result_t emb;
    assert(aegis_embed(reg, "mock-embed", "x", 1, tok, &emb) == AEGIS_ERR_CANCELLED);
    aegis_cancellation_token_destroy(tok);

    aegis_provider_registry_destroy(reg);
    mock_store_destroy(&store);
}

static void test_embedding_roundtrip(void)
{
    mock_llm_state_t llm_st = {0};
    mock_store_t     store  = {0};
    aegis_provider_registry_t* reg = make_reg_with_mocks(&llm_st, &store);

    aegis_embedding_result_t emb;
    assert(aegis_embed(reg, "mock-embed", "abc", 3, NULL, &emb) == AEGIS_OK);
    assert(emb.dim == 3);
    assert(emb.vector && emb.vector[0] == 'a' && emb.vector[1] == 'a' * 2 &&
           emb.vector[2] == 'a' * 3);
    aegis_embedding_result_destroy(&emb);
    aegis_embedding_result_destroy(&emb); /* Idempotent. */

    /* Gates mirror llm. */
    assert(aegis_embed(reg, "mock-llm", "x", 1, NULL, &emb) == AEGIS_ERR_INVALID);
    assert(aegis_embed(reg, "nope", "x", 1, NULL, &emb) == AEGIS_ERR_NOT_FOUND);
    assert(aegis_embed(NULL, "mock-embed", "x", 1, NULL, &emb) == AEGIS_ERR_INVALID);
    assert(aegis_embed(reg, "mock-embed", NULL, 1, NULL, &emb) == AEGIS_ERR_INVALID);
    /* Empty text is legal (len 0). */
    assert(aegis_embed(reg, "mock-embed", "", 0, NULL, &emb) == AEGIS_OK);
    assert(emb.dim == 3 && emb.vector[0] == 1.0f);
    aegis_embedding_result_destroy(&emb);

    aegis_provider_registry_destroy(reg);
    mock_store_destroy(&store);
}

static void test_storage_roundtrip(void)
{
    mock_llm_state_t llm_st = {0};
    mock_store_t     store  = {0};
    aegis_provider_registry_t* reg = make_reg_with_mocks(&llm_st, &store);

    aegis_storage_blob_t blob;
    assert(aegis_storage_get(reg, "mock-store", "k", 1, NULL, &blob) == AEGIS_ERR_NOT_FOUND);
    assert(blob.data == NULL && blob.len == 0);

    assert(aegis_storage_put(reg, "mock-store", "k", 1, "v1", 2, NULL) == AEGIS_OK);
    assert(aegis_storage_get(reg, "mock-store", "k", 1, NULL, &blob) == AEGIS_OK);
    assert(blob.len == 2 && memcmp(blob.data, "v1", 2) == 0);
    aegis_storage_blob_destroy(&blob);
    aegis_storage_blob_destroy(&blob); /* Idempotent. */

    /* Overwrite. */
    assert(aegis_storage_put(reg, "mock-store", "k", 1, "v22", 3, NULL) == AEGIS_OK);
    assert(aegis_storage_get(reg, "mock-store", "k", 1, NULL, &blob) == AEGIS_OK);
    assert(blob.len == 3 && memcmp(blob.data, "v22", 3) == 0);
    aegis_storage_blob_destroy(&blob);

    /* Delete then missing. */
    assert(aegis_storage_delete(reg, "mock-store", "k", 1, NULL) == AEGIS_OK);
    assert(aegis_storage_delete(reg, "mock-store", "k", 1, NULL) == AEGIS_ERR_NOT_FOUND);
    assert(aegis_storage_get(reg, "mock-store", "k", 1, NULL, &blob) == AEGIS_ERR_NOT_FOUND);

    /* Zero-length value roundtrip. */
    assert(aegis_storage_put(reg, "mock-store", "empty", 5, NULL, 0, NULL) == AEGIS_OK);
    assert(aegis_storage_get(reg, "mock-store", "empty", 5, NULL, &blob) == AEGIS_OK);
    assert(blob.len == 0 && blob.data == NULL);

    /* Argument validation. */
    assert(aegis_storage_put(reg, "mock-store", NULL, 1, "v", 1, NULL) == AEGIS_ERR_INVALID);
    assert(aegis_storage_put(reg, "mock-store", "k", 0, "v", 1, NULL) == AEGIS_ERR_INVALID);
    assert(aegis_storage_put(reg, "mock-store", "k", 1, NULL, 1, NULL) == AEGIS_ERR_INVALID);
    assert(aegis_storage_get(reg, "mock-store", "k", 0, NULL, &blob) == AEGIS_ERR_INVALID);
    assert(aegis_storage_delete(reg, "mock-store", "k", 0, NULL) == AEGIS_ERR_INVALID);
    assert(aegis_storage_put(reg, "mock-llm", "k", 1, "v", 1, NULL) == AEGIS_ERR_INVALID);

    aegis_provider_registry_destroy(reg);
    mock_store_destroy(&store);
}

int main(void)
{
    test_llm_happy_path_and_gates();
    test_embedding_roundtrip();
    test_storage_roundtrip();
    printf("test_provider_interfaces: all cases passed\n");
    return 0;
}
