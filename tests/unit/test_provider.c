/**
 * @file test_provider.c
 * @brief Unit tests: provider registry, lifecycle, ownership semantics.
 */
#include "aegis/provider/provider.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/* ── Recording mock providers ─────────────────────────────────────────────── */

#define MOCK_LOG_CAP 64

typedef struct mock_log {
    int  init_calls;
    int  shutdown_calls;
    char events[MOCK_LOG_CAP][32];
    int  event_count;
} mock_log_t;

static void log_event(mock_log_t* log, const char* what)
{
    if (log->event_count < MOCK_LOG_CAP) {
        snprintf(log->events[log->event_count], sizeof(log->events[0]), "%s", what);
        log->event_count++;
    }
}

static aegis_status_t mock_init(void* user)
{
    mock_log_t* log = user;
    log->init_calls++;
    log_event(log, "init");
    return AEGIS_OK;
}

static aegis_status_t mock_init_fail(void* user)
{
    mock_log_t* log = user;
    log->init_calls++;
    log_event(log, "init-fail");
    return AEGIS_ERR_PROVIDER;
}

static void mock_shutdown(void* user)
{
    mock_log_t* log = user;
    log->shutdown_calls++;
    log_event(log, "shutdown");
}

static void make_def(aegis_provider_def_t* d, const char* name, void* user)
{
    memset(d, 0, sizeof(*d));
    d->name         = name;
    d->description  = "mock";
    d->abi_version  = AEGIS_PROVIDER_ABI_VERSION;
    d->kind         = AEGIS_PROVIDER_GENERIC;
    d->capabilities = AEGIS_CAP_NONE;
    d->thread_model = AEGIS_PROVIDER_THREAD_SAFE;
    d->init         = mock_init;
    d->shutdown     = mock_shutdown;
    d->user         = user;
}

/* ── Tests ────────────────────────────────────────────────────────────────── */

static void test_registry_lifecycle(void)
{
    assert(aegis_provider_registry_create(NULL) == AEGIS_ERR_INVALID);

    aegis_provider_registry_t* reg = NULL;
    assert(aegis_provider_registry_create(&reg) == AEGIS_OK);
    assert(reg != NULL);
    assert(aegis_provider_count(reg) == 0);
    aegis_provider_registry_destroy(NULL); /* No-op. */

    aegis_provider_def_t d;
    mock_log_t           log = {0};
    make_def(&d, "mock-a", &log);

    assert(aegis_provider_register(reg, NULL) == AEGIS_ERR_INVALID);
    assert(aegis_provider_register(NULL, &d) == AEGIS_ERR_INVALID);

    const char* borrowed_name = "mock-a"; /* Registry copies def; name stays borrowed. */
    d.name                    = borrowed_name;
    assert(aegis_provider_register(reg, &d) == AEGIS_OK);
    assert(aegis_provider_count(reg) == 1);
    assert(aegis_provider_register(reg, &d) == AEGIS_ERR_BUSY); /* Duplicate. */

    /* Bad ABI versions rejected. */
    aegis_provider_def_t bad = d;
    bad.name                 = "bad-abi";
    bad.abi_version          = AEGIS_PROVIDER_ABI_VERSION + 1;
    assert(aegis_provider_register(reg, &bad) == AEGIS_ERR_INVALID);
    bad.abi_version = 0;
    assert(aegis_provider_register(reg, &bad) == AEGIS_ERR_INVALID);
    bad.abi_version = AEGIS_PROVIDER_ABI_VERSION;
    bad.name        = ""; /* Empty name rejected. */
    assert(aegis_provider_register(reg, &bad) == AEGIS_ERR_INVALID);
    bad.name = NULL;
    assert(aegis_provider_register(reg, &bad) == AEGIS_ERR_INVALID);
    assert(aegis_provider_count(reg) == 1);

    /* find returns a value copy: mutating it must not affect the registry. */
    aegis_provider_view_t v;
    assert(aegis_provider_find(reg, "nope", &v) == AEGIS_ERR_NOT_FOUND);
    assert(aegis_provider_find(reg, NULL, &v) == AEGIS_ERR_INVALID);
    assert(aegis_provider_find(reg, "mock-a", NULL) == AEGIS_ERR_INVALID);
    assert(aegis_provider_find(reg, "mock-a", &v) == AEGIS_OK);
    assert(v.state == AEGIS_PROVIDER_REGISTERED);
    assert(v.def.kind == AEGIS_PROVIDER_GENERIC);
    v.def.kind = AEGIS_PROVIDER_LLM; /* Scrawl on the copy. */
    v.state    = AEGIS_PROVIDER_INITIALIZED;
    aegis_provider_view_t v2;
    assert(aegis_provider_find(reg, "mock-a", &v2) == AEGIS_OK);
    assert(v2.def.kind == AEGIS_PROVIDER_GENERIC);
    assert(v2.state == AEGIS_PROVIDER_REGISTERED);

    /* Unknown lifecycle targets. */
    assert(aegis_provider_init(reg, "nope") == AEGIS_ERR_NOT_FOUND);
    assert(aegis_provider_shutdown(reg, "nope") == AEGIS_ERR_NOT_FOUND);
    assert(aegis_provider_init(NULL, "x") == AEGIS_ERR_INVALID);
    assert(aegis_provider_shutdown(reg, NULL) == AEGIS_ERR_INVALID);
    assert(aegis_provider_unregister(reg, NULL) == AEGIS_ERR_INVALID);
    assert(aegis_provider_unregister(NULL, "x") == AEGIS_ERR_INVALID);
    assert(aegis_provider_unregister(reg, "nope") == AEGIS_ERR_NOT_FOUND);

    aegis_provider_registry_destroy(reg); /* Auto-shutdown path covered below. */
}

static void test_init_shutdown_transitions(void)
{
    aegis_provider_registry_t* reg = NULL;
    assert(aegis_provider_registry_create(&reg) == AEGIS_OK);

    mock_log_t           log = {0};
    aegis_provider_def_t d;
    make_def(&d, "lifecycle", &log);
    assert(aegis_provider_register(reg, &d) == AEGIS_OK);

    assert(aegis_provider_init(reg, "lifecycle") == AEGIS_OK);
    assert(log.init_calls == 1);

    aegis_provider_view_t v;
    assert(aegis_provider_find(reg, "lifecycle", &v) == AEGIS_OK);
    assert(v.state == AEGIS_PROVIDER_INITIALIZED);

    assert(aegis_provider_init(reg, "lifecycle") == AEGIS_ERR_BUSY); /* Double init. */
    assert(log.init_calls == 1);

    assert(aegis_provider_shutdown(reg, "lifecycle") == AEGIS_OK);
    assert(log.shutdown_calls == 1);
    assert(aegis_provider_shutdown(reg, "lifecycle") == AEGIS_OK); /* Idempotent... */
    assert(log.shutdown_calls == 1);                               /* ...no extra call. */
    assert(aegis_provider_find(reg, "lifecycle", &v) == AEGIS_OK);
    assert(v.state == AEGIS_PROVIDER_REGISTERED);

    /* Re-init after shutdown works. */
    assert(aegis_provider_init(reg, "lifecycle") == AEGIS_OK);
    assert(log.init_calls == 2);

    aegis_provider_registry_destroy(reg); /* Must auto-shutdown the active provider. */
    assert(log.shutdown_calls == 2);
}

static void test_init_failure_rolls_back(void)
{
    aegis_provider_registry_t* reg = NULL;
    assert(aegis_provider_registry_create(&reg) == AEGIS_OK);

    mock_log_t           log = {0};
    aegis_provider_def_t d;
    make_def(&d, "fails", &log);
    d.init = mock_init_fail;
    assert(aegis_provider_register(reg, &d) == AEGIS_OK);

    assert(aegis_provider_init(reg, "fails") == AEGIS_ERR_PROVIDER);
    assert(log.init_calls == 1);

    aegis_provider_view_t v;
    assert(aegis_provider_find(reg, "fails", &v) == AEGIS_OK);
    assert(v.state == AEGIS_PROVIDER_REGISTERED); /* Rolled back. */

    /* Dispatch-style gate: state query stays REGISTERED so re-init allowed. */
    assert(aegis_provider_init(reg, "fails") == AEGIS_ERR_PROVIDER);
    assert(log.init_calls == 2);

    aegis_provider_registry_destroy(reg);
    assert(log.shutdown_calls == 0); /* Never reached INITIALIZED. */
}

static void test_unregister_semantics(void)
{
    aegis_provider_registry_t* reg = NULL;
    assert(aegis_provider_registry_create(&reg) == AEGIS_OK);

    mock_log_t           log = {0};
    aegis_provider_def_t d;
    make_def(&d, "temp", &log);
    assert(aegis_provider_register(reg, &d) == AEGIS_OK);
    assert(aegis_provider_init(reg, "temp") == AEGIS_OK);

    /* Unregistering an INITIALIZED provider shuts it down first. */
    assert(aegis_provider_unregister(reg, "temp") == AEGIS_OK);
    assert(log.shutdown_calls == 1);
    assert(aegis_provider_count(reg) == 0);

    aegis_provider_view_t v;
    assert(aegis_provider_find(reg, "temp", &v) == AEGIS_ERR_NOT_FOUND);
    assert(aegis_provider_unregister(reg, "temp") == AEGIS_ERR_NOT_FOUND); /* Double. */

    /* Name is reusable after unregister. */
    log.init_calls = log.shutdown_calls = 0;
    assert(aegis_provider_register(reg, &d) == AEGIS_OK);
    assert(aegis_provider_count(reg) == 1);
    assert(aegis_provider_init(reg, "temp") == AEGIS_OK);
    aegis_provider_registry_destroy(reg);
    assert(log.shutdown_calls == 1);
}

static void test_null_lifecycle_callbacks_are_legal(void)
{
    aegis_provider_registry_t* reg = NULL;
    assert(aegis_provider_registry_create(&reg) == AEGIS_OK);

    aegis_provider_def_t d;
    make_def(&d, "stateless", NULL);
    d.init     = NULL;
    d.shutdown = NULL;
    assert(aegis_provider_register(reg, &d) == AEGIS_OK);
    assert(aegis_provider_init(reg, "stateless") == AEGIS_OK);
    aegis_provider_view_t v;
    assert(aegis_provider_find(reg, "stateless", &v) == AEGIS_OK);
    assert(v.state == AEGIS_PROVIDER_INITIALIZED);
    assert(aegis_provider_shutdown(reg, "stateless") == AEGIS_OK);
    assert(aegis_provider_unregister(reg, "stateless") == AEGIS_OK);

    aegis_provider_registry_destroy(reg);
}

int main(void)
{
    test_registry_lifecycle();
    test_init_shutdown_transitions();
    test_init_failure_rolls_back();
    test_unregister_semantics();
    test_null_lifecycle_callbacks_are_legal();
    printf("test_provider: all cases passed\n");
    return 0;
}
