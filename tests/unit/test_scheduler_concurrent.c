/**
 * @file test_scheduler_concurrent.c
 * @brief Concurrency and stress tests for the scheduler.
 *
 * Invariants verified under multi-threaded load:
 *   1. No duplicate concurrent dispatch (each task handed to at most
 *      one executor at any time).
 *   2. Exactly-once dispatch for tasks driven to a terminal state.
 *   3. Dependency gating holds under concurrency: a dependent is never
 *      dispatched before its dependency source completed successfully.
 *   4. Liveness: mixed poll/next/notify/introspection from many threads
 *      makes progress without deadlock or livelock.
 */
#include "aegis/scheduler.h"
#include "aegis/graph.h"
#include "aegis/task.h"
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int             g_failures = 0;
static pthread_mutex_t g_lock     = PTHREAD_MUTEX_INITIALIZER;

static void fail(const char* fmt, ...)
{
    pthread_mutex_lock(&g_lock);
    g_failures++;
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    fflush(stdout);
    pthread_mutex_unlock(&g_lock);
}

/* Per-task observation slots, indexed by (task id - 1); the fixture
 * assigns ids 1..N deterministically via a fresh graph. */
typedef struct {
    atomic_int held;       /**< 1 while a thread believes it owns a dispatch. */
    atomic_int dispatches; /**< Total times the task was handed out. */
    atomic_int done;       /**< 1 once driven to SUCCESS by its executor. */
} slot_t;

static slot_t* g_slots      = NULL;
static size_t  g_slot_count = 0;
/* Task ids come from a PROCESS-GLOBAL allocator, so a fresh fixture's
 * first id is arbitrary (earlier tests consume ids too). All slot
 * indexes are computed relative to the fixture's base id. */
static uint32_t g_id_base = 0;

static void init_slots(size_t n, uint32_t id_base)
{
    free(g_slots);
    g_slots = (slot_t*)calloc(n, sizeof(slot_t));
    assert(g_slots);
    g_slot_count = n;
    g_id_base    = id_base;
}

static slot_t* slot_for(const aegis_task_t* t)
{
    uint32_t id  = aegis_task_id(t);
    size_t   idx = (size_t)(id - g_id_base);
    if (id < g_id_base || idx >= g_slot_count) {
        fail("slot_for: id %u out of fixture range\n", id);
        return &g_slots[0];
    }
    return &g_slots[idx];
}

/* ── Test 1: concurrent hammer on an independent-task graph ───────────────── */

enum { HAMMER_TASKS = 64, HAMMER_THREADS = 8, HAMMER_ITERS = 200 };

typedef struct {
    int                tid;
    aegis_scheduler_t* sched;
} hammer_arg_t;

static void* hammer_worker(void* p)
{
    hammer_arg_t* arg = (hammer_arg_t*)p;

    for (int i = 0; i < HAMMER_ITERS; i++) {
        size_t enqueued = 0;
        aegis_scheduler_poll(arg->sched, &enqueued);

        aegis_task_t* t = NULL;
        if (aegis_scheduler_next(arg->sched, &t) == AEGIS_OK) {
            slot_t* slot = slot_for(t);

            int prev = atomic_fetch_add(&slot->held, 1);
            if (prev != 0) {
                fail("hammer: task %u dispatched concurrently twice\n", aegis_task_id(t));
            }

            atomic_fetch_add(&slot->dispatches, 1);

            /* Simulate executor lifecycle. Publish the completion marker
             * BEFORE the terminal state: any observer that sees SUCCESS
             * (via the task mutex) must also observe done == 1. */
            aegis_task_set_state_for_test(t, AEGIS_TASK_RUNNING);
            atomic_store(&slot->done, 1);
            aegis_task_set_state_for_test(t, AEGIS_TASK_SUCCESS);

            if (aegis_scheduler_notify_complete(arg->sched, t) != AEGIS_OK) {
                fail("hammer: notify_complete failed for task %u\n", aegis_task_id(t));
            }

            atomic_store(&slot->done, 1);
            atomic_fetch_sub(&slot->held, 1);
        }

        /* Exercise introspection paths concurrently. */
        (void)aegis_scheduler_pending_count(arg->sched);
        (void)aegis_scheduler_inflight_count(arg->sched);
    }
    return NULL;
}

static void test_concurrent_hammer(void)
{
    aegis_scheduler_t*  s = NULL;
    aegis_task_graph_t* g = NULL;
    assert(aegis_scheduler_create(&s) == AEGIS_OK);
    assert(aegis_task_graph_create(&g) == AEGIS_OK);
    assert(aegis_scheduler_attach(s, g) == AEGIS_OK);

    char name[32];
    for (int i = 0; i < HAMMER_TASKS; i++) {
        snprintf(name, sizeof(name), "h%d", i);
        aegis_task_t* t = NULL;
        assert(aegis_task_create(&t, name, NULL) == AEGIS_OK);
        assert(aegis_task_graph_add_task(g, t) == AEGIS_OK);
        if (i == 0) {
            init_slots(HAMMER_TASKS, aegis_task_id(t));
        }
    }

    pthread_t    threads[HAMMER_THREADS];
    hammer_arg_t args[HAMMER_THREADS];
    for (int i = 0; i < HAMMER_THREADS; i++) {
        args[i].tid   = i;
        args[i].sched = s;
        assert(pthread_create(&threads[i], NULL, hammer_worker, &args[i]) == 0);
    }
    for (int i = 0; i < HAMMER_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }

    unsigned long total = 0;
    for (int i = 0; i < HAMMER_TASKS; i++) {
        if (atomic_load(&g_slots[i].held) != 0) {
            fail("hammer: task %d left held\n", i + 1);
        }
        unsigned long d = (unsigned long)atomic_load(&g_slots[i].dispatches);
        if (d != 1) {
            fail("hammer: task %d dispatched %lu times (expected 1)\n", i + 1, d);
        }
        total += d;
    }
    if (total != (unsigned long)HAMMER_TASKS) {
        fail("hammer: total dispatches %lu (expected %d)\n", total, HAMMER_TASKS);
    }

    aegis_scheduler_destroy(s);
    aegis_task_graph_destroy(g);
}

/* ── Test 2: dependency-chain stress under concurrency ─────────────────────── */

enum { CHAIN_LEN = 300, CHAIN_THREADS = 8 };

typedef struct {
    int                tid;
    aegis_scheduler_t* sched;
    atomic_int*        completed;
} chain_arg_t;

static void* chain_worker(void* p)
{
    chain_arg_t* arg       = (chain_arg_t*)p;
    long         guard     = 0;
    const long   GUARD_MAX = 2L * 1000 * 1000; /* livelock tripwire (fast-fail) */

    while (atomic_load(arg->completed) < CHAIN_LEN) {
        if (++guard > GUARD_MAX) {
            fail("chain: livelock detected after %ld spins\n", guard);
            return NULL;
        }

        size_t enqueued = 0;
        aegis_scheduler_poll(arg->sched, &enqueued);

        aegis_task_t* t = NULL;
        while (aegis_scheduler_next(arg->sched, &t) == AEGIS_OK) {
            slot_t* slot = slot_for(t);
            size_t  idx  = (size_t)(aegis_task_id(t) - g_id_base);

            int prev = atomic_fetch_add(&slot->held, 1);
            if (prev != 0) {
                fail("chain: task %u dispatched concurrently twice\n", aegis_task_id(t));
            }

            /* Dependency gating: predecessor must have COMPLETED. */
            if (idx > 0 && atomic_load(&g_slots[idx - 1].done) != 1) {
                fail("chain: task %u dispatched before predecessor completed\n", aegis_task_id(t));
            }

            atomic_fetch_add(&slot->dispatches, 1);
            aegis_task_set_state_for_test(t, AEGIS_TASK_RUNNING);
            atomic_store(&slot->done, 1);
            aegis_task_set_state_for_test(t, AEGIS_TASK_SUCCESS);
            atomic_fetch_add(arg->completed, 1);

            if (aegis_scheduler_notify_complete(arg->sched, t) != AEGIS_OK) {
                fail("chain: notify_complete failed for task %u\n", aegis_task_id(t));
            }
            atomic_fetch_sub(&slot->held, 1);

            /* Immediately try to drain more ready work. */
            t = NULL;
        }
    }
    return NULL;
}

static void test_dependency_chain_stress(void)
{
    aegis_scheduler_t*  s = NULL;
    aegis_task_graph_t* g = NULL;
    assert(aegis_scheduler_create(&s) == AEGIS_OK);
    assert(aegis_task_graph_create(&g) == AEGIS_OK);
    assert(aegis_scheduler_attach(s, g) == AEGIS_OK);

    aegis_task_t* tasks[CHAIN_LEN];
    char          name[32];
    for (int i = 0; i < CHAIN_LEN; i++) {
        snprintf(name, sizeof(name), "c%d", i);
        assert(aegis_task_create(&tasks[i], name, NULL) == AEGIS_OK);
        assert(aegis_task_graph_add_task(g, tasks[i]) == AEGIS_OK);
        if (i == 0) {
            init_slots((size_t)CHAIN_LEN, aegis_task_id(tasks[0]));
        }
    }
    for (int i = 0; i + 1 < CHAIN_LEN; i++) {
        assert(aegis_task_graph_add_dependency(g, tasks[i], tasks[i + 1]) == AEGIS_OK);
    }
    assert(aegis_task_graph_validate(g) == AEGIS_OK);

    atomic_int completed;
    atomic_init(&completed, 0);

    pthread_t   threads[CHAIN_THREADS];
    chain_arg_t args[CHAIN_THREADS];
    for (int i = 0; i < CHAIN_THREADS; i++) {
        args[i].tid       = i;
        args[i].sched     = s;
        args[i].completed = &completed;
        assert(pthread_create(&threads[i], NULL, chain_worker, &args[i]) == 0);
    }
    for (int i = 0; i < CHAIN_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }

    if (atomic_load(&completed) != CHAIN_LEN) {
        fail("chain: completed %d (expected %d)\n", atomic_load(&completed), CHAIN_LEN);
    }
    for (int i = 0; i < CHAIN_LEN; i++) {
        if (atomic_load(&g_slots[i].dispatches) != 1) {
            fail("chain: task %d dispatched %d times (expected 1)\n", i + 1,
                 atomic_load(&g_slots[i].dispatches));
        }
        if (aegis_task_state(tasks[i]) != AEGIS_TASK_SUCCESS) {
            fail("chain: task %d not SUCCESS\n", i + 1);
        }
    }
    if (aegis_scheduler_pending_count(s) != 0 || aegis_scheduler_inflight_count(s) != 0) {
        fail("chain: scheduler not drained (pending=%zu inflight=%zu)\n",
             aegis_scheduler_pending_count(s), aegis_scheduler_inflight_count(s));
    }

    aegis_scheduler_destroy(s);
    aegis_task_graph_destroy(g);
}

int main(void)
{
    test_concurrent_hammer();
    test_dependency_chain_stress();

    if (g_failures > 0) {
        printf("%d FAILURE(S)\n", g_failures);
        return 1;
    }
    return 0;
}
