/**
 * @file test_runtime.c
 * @brief Tests for aegis_runtime lifecycle.
 */
#include "aegis/runtime.h"
#include "aegis/config.h"
#include <assert.h>
#include <stdio.h>

static void test_create_and_destroy(void)
{
    aegis_runtime_t* rt = NULL;
    assert(aegis_runtime_create(&rt) == AEGIS_OK);
    assert(rt != NULL);
    aegis_runtime_destroy(rt);
}

static void test_null_create(void)
{
    aegis_status_t st = aegis_runtime_create(NULL);
    assert(st == AEGIS_ERR_INVALID);
}

static void test_null_operations(void)
{
    assert(aegis_runtime_start(NULL) == AEGIS_ERR_INVALID);
    assert(aegis_runtime_stop(NULL) == AEGIS_ERR_INVALID);
    aegis_runtime_destroy(NULL); /* should not crash */
}

static void test_full_lifecycle(void)
{
    aegis_runtime_t* rt = NULL;
    assert(aegis_runtime_create(&rt) == AEGIS_OK);

    assert(aegis_runtime_start(rt) == AEGIS_OK);
    assert(aegis_runtime_stop(rt) == AEGIS_OK);

    aegis_runtime_destroy(rt);
}

static void test_idempotent_start(void)
{
    aegis_runtime_t* rt = NULL;
    assert(aegis_runtime_create(&rt) == AEGIS_OK);

    assert(aegis_runtime_start(rt) == AEGIS_OK);
    assert(aegis_runtime_start(rt) == AEGIS_OK); /* idempotent */
    assert(aegis_runtime_start(rt) == AEGIS_OK); /* still idempotent */

    assert(aegis_runtime_stop(rt) == AEGIS_OK);
    aegis_runtime_destroy(rt);
}

static void test_idempotent_stop(void)
{
    aegis_runtime_t* rt = NULL;
    assert(aegis_runtime_create(&rt) == AEGIS_OK);

    assert(aegis_runtime_start(rt) == AEGIS_OK);
    assert(aegis_runtime_stop(rt) == AEGIS_OK);
    assert(aegis_runtime_stop(rt) == AEGIS_OK); /* idempotent */
    assert(aegis_runtime_stop(rt) == AEGIS_OK); /* still idempotent */

    aegis_runtime_destroy(rt);
}

static void test_start_stop_twice(void)
{
    aegis_runtime_t* rt = NULL;
    assert(aegis_runtime_create(&rt) == AEGIS_OK);

    assert(aegis_runtime_start(rt) == AEGIS_OK);
    assert(aegis_runtime_stop(rt) == AEGIS_OK);

    /* Restart should work */
    assert(aegis_runtime_start(rt) == AEGIS_OK);
    assert(aegis_runtime_stop(rt) == AEGIS_OK);

    aegis_runtime_destroy(rt);
}

static void test_config_default(void)
{
    aegis_config_t c = aegis_config_default();
    assert(c.max_workers == 4);
    assert(c.event_queue_cap == 256);
    assert(c.stop_timeout_ms == 5000L);
    assert(c.name == NULL);
}

int main(void)
{
    test_create_and_destroy();
    test_null_create();
    test_null_operations();
    test_full_lifecycle();
    test_idempotent_start();
    test_idempotent_stop();
    test_start_stop_twice();
    test_config_default();

    printf("runtime lifecycle test passed\n");
    return 0;
}
