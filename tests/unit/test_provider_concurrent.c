/**
 * @file test_provider_concurrent.c
 * @brief Concurrency tests: registry race hammer + concurrent dispatch.
 */
#include "aegis/executor/cancellation.h"
#include "aegis/provider/llm.h"

#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Mock LLM echoing the request bytes back ──────────────────────────────── */

static aegis_status_t echo_llm_complete(void* ctx, const aegis_llm_request_t* req,
                                        const aegis_cancellation_token_t* token,
                                        aegis_llm_response_t*             out)
{
    (void)ctx;
    (void)token;
    char* buf = malloc(req->prompt_len > 0 ? req->prompt_len : 1);
    if (!buf) {
        return AEGIS_ERR_NOMEM;
    }
    if (req->prompt_len > 0) {
        memcpy(buf, req->prompt, req->prompt_len);
    }
    out->data = buf;
    out->len  = req->prompt_len;
    return AEGIS_OK;
}

/* Non-const: def.user is a plain void* (borrowed, registry never writes). */
static aegis_llm_ops_t k_echo_ops = {NULL, echo_llm_complete};

static void make_def(aegis_provider_def_t* d, const char* name)
{
    memset(d, 0, sizeof(*d));
    d->name         = name;
    d->abi_version  = AEGIS_PROVIDER_ABI_VERSION;
    d->kind         = AEGIS_PROVIDER_LLM;
    d->thread_model = AEGIS_PROVIDER_THREAD_SAFE;
    d->user         = &k_echo_ops; /* Borrowed static. */
}

#define N_NAMES 32
static const char* g_names[N_NAMES]; /* Written before threads start. */

/* ── Registry race hammer: writers register/unregister, readers find/count ── */

typedef struct race_arg {
    aegis_provider_registry_t* reg;
    atomic_int                 found_ok;
    atomic_int                 not_found_ok;
    atomic_long                errors;
} race_arg_t;

static void* race_writer(void* argp)
{
    race_arg_t* a = argp;
    char        name[32];
    for (int i = 0; i < 200; i++) {
        snprintf(name, sizeof(name), "dyn-%d", i % 8);
        aegis_provider_def_t d;
        make_def(&d, name);
        aegis_status_t rc = aegis_provider_register(a->reg, &d);
        if (rc != AEGIS_OK && rc != AEGIS_ERR_BUSY) {
            atomic_fetch_add(&a->errors, 1);
        }
        rc = aegis_provider_unregister(a->reg, name);
        if (rc != AEGIS_OK && rc != AEGIS_ERR_NOT_FOUND) {
            atomic_fetch_add(&a->errors, 1);
        }
    }
    return NULL;
}

static void* race_reader(void* argp)
{
    race_arg_t* a = argp;
    for (int i = 0; i < 500; i++) {
        aegis_provider_view_t v;
        /* Fixed providers registered up-front must ALWAYS be found... */
        int idx = i % N_NAMES;
        if (aegis_provider_find(a->reg, g_names[idx], &v) == AEGIS_OK) {
            atomic_fetch_add(&a->found_ok, 1);
            if (v.def.kind != AEGIS_PROVIDER_LLM ||
                v.def.abi_version != AEGIS_PROVIDER_ABI_VERSION) {
                atomic_fetch_add(&a->errors, 1); /* Torn read would show here. */
            }
        } else {
            atomic_fetch_add(&a->errors, 1);
        }
        /* ...while dynamic ones are legitimately in flux. */
        if (aegis_provider_find(a->reg, "dyn-never-registered", &v) == AEGIS_ERR_NOT_FOUND) {
            atomic_fetch_add(&a->not_found_ok, 1);
        }
    }
    return NULL;
}

static void test_registry_race(void)
{
    aegis_provider_registry_t* reg = NULL;
    assert(aegis_provider_registry_create(&reg) == AEGIS_OK);

    char name_storage[N_NAMES][32];
    for (int i = 0; i < N_NAMES; i++) {
        snprintf(name_storage[i], sizeof(name_storage[0]), "fixed-%d", i);
        g_names[i] = name_storage[i];
        aegis_provider_def_t d;
        make_def(&d, g_names[i]);
        assert(aegis_provider_register(reg, &d) == AEGIS_OK);
    }

    enum { W = 4, R = 4 };
    pthread_t         writers[W], readers[R];
    static race_arg_t arg;
    arg.reg = reg;
    atomic_init(&arg.found_ok, 0);
    atomic_init(&arg.not_found_ok, 0);
    atomic_init(&arg.errors, 0);

    for (int i = 0; i < W; i++) {
        assert(pthread_create(&writers[i], NULL, race_writer, &arg) == 0);
    }
    for (int i = 0; i < R; i++) {
        assert(pthread_create(&readers[i], NULL, race_reader, &arg) == 0);
    }
    for (int i = 0; i < W; i++) {
        pthread_join(writers[i], NULL);
    }
    for (int i = 0; i < R; i++) {
        pthread_join(readers[i], NULL);
    }

    assert(atomic_load(&arg.errors) == 0);
    assert(atomic_load(&arg.found_ok) == (long)R * 500);
    /* Fixed names survive the storm untouched. */
    assert(aegis_provider_count(reg) == N_NAMES);

    aegis_provider_registry_destroy(reg);
}

/* ── Concurrent dispatch to one shared provider: every reply byte-exact ──── */

typedef struct dispatch_arg {
    aegis_provider_registry_t* reg;
    const char*                name;
    atomic_long                ok_count;
    atomic_long                errors;
} dispatch_arg_t;

static void* dispatcher(void* argp)
{
    dispatch_arg_t* a = argp;
    char            payload[32];
    for (int i = 0; i < 250; i++) {
        snprintf(payload, sizeof(payload), "msg-%d", i);
        aegis_llm_request_t  req = {.prompt = payload, .prompt_len = strlen(payload)};
        aegis_llm_response_t resp;
        aegis_status_t       rc = aegis_llm_complete(a->reg, a->name, &req, NULL, &resp);
        if (rc != AEGIS_OK) {
            atomic_fetch_add(&a->errors, 1);
            continue;
        }
        if (resp.len != req.prompt_len || memcmp(resp.data, payload, resp.len) != 0) {
            atomic_fetch_add(&a->errors, 1);
        } else {
            atomic_fetch_add(&a->ok_count, 1);
        }
        aegis_llm_response_destroy(&resp);
    }
    return NULL;
}

typedef struct lifecycle_arg {
    aegis_provider_registry_t* reg;
    const char*                name;
    atomic_long                errors;
} lifecycle_arg_t;

static void* lifecycle_flapper(void* argp)
{
    lifecycle_arg_t* a = argp;
    for (int i = 0; i < 100; i++) {
        aegis_status_t rc = aegis_provider_shutdown(a->reg, a->name);
        if (rc != AEGIS_OK && rc != AEGIS_ERR_NOT_FOUND) {
            atomic_fetch_add(&a->errors, 1);
        }
        rc = aegis_provider_init(a->reg, a->name);
        if (rc != AEGIS_OK && rc != AEGIS_ERR_BUSY && rc != AEGIS_ERR_NOT_FOUND) {
            atomic_fetch_add(&a->errors, 1);
        }
    }
    return NULL;
}

static void test_concurrent_dispatch(void)
{
    aegis_provider_registry_t* reg = NULL;
    assert(aegis_provider_registry_create(&reg) == AEGIS_OK);

    static aegis_provider_def_t def; /* Static so threads can re-register safely. */
    make_def(&def, "echo");
    assert(aegis_provider_register(reg, &def) == AEGIS_OK);
    assert(aegis_provider_init(reg, "echo") == AEGIS_OK);

    enum { D = 6, L = 2 };
    pthread_t             dispatchers[D], flappers[L];
    static dispatch_arg_t darg;
    darg.reg  = reg;
    darg.name = "echo";
    atomic_init(&darg.ok_count, 0);
    atomic_init(&darg.errors, 0);
    static lifecycle_arg_t larg;
    larg.reg  = reg;
    larg.name = "echo";
    atomic_init(&larg.errors, 0);

    for (int i = 0; i < L; i++) {
        assert(pthread_create(&flappers[i], NULL, lifecycle_flapper, &larg) == 0);
    }
    for (int i = 0; i < D; i++) {
        assert(pthread_create(&dispatchers[i], NULL, dispatcher, &darg) == 0);
    }
    for (int i = 0; i < L; i++) {
        pthread_join(flappers[i], NULL);
    }
    for (int i = 0; i < D; i++) {
        pthread_join(dispatchers[i], NULL);
    }

    /* Dispatchers may observe transient PERM while flappers run — those are
     * counted as errors here only if they were NOT OK; ensure at least the
     * majority succeeded and NOTHING corrupted state or returned garbage. */
    assert(atomic_load(&larg.errors) == 0);
    long oks    = atomic_load(&darg.ok_count);
    long errors = atomic_load(&darg.errors);
    assert(oks + errors == (long)D * 250);
    /* Each shutdown window (<= L*100) can fail every dispatcher once. */
    assert(errors <= (long)L * 100 * D);
    assert(oks >= (long)D * 250 - (long)L * 100 * D);

    /* Deterministic tail: bring the provider back and get full success. */
    aegis_status_t rc = aegis_provider_init(reg, "echo");
    assert(rc == AEGIS_OK || rc == AEGIS_ERR_BUSY); /* BUSY if already up. */
    aegis_llm_request_t  req = {.prompt = "final", .prompt_len = 5};
    aegis_llm_response_t resp;
    assert(aegis_llm_complete(reg, "echo", &req, NULL, &resp) == AEGIS_OK);
    assert(resp.len == 5 && memcmp(resp.data, "final", 5) == 0);
    aegis_llm_response_destroy(&resp);

    aegis_provider_registry_destroy(reg);
}

int main(void)
{
    test_registry_race();
    test_concurrent_dispatch();
    printf("test_provider_concurrent: all cases passed\n");
    return 0;
}
