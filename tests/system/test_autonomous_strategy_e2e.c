/**
 * @file test_autonomous_strategy_e2e.c
 * @brief E2E AutonomousStrategy inside the reactive loop: goal flows from
 * the loop session through plan, execute and critic to completion.
 */
#define _POSIX_C_SOURCE 200809L
#include "aegis/strategy/autonomous_strategy.h"
#include "aegis/autonomous_agent.h"
#include "aegis/agent/loop.h"
#include "aegis/session/session.h"
#include "aegis/model/model.h"
#include "aegis/provider/provider_llm_mock.h"
#include "aegis/provider/provider.h"
#include "aegis/provider/llm.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static aegis_status_t text_backend_stream(void* user, const aegis_model_request_t* req,
                                          const aegis_cancellation_token_t* tok,
                                          aegis_model_stream_callback_fn cb, void* cbuser)
{
    const char* text = "working";
    (void)user;
    (void)req;
    (void)tok;
    aegis_model_stream_event_t ev = {
        .type = AEGIS_MODEL_STREAM_TEXT_DELTA,
        .data = text,
        .len  = strlen(text),
    };
    if (cb(&ev, cbuser) != AEGIS_OK) {
        return AEGIS_ERR_INTERNAL;
    }
    aegis_model_stream_event_t end = {.type = AEGIS_MODEL_STREAM_END};
    return cb(&end, cbuser);
}

static aegis_model_backend_t text_backend = {
    .user         = NULL,
    .complete     = NULL,
    .stream       = text_backend_stream,
    .capabilities = AEGIS_MODEL_CAP_TEXT | AEGIS_MODEL_CAP_STREAMING,
};

int main(void)
{
    /* Provider side: canned computational plan, one task. */
    const char*                resp = "STEP|-1|computational||compute|perform computation\n";
    const char*                seq[1] = {resp};
    aegis_provider_registry_t* preg   = NULL;
    llm_mock_ctx_t*            pctx   = NULL;
    const aegis_llm_ops_t*     pops   = NULL;
    aegis_provider_def_t       pdef;
    assert(aegis_provider_registry_create(&preg) == AEGIS_OK);
    assert(aegis_llm_mock_create(&pctx, &pops, &pdef) == AEGIS_OK);
    assert(aegis_llm_mock_set_responses(pctx, seq, 1) == AEGIS_OK);
    assert(aegis_provider_register(preg, &pdef) == AEGIS_OK);
    assert(aegis_provider_init(preg, pdef.name) == AEGIS_OK);

    aegis_autonomous_agent_config_t acost = {
        .provider_registry = preg,
        .llm_provider_name = pdef.name,
        .max_iterations    = 3,
    };
    aegis_autonomous_strategy_t* strat = NULL;
    assert(aegis_autonomous_strategy_create(&acost, &strat) == AEGIS_OK);
    const aegis_agent_strategy_def_t* def = aegis_autonomous_strategy_def(strat);
    assert(def != NULL);

    /* Loop side: session + text-only model + strategy. */
    aegis_session_t*      session = NULL;
    aegis_model_client_t* model   = NULL;
    aegis_agent_loop_t*   loop    = NULL;
    assert(aegis_session_create(".", &session) == AEGIS_OK);
    assert(aegis_model_client_create_with_backend("fixture-auto-strat", &text_backend, &model) ==
           AEGIS_OK);
    aegis_agent_loop_config_t lcfg = {
        .session       = session,
        .model         = model,
        .tools         = NULL,
        .strategy      = def,
    };
    assert(aegis_agent_loop_create(&lcfg, &loop) == AEGIS_OK);

    /* Goal flows loop session -> strategy -> autonomous plan/execute/evaluate. */
    assert(aegis_agent_loop_run_turn(loop, "compute") == AEGIS_OK);
    assert(aegis_agent_loop_state(loop) == AEGIS_AGENT_LOOP_COMPLETED);
    printf("autonomous_strategy_e2e PASS\n");

    aegis_agent_loop_destroy(loop);
    aegis_model_client_destroy(model);
    aegis_session_destroy(session);
    aegis_autonomous_strategy_destroy(strat);
    aegis_provider_unregister(preg, pdef.name);
    aegis_llm_mock_destroy(pctx, pops);
    aegis_provider_registry_destroy(preg);
    printf("ALL_AUTONOMOUS_STRATEGY_E2E_PASSED\n");
    return 0;
}
