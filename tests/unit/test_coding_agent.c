#define _POSIX_C_SOURCE 200809L
#include "aegis/coding/coding_agent.h"
#include "aegis/session/session.h"
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

static aegis_tool_approval_t deny_all_fwd(const char* n, const char* a, void* u)
{
    (void)n;
    (void)a;
    (void)u;
    return AEGIS_TOOL_APPROVAL_DENY;
}

/* Event observer: count events and remember whether text was streamed. */
typedef struct ev_log {
    size_t count;
    size_t text_deltas;
    char   last_text[512];
} ev_log_t;

static void ev_cb(const aegis_agent_event_t* ev, void* user)
{
    ev_log_t* log = (ev_log_t*)user;
    log->count++;
    if (ev->type == AEGIS_AGENT_EVENT_TEXT_DELTA && ev->data && ev->len) {
        log->text_deltas++;
        /* Append the fragment so the reassembled stream stays inspectable. */
        size_t room = sizeof(log->last_text) - 1 - strlen(log->last_text);
        size_t take = ev->len < room ? ev->len : room;
        strncat(log->last_text, (const char*)ev->data, take);
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
    assert(aegis_coding_agent_set_event_callback(NULL, NULL, NULL) == AEGIS_ERR_INVALID);

    aegis_coding_agent_destroy(agent);

    /* Event observer pass-through: register -> run -> observe -> clear */
    aegis_coding_agent_t* agent2 = NULL;
    expect_ok(aegis_coding_agent_create(&cfg, &agent2), "create2");
    ev_log_t log = {0};
    expect_ok(aegis_coding_agent_set_event_callback(agent2, ev_cb, &log), "set_event");
    expect_ok(aegis_coding_agent_run(agent2, "stream me"), "run with events");
    assert(log.count > 0);
    assert(log.text_deltas > 0);
    assert(strstr(log.last_text, "mock stream for:") != NULL);

    /* Clearing the callback stops events */
    expect_ok(aegis_coding_agent_set_event_callback(agent2, NULL, NULL), "clear event");
    size_t after = log.count;
    expect_ok(aegis_coding_agent_run(agent2, "stream again"), "run without events");
    assert(log.count == after);

    aegis_coding_agent_destroy(agent2);

    /* Approval gate pass-through: the gate receives verdicts through rebuilds. */
    aegis_coding_agent_t* agent3 = NULL;
    expect_ok(aegis_coding_agent_create(&cfg, &agent3), "create3");
    assert(aegis_coding_agent_set_tool_approval(NULL, deny_all_fwd, NULL) == AEGIS_ERR_INVALID);
    expect_ok(aegis_coding_agent_set_tool_approval(agent3, deny_all_fwd, NULL), "set approval");
    expect_ok(aegis_coding_agent_run(agent3, "denied run"), "run under deny gate");
    /* Switching model rebuilds the loop; the gate must survive. */
    expect_ok(aegis_coding_agent_set_model(agent3, "gpt-x"), "switch with gate");
    expect_ok(aegis_coding_agent_run(agent3, "denied run 2"), "run after rebuild");
    expect_ok(aegis_coding_agent_set_tool_approval(agent3, NULL, NULL), "clear approval");
    aegis_coding_agent_destroy(agent3);
    printf("ALL_CODING_AGENT_TESTS PASSED\n");
    return 0;
}
