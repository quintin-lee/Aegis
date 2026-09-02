#define _POSIX_C_SOURCE 200809L
#include "aegis/coding/coding_agent.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void expect_ok(aegis_status_t rc, const char* msg)
{
    if (rc != AEGIS_OK) {
        fprintf(stderr, "FAIL %s: %d\n", msg, (int)rc);
        assert(0);
    }
}

int main(void)
{
    aegis_coding_agent_config_t cfg = {0};
    cfg.project_root                = ".";
    cfg.model                       = "mock";
    aegis_coding_agent_t* agent     = NULL;
    expect_ok(aegis_coding_agent_create(&cfg, &agent), "create");
    assert(strcmp(aegis_coding_agent_model_name(agent), "mock") == 0);

    /* run works before switch (mock model) */
    expect_ok(aegis_coding_agent_run(agent, "hello"), "run before");

    /* invalid switches leave state intact */
    assert(aegis_coding_agent_set_model(agent, NULL) == AEGIS_ERR_INVALID);
    assert(aegis_coding_agent_set_model(agent, "") == AEGIS_ERR_INVALID);
    assert(strcmp(aegis_coding_agent_model_name(agent), "mock") == 0);

    /* hot switch */
    expect_ok(aegis_coding_agent_set_model(agent, "gpt-x"), "switch");
    assert(strcmp(aegis_coding_agent_model_name(agent), "gpt-x") == 0);
    expect_ok(aegis_coding_agent_run(agent, "again"), "run after");

    /* NULL agent rejected */
    assert(aegis_coding_agent_set_model(NULL, "x") == AEGIS_ERR_INVALID);
    assert(aegis_coding_agent_model_name(NULL) == NULL);

    aegis_coding_agent_destroy(agent);
    printf("ALL_CODING_AGENT_TESTS PASSED\n");
    return 0;
}
