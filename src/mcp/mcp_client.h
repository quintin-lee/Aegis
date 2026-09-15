/**
 * @file mcp_client.h
 * @brief Public MCP client: discover and call remote tools over an MCP server.
 *
 * Spawns an MCP server subprocess (JSON-RPC 2.0 over stdio), discovers its
 * tools via "tools/list", and wraps each remote tool as an aegis tool
 * definition that can be registered into a coding-agent tool registry.
 */
#ifndef AEGIS_MCP_CLIENT_H
#define AEGIS_MCP_CLIENT_H

#include "aegis/types.h"
#include "aegis/tool/tool.h"
#include "mcp_json.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Opaque MCP client. Owns the server subprocess and discovered tool state. */
typedef struct aegis_mcp_client aegis_mcp_client_t;

/**
 * @brief A remote tool discovered from an MCP server.
 *
 * @p input_schema is a DOM object (the tool's "inputSchema") owned by the
 * client that produced it; do not free it separately.
 */
typedef struct aegis_mcp_tool_info {
    char                name[128];
    char                description[256];
    aegis_json_value_t* input_schema;
} aegis_mcp_tool_info_t;

/**
 * @brief Connect to an MCP server and complete the initialize handshake.
 *
 * @param[in]  server_cmd  Executable path to the MCP server.
 * @param[in]  server_argv NULL-terminated argv (without the program name), or NULL.
 * @param[out] out         Receives the owned client on success.
 * @return AEGIS_OK on success, AEGIS_ERR_INVALID on NULL server_cmd/out,
 *   AEGIS_ERR_PROVIDER on spawn/handshake failure, AEGIS_ERR_NOMEM on allocation.
 */
aegis_status_t aegis_mcp_client_create(const char* server_cmd, const char* const* server_argv,
                                       aegis_mcp_client_t** out);

/**
 * @brief Terminate the server, freeing the subprocess and all tool state.
 *
 * @param[in] c Client to destroy, or NULL.
 *
 * Ownership: any tool definitions registered via aegis_mcp_register_tools()
 * borrow strings from this client, so the caller must destroy the tool
 * registry (or unregister the tools) BEFORE calling this.
 */
void aegis_mcp_client_destroy(aegis_mcp_client_t* c);

/**
 * @brief Discover the server's tools via "tools/list".
 *
 * @param[out] out_count Receives the tool count.
 * @param[out] out_tools  Receives an array of @p *out_count tool infos, owned
 *   by the client (valid until aegis_mcp_client_destroy).
 * @return AEGIS_OK, AEGIS_ERR_INVALID, AEGIS_ERR_PROVIDER, AEGIS_ERR_NOMEM.
 */
aegis_status_t aegis_mcp_list_tools(aegis_mcp_client_t* c, size_t* out_count,
                                    aegis_mcp_tool_info_t** out_tools);

/**
 * @brief Call a remote tool via "tools/call".
 *
 * @param[in]  name  Remote tool name.
 * @param[in]  args  Call arguments (NULL = none).
 * @param[out] out   Result to fill on success.
 * @return AEGIS_OK on success (the result payload is set even when the remote
 *   tool reports isError; callers surface the text), AEGIS_ERR_PROVIDER on
 *   transport failure, AEGIS_ERR_INVALID, AEGIS_ERR_NOMEM.
 */
aegis_status_t aegis_mcp_call_tool(aegis_mcp_client_t* c, const char* name,
                                   const aegis_tool_args_t* args, aegis_tool_result_t* out);

/**
 * @brief Register all discovered remote tools into @p reg as aegis tool defs.
 *
 * Discovers tools (via aegis_mcp_list_tools) and registers one aegis tool
 * per remote tool. The aegis tool definitions borrow strings owned by the
 * client, so @p c must outlive the registration in @p reg.
 *
 * @param[out] reg  Target registry (borrowed; not owned).
 * @return AEGIS_OK on success, else the discovery/registration error.
 */
aegis_status_t aegis_mcp_register_tools(aegis_mcp_client_t* c, aegis_tool_registry_t* reg);

#ifdef __cplusplus
}
#endif

#endif /* AEGIS_MCP_CLIENT_H */
