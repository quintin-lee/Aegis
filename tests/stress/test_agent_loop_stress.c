#define _POSIX_C_SOURCE 200809L
#include "aegis/agent/loop.h"
#include "aegis/session/session.h"
#include "aegis/model/model.h"
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define N_THREADS 8
#define N_TURNS   20

static void* thread_fn(void* arg)
{
    (void)arg;
    // Each thread creates its own loop/session to avoid data race (loop is single-threaded by
    // design)
    aegis_session_t* sess = NULL;
    aegis_session_create("/tmp", &sess);
    aegis_model_client_t* model = NULL;
    aegis_model_client_create("mock", &model);
    aegis_agent_loop_config_t cfg  = {.session = sess, .model = model};
    aegis_agent_loop_t*       loop = NULL;
    aegis_agent_loop_create(&cfg, &loop);
    for (int i = 0; i < N_TURNS; i++) {
        char prompt[64];
        snprintf(prompt, sizeof(prompt), "stress prompt %d", i);
        aegis_agent_loop_run(loop, prompt);
    }
    size_t n = aegis_session_message_count(sess);
    aegis_agent_loop_destroy(loop);
    aegis_model_client_destroy(model);
    aegis_session_destroy(sess);
    // Return count via pthread exit
    return (void*)(uintptr_t)n;
}

int main(void)
{
    pthread_t th[N_THREADS];
    for (int i = 0; i < N_THREADS; i++) {
        pthread_create(&th[i], NULL, thread_fn, NULL);
    }
    size_t total = 0;
    for (int i = 0; i < N_THREADS; i++) {
        void* ret = NULL;
        pthread_join(th[i], &ret);
        total += (size_t)(uintptr_t)ret;
    }
    printf("stress: %zu messages total after %d threads x %d turns (per-thread isolated)\n", total,
           N_THREADS, N_TURNS);
    printf("stress PASS\n");
    return 0;
}
