/**
 * @file test_mcp_client.c
 * @brief End-to-end test for the MCP client against a mock stdio server.
 *
 * Spawns the mock_mcp_server binary (path from MOCK_MCP_SERVER env var) and
 * verifies: tool discovery (list_tools), direct invocation (call_tool), and
 * registry-bridged invocation (register_tools + aegis_tool_call).
 */
#define _POSIX_C_SOURCE 200809L
#include "mcp_client.h"
#include "aegis/tool/tool.h"
#include "aegis/common/cancellation/cancellation.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void)
{
    const char* server = getenv("MOCK_MCP_SERVER");
    assert(server != NULL);
    const char* argv[2] = {server, NULL};

    aegis_mcp_client_t* c  = NULL;
    aegis_status_t      st = aegis_mcp_client_create(server, argv, &c);
    assert(st == AEGIS_OK && c != NULL);

    /* ── Tool discovery ─────────────────────────────────────────────── */
    size_t                 n     = 0;
    aegis_mcp_tool_info_t* tools = NULL;
    st                           = aegis_mcp_list_tools(c, &n, &tools);
    assert(st == AEGIS_OK);
    assert(n == 1);
    assert(strcmp(tools[0].name, "echo") == 0);
    assert(tools[0].input_schema != NULL);

    /* ── Register tools (populates per-tool param-spec blobs) ─────────── */
    aegis_tool_registry_t* reg = NULL;
    assert(aegis_tool_registry_create(&reg) == AEGIS_OK);
    st = aegis_mcp_register_tools(c, reg);
    assert(st == AEGIS_OK);
    assert(aegis_tool_registry_count(reg) == 1);

    /* ── Direct invocation via aegis_mcp_call_tool ──────────────────── */
    aegis_tool_args_t* args = NULL;
    assert(aegis_tool_args_create(&args) == AEGIS_OK);
    assert(aegis_tool_args_add_string(args, "text", "hello") == AEGIS_OK);
    aegis_tool_result_t result = {0};
    st                         = aegis_mcp_call_tool(c, "echo", args, &result);
    assert(st == AEGIS_OK);
    assert(result.value.type == AEGIS_TOOL_VAL_STRING);
    assert(strcmp(result.value.as.str.ptr, "echo: hello") == 0);
    aegis_tool_result_destroy(&result);
    aegis_tool_args_destroy(args);

    /* ── Registry-bridged invocation via aegis_tool_call ────────────── */
    aegis_tool_args_t* args2 = NULL;
    assert(aegis_tool_args_create(&args2) == AEGIS_OK);
    assert(aegis_tool_args_add_string(args2, "text", "world") == AEGIS_OK);
    aegis_tool_result_t result2 = {0};
    st = aegis_tool_call(reg, "echo", args2, /*timeout_ms*/ 5000, &result2);
    assert(st == AEGIS_OK);
    assert(result2.value.type == AEGIS_TOOL_VAL_STRING);
    assert(strcmp(result2.value.as.str.ptr, "echo: world") == 0);
    aegis_tool_result_destroy(&result2);
    aegis_tool_args_destroy(args2);

    aegis_tool_registry_destroy(reg);
    aegis_mcp_client_destroy(c);
    printf("All mcp client tests PASS\n");
    return 0;
}
