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

/**
 * @brief Smoke-test entry point: create then destroy a demo agent.
 *
 * Exercises the public create/destroy path as a quick sanity check; takes no
 * arguments and is not a production binary.
 *
 * @param[in] argc  Unused argument count.
 * @param[in] argv  Unused argument vector.
 *
 * @return 0 when the agent round-trips cleanly, 1 on creation failure.
 */
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
