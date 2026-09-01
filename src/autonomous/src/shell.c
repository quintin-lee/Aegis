/**
 * @file shell.c
 * @brief Minimal CLI entry point for the Aegis runtime.
 *
 * Creates and destroys a demo agent to exercise the public API.
 * Intended as a smoke test and quick sanity check; not a production binary.
 */
#include "aegis/agent/agent.h"
#include "aegis/status.h"
#include <stdio.h>

int main(int argc, char** argv)
{
    (void)argc;
    (void)argv;

    aegis_agent_t* agent = NULL;
    aegis_status_t st    = aegis_agent_create(&agent, "demo");
    if (st != AEGIS_OK) {
        fprintf(stderr, "aegis_agent_create failed: %s\n", aegis_status_str(st));
        return 1;
    }
    printf("agent created: ok\n");
    aegis_agent_destroy(agent);
    return 0;
}
