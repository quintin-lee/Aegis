/**
 * @file test_tool_concurrent.c
 * @brief Concurrency tests: shared registry + executor invoke hammer with
 *        per-invocation result verification, and a concurrent
 *        register/find race.
 */
#include "aegis/tool.h"
#include "aegis/common/time.h"

#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Shared mock tool ───────────────────────────────────────────────── */

static const aegis_tool_param_spec_t k_add_params[] = {
    {"a", AEGIS_TOOL_VAL_INT, true, NULL},
    {"b", AEGIS_TOOL_VAL_INT, true, NULL},
};
static const aegis_tool_schema_t k_add_schema = {k_add_params, 2};

static aegis_status_t add_ints(void* user, const aegis_tool_args_t* args,
                               const aegis_cancellation_token_t* token, aegis_tool_result_t* out)
{
    (void)user;
    (void)token;
    const aegis_tool_value_t *a = NULL, *b = NULL;
    if (!aegis_tool_args_find(args, "a", &a) || !aegis_tool_args_find(args, "b", &b)) {
        return AEGIS_ERR_INVALID;
    }
    return aegis_tool_result_set_int(out, a->as.i + b->as.i);
}

static const aegis_tool_schema_t k_no_schema = {NULL, 0}; /* empty schema */

static aegis_status_t ok_tool(void* user, const aegis_tool_args_t* args,
                              const aegis_cancellation_token_t* token, aegis_tool_result_t* out)
{
    (void)user;
    (void)args;
    (void)token;
    return aegis_tool_result_set_bool(out, true);
}

static aegis_tool_registry_t* g_reg  = NULL;
static aegis_executor_t*      g_exec = NULL;

#define HAMMER_THREADS    8
#define HAMMER_PER_THREAD 25

typedef struct hammer_arg {
    int        tid;
    atomic_int failures;
} hammer_arg_t;

static void* hammer_thread(void* p)
{
    hammer_arg_t* arg = (hammer_arg_t*)p;

    for (int i = 0; i < HAMMER_PER_THREAD; i++) {
        aegis_task_t* task = NULL;
        char          name[64];
        snprintf(name, sizeof(name), "hammer-%d-%d", arg->tid, i);
        if (aegis_task_create(&task, name, "") != AEGIS_OK) {
            atomic_fetch_add(&arg->failures, 1);
            continue;
        }

        const int64_t      x    = arg->tid * 1000 + i;
        aegis_tool_args_t* args = NULL;
        if (aegis_tool_args_create(&args) != AEGIS_OK ||
            aegis_tool_args_add_int(args, "a", x) != AEGIS_OK ||
            aegis_tool_args_add_int(args, "b", 2 * x) != AEGIS_OK) {
            atomic_fetch_add(&arg->failures, 1);
            aegis_task_destroy(task);
            continue;
        }

        if (aegis_tool_submit(g_exec, g_reg, task, "add_ints", args) != AEGIS_OK) {
            atomic_fetch_add(&arg->failures, 1);
            aegis_task_destroy(task);
            continue;
        }

        aegis_exec_result_t result;
        memset(&result, 0, sizeof(result));
        if (aegis_executor_wait(g_exec, aegis_task_id(task), &result, -1) != AEGIS_OK ||
            result.outcome != AEGIS_EXEC_COMPLETED) {
            atomic_fetch_add(&arg->failures, 1);
            aegis_task_destroy(task);
            continue;
        }

        size_t      len  = 0;
        const void* data = aegis_task_output(task, &len);
        int64_t     got  = 0;
        if (!data || len != 8) {
            atomic_fetch_add(&arg->failures, 1);
        } else {
            memcpy(&got, data, 8);
            if (got != 3 * x) {
                atomic_fetch_add(&arg->failures, 1);
            }
        }
        aegis_task_destroy(task);
    }
    return NULL;
}

static void test_invoke_hammer(void)
{
    assert(aegis_tool_registry_create(&g_reg) == AEGIS_OK);

    aegis_tool_def_t d;
    memset(&d, 0, sizeof(d));
    d.name    = "add_ints";
    d.schema  = k_add_schema;
    d.execute = add_ints;
    assert(aegis_tool_registry_register(g_reg, &d) == AEGIS_OK);

    aegis_executor_config_t cfg;
    cfg.worker_count   = 8;
    cfg.queue_capacity = 256;
    assert(aegis_executor_create(&g_exec, &cfg) == AEGIS_OK);

    pthread_t    threads[HAMMER_THREADS];
    hammer_arg_t hargs[HAMMER_THREADS];
    for (int t = 0; t < HAMMER_THREADS; t++) {
        hargs[t].tid = t;
        atomic_init(&hargs[t].failures, 0);
        assert(pthread_create(&threads[t], NULL, hammer_thread, &hargs[t]) == 0);
    }
    for (int t = 0; t < HAMMER_THREADS; t++) {
        pthread_join(threads[t], NULL);
    }

    int total_failures = 0;
    for (int t = 0; t < HAMMER_THREADS; t++) {
        total_failures += atomic_load(&hargs[t].failures);
    }
    assert(total_failures == 0);

    aegis_executor_destroy(g_exec);
    g_exec = NULL;
    aegis_tool_registry_destroy(g_reg);
    g_reg = NULL;
}

/* ── Register / find race ───────────────────────────────────────────── */

#define REG_WRITERS          4
#define REG_TOOLS_PER_WRITER 16 /* 64 tools total */
#define REG_READERS          4
#define READER_ROUNDS        200

typedef struct reg_writer_arg {
    int        wid; /* 0..REG_WRITERS-1 */
    atomic_int failures;
} reg_writer_arg_t;

typedef struct reg_reader_arg {
    atomic_int lookups;
    atomic_int misses;
} reg_reader_arg_t;

/* Borrowed name storage must outlive registrations: static. */
static char g_race_names[REG_WRITERS][REG_TOOLS_PER_WRITER][32];

static void* reg_writer_thread(void* p)
{
    reg_writer_arg_t* arg = (reg_writer_arg_t*)p;

    for (int i = 0; i < REG_TOOLS_PER_WRITER; i++) {
        aegis_tool_def_t d;
        memset(&d, 0, sizeof(d));
        snprintf(g_race_names[arg->wid][i], sizeof(g_race_names[arg->wid][i]), "race-%d-%d",
                 arg->wid, i);
        d.name    = g_race_names[arg->wid][i];
        d.schema  = k_no_schema;
        d.execute = ok_tool;
        if (aegis_tool_registry_register(g_reg, &d) != AEGIS_OK) {
            atomic_fetch_add(&arg->failures, 1);
        }
    }
    return NULL;
}

static void* reg_reader_thread(void* p)
{
    reg_reader_arg_t* arg = (reg_reader_arg_t*)p;

    unsigned seed = (unsigned)(uintptr_t)p * 2654435761u + 1u;
    for (int r = 0; r < READER_ROUNDS; r++) {
        /* Look up a random raced tool: may legitimately not exist yet. */
        char name[32];
        snprintf(name, sizeof(name), "race-%u-%u", rand_r(&seed) % REG_WRITERS,
                 rand_r(&seed) % REG_TOOLS_PER_WRITER);

        aegis_tool_def_t found;
        memset(&found, 0, sizeof(found));
        if (aegis_tool_registry_find(g_reg, name, &found) == AEGIS_OK) {
            atomic_fetch_add(&arg->lookups, 1);
            assert(found.execute == ok_tool); /* copy is coherent */
            /* Found tools are fully registered: direct call must work. */
            aegis_tool_result_t out;
            memset(&out, 0, sizeof(out));
            assert(aegis_tool_call(g_reg, name, NULL, 1000, &out) == AEGIS_OK);
            aegis_tool_result_destroy(&out);
        } else {
            atomic_fetch_add(&arg->misses, 1);
        }

        /* The pre-registered tool must ALWAYS resolve. */
        memset(&found, 0, sizeof(found));
        assert(aegis_tool_registry_find(g_reg, "adder-base", &found) == AEGIS_OK);
    }
    return NULL;
}

static void test_register_find_race(void)
{
    assert(aegis_tool_registry_create(&g_reg) == AEGIS_OK);

    /* Pre-registered tool readers can always rely on. */
    aegis_tool_def_t base;
    memset(&base, 0, sizeof(base));
    base.name    = "adder-base";
    base.schema  = k_no_schema;
    base.execute = ok_tool;
    assert(aegis_tool_registry_register(g_reg, &base) == AEGIS_OK);

    pthread_t        writers[REG_WRITERS];
    pthread_t        readers[REG_READERS];
    reg_writer_arg_t wargs[REG_WRITERS];
    reg_reader_arg_t rargs[REG_READERS];

    for (int t = 0; t < REG_READERS; t++) {
        atomic_init(&rargs[t].lookups, 0);
        atomic_init(&rargs[t].misses, 0);
        assert(pthread_create(&readers[t], NULL, reg_reader_thread, &rargs[t]) == 0);
    }
    for (int t = 0; t < REG_WRITERS; t++) {
        wargs[t].wid = t;
        atomic_init(&wargs[t].failures, 0);
        assert(pthread_create(&writers[t], NULL, reg_writer_thread, &wargs[t]) == 0);
    }
    for (int t = 0; t < REG_WRITERS; t++) {
        pthread_join(writers[t], NULL);
    }
    for (int t = 0; t < REG_READERS; t++) {
        pthread_join(readers[t], NULL);
    }

    int write_failures = 0;
    for (int t = 0; t < REG_WRITERS; t++) {
        write_failures += atomic_load(&wargs[t].failures);
    }
    assert(write_failures == 0);
    assert(aegis_tool_registry_count(g_reg) == (size_t)(REG_WRITERS * REG_TOOLS_PER_WRITER) + 1);

    /* Every raced tool resolves once writers are done. */
    for (int w = 0; w < REG_WRITERS; w++) {
        for (int i = 0; i < REG_TOOLS_PER_WRITER; i++) {
            aegis_tool_def_t found;
            assert(aegis_tool_registry_find(g_reg, g_race_names[w][i], &found) == AEGIS_OK);
        }
    }

    int lookups = 0, misses = 0;
    for (int t = 0; t < REG_READERS; t++) {
        lookups += atomic_load(&rargs[t].lookups);
        misses += atomic_load(&rargs[t].misses);
    }
    assert(lookups + misses == REG_READERS * READER_ROUNDS);
    printf("register/find race: %d hits, %d not-yet-registered misses\n", lookups, misses);

    aegis_tool_registry_destroy(g_reg);
    g_reg = NULL;
}

int main(void)
{
    test_invoke_hammer();
    test_register_find_race();
    printf("test_tool_concurrent: all cases passed\n");
    return 0;
}
