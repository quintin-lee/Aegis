/**
 * @file mcp_stdio.h
 * @brief MCP stdio transport: spawn an MCP server subprocess and speak
 *        newline-delimited JSON-RPC 2.0 over its stdin/stdout.
 *
 * Internal module (not part of the public aegis API). The public MCP client
 * lives in mcp_client.h.
 */
#ifndef AEGIS_MCP_STDIO_H
#define AEGIS_MCP_STDIO_H

#include "aegis/types.h"
#include "mcp_json.h"
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief An MCP server subprocess bound to a JSON-RPC stdio channel.
 *
 * Opaque; the server is spawned at construction (after running the
 * initialize handshake + "initialized" notification). Destroying the handle
 * terminates the child and frees all allocated state.
 */
typedef struct aegis_mcp_stdio aegis_mcp_stdio_t;

/**
 * @brief Spawn an MCP server and complete the initialize handshake.
 *
 * Forks/execs @p cmd with @p argv (argv is a NULL-terminated array; if
 * @p argv is NULL, @p cmd alone is used). After the child is up, sends the
 * "initialize" request, then the "initialized" notification.
 *
 * @param[out] out  Receives the owned handle on success.
 * @return AEGIS_OK on success, AEGIS_ERR_PROVIDER on fork/exec or handshake
 *   failure, AEGIS_ERR_INVALID on NULL out/cmd, AEGIS_ERR_NOMEM on
 *   allocation failure.
 */
aegis_status_t aegis_mcp_stdio_spawn(const char* cmd, const char* const* argv,
                                     aegis_mcp_stdio_t** out);

/**
 * @brief Terminate the child and free all state.
 *
 * Sends SIGTERM to the process group, waits, and frees the handles and any
 * DOM owned by the transport. Safe with NULL.
 *
 * @param[in] s  Handle to destroy, or NULL.
 */
void aegis_mcp_stdio_destroy(aegis_mcp_stdio_t* s);

/**
 * @brief Send one JSON-RPC request and block for its response.
 *
 * Writes a newline-delimited `{"jsonrpc":"2.0","id":<id>,"method":...,"params":...}`
 * line to the child's stdin, then reads lines from the child's stdout until
 * one whose "id" matches @p id (notifications and other ids are skipped).
 * The "result" of the matched response is parsed into @p *out_result.
 *
 * @param[in]  s         Live transport.
 * @param[in]  id        JSON-RPC request id.
 * @param[in]  method    JSON-RPC method name (non-NULL).
 * @param[in]  params    JSON-RPC params object; NULL when the method takes none.
 * @param[out] out_result Receives the owned "result" DOM on success (or a NULL
 *   value if the response carried no result); free with
 *   aegis_json_value_destroy.
 * @return AEGIS_OK on success, AEGIS_ERR_PROVIDER on child exit / read error /
 *   JSON-RPC error response, AEGIS_ERR_INVALID on NULL method,
 *   AEGIS_ERR_NOMEM on allocation failure.
 */
aegis_status_t aegis_mcp_stdio_request(aegis_mcp_stdio_t* s, uint64_t id, const char* method,
                                       const aegis_json_value_t* params,
                                       aegis_json_value_t**      out_result);

#ifdef __cplusplus
}
#endif

#endif /* AEGIS_MCP_STDIO_H */
