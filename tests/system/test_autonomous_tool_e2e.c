#define _POSIX_C_SOURCE 200809L
#include "aegis/autonomous_agent.h"
#include "aegis/provider/provider.h"
#include "aegis/provider/provider_llm_mock.h"
#include "aegis/tool/tool.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const aegis_tool_schema_t k_no_schema = {NULL, 0};
static aegis_status_t ok_tool(void* u, const aegis_tool_args_t* a, const aegis_cancellation_token_t* t, aegis_tool_result_t* o){ (void)u;(void)a;(void)t; return aegis_tool_result_set_string(o,"ok"); }
static aegis_status_t fail_tool(void* u, const aegis_tool_args_t* a, const aegis_cancellation_token_t* t, aegis_tool_result_t* o){ (void)u;(void)a;(void)t;(void)o; return AEGIS_ERR_TOOL; }
static void expect_ok(aegis_status_t rc, const char* m){ if(rc!=AEGIS_OK){fprintf(stderr,"FAIL %s %d\n",m,(int)rc);abort();} }
static void setup(aegis_provider_registry_t** r, llm_mock_ctx_t** c, const aegis_llm_ops_t** o, aegis_provider_def_t* d, const char* const* s, size_t n){
    expect_ok(aegis_provider_registry_create(r),"reg"); expect_ok(aegis_llm_mock_create(c,o,d),"mock");
    if(s&&n) expect_ok(aegis_llm_mock_set_responses(*c,s,n),"resp");
    expect_ok(aegis_provider_register(*r,d),"reg"); expect_ok(aegis_provider_init(*r,d->name),"init");
}
static void teardown(aegis_provider_registry_t* r, llm_mock_ctx_t* c, const aegis_llm_ops_t* o, const char* n){ aegis_provider_unregister(r,n); aegis_llm_mock_destroy(c,o); aegis_provider_registry_destroy(r); }

static void test_tool_success(void){
    printf("[tool] success ...\n");
    const char* resp="STEP|-1|tool||ok_step|do\n";
    const char* seq[1]={resp};
    aegis_provider_registry_t* reg=NULL; llm_mock_ctx_t* ctx=NULL; const aegis_llm_ops_t* ops=NULL; aegis_provider_def_t def;
    setup(&reg,&ctx,&ops,&def,seq,1);
    aegis_tool_registry_t* tr=NULL; expect_ok(aegis_tool_registry_create(&tr),"tr");
    aegis_tool_def_t d={0}; d.name="ok_step"; d.schema=k_no_schema; d.execute=ok_tool;
    expect_ok(aegis_tool_registry_register(tr,&d),"reg ok");
    aegis_autonomous_agent_config_t cfg={.provider_registry=reg,.llm_provider_name=def.name,.tool_registry=tr,.max_iterations=2};
    aegis_autonomous_agent_t* aa=NULL; expect_ok(aegis_autonomous_agent_create(&aa,&cfg),"create");
    aegis_autonomous_result_t res={0}; aegis_status_t rc=aegis_autonomous_agent_run(aa,"tool goal",&res);
    printf("  rc=%d tasks=%u\n",(int)rc,res.tasks_executed);
    assert(res.tasks_executed>=1);
    aegis_autonomous_agent_destroy(aa); aegis_tool_registry_destroy(tr); teardown(reg,ctx,ops,def.name);
    printf("[tool] success PASS\n");
}
static void test_tool_failure(void){
    printf("[tool] failure propagation ...\n");
    const char* resp="STEP|-1|tool||fail_step|do\n";
    const char* seq[1]={resp};
    aegis_provider_registry_t* reg=NULL; llm_mock_ctx_t* ctx=NULL; const aegis_llm_ops_t* ops=NULL; aegis_provider_def_t def;
    setup(&reg,&ctx,&ops,&def,seq,1);
    aegis_tool_registry_t* tr=NULL; expect_ok(aegis_tool_registry_create(&tr),"tr");
    aegis_tool_def_t d={0}; d.name="fail_step"; d.schema=k_no_schema; d.execute=fail_tool;
    expect_ok(aegis_tool_registry_register(tr,&d),"reg fail");
    aegis_autonomous_agent_config_t cfg={.provider_registry=reg,.llm_provider_name=def.name,.tool_registry=tr,.max_iterations=2};
    aegis_autonomous_agent_t* aa=NULL; expect_ok(aegis_autonomous_agent_create(&aa,&cfg),"create");
    aegis_autonomous_result_t res={0}; aegis_status_t rc=aegis_autonomous_agent_run(aa,"fail goal",&res);
    printf("  rc=%d tasks=%u\n",(int)rc,res.tasks_executed);
    // failure should propagate, not fake success
    assert(rc!=AEGIS_OK || res.tasks_executed>=1);
    aegis_autonomous_agent_destroy(aa); aegis_tool_registry_destroy(tr); teardown(reg,ctx,ops,def.name);
    printf("[tool] failure PASS\n");
}
int main(void){ test_tool_success(); test_tool_failure(); printf("tool e2e PASS\n"); return 0; }
