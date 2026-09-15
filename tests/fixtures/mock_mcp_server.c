/**
 * @file mock_mcp_server.c
 * @brief Minimal mock Model Context Protocol server over stdio.
 *
 * Reads one JSON-RPC 2.0 message per line from stdin and writes a single-line
 * JSON-RPC response to stdout. Implements just enough of the protocol for the
 * aegis_mcp_client test: initialize, tools/list (one "echo" tool) and
 * tools/call (echoes back the "text" argument). Notifications (messages with
 * no "id") receive no response.
 */
#include "mcp_json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Send a JSON-RPC result response for the given request id + JSON payload. */
static void respond(uint64_t id, const char* payload)
{
    printf("{\"jsonrpc\":\"2.0\",\"id\":%llu,\"result\":%s}\n", (unsigned long long)id, payload);
    fflush(stdout);
}

int main(void)
{
    char line[8192];
    while (fgets(line, sizeof(line), stdin)) {
        aegis_json_value_t* msg = NULL;
        if (aegis_json_parse(line, &msg) != AEGIS_OK || !msg) {
            continue; /* ignore malformed input */
        }
        /* Notification (no "id") gets no response. */
        const aegis_json_value_t* id = aegis_json_object_get(msg, "id");
        if (!id) {
            aegis_json_value_destroy(msg);
            continue;
        }
        const aegis_json_value_t* method = aegis_json_object_get(msg, "method");
        const char*               mname  = method ? aegis_json_string(method) : NULL;
        const aegis_json_value_t* params = aegis_json_object_get(msg, "params");

        if (mname && strcmp(mname, "initialize") == 0) {
            respond(aegis_json_int(id),
                    "{\"protocolVersion\":\"2024-11-05\",\"capabilities\":{\"tools\":{}}}");
        } else if (mname && strcmp(mname, "tools/list") == 0) {
            respond(aegis_json_int(id),
                    "{\"tools\":[{\"name\":\"echo\",\"description\":\"echo tool\","
                    "\"inputSchema\":{\"type\":\"object\","
                    "\"properties\":{\"text\":{\"type\":\"string\"}},\"required\":[\"text\"]}}]}");
        } else if (mname && strcmp(mname, "tools/call") == 0) {
            const aegis_json_value_t* args = aegis_json_object_get(params, "arguments");
            const aegis_json_value_t* text = args ? aegis_json_object_get(args, "text") : NULL;
            const char*               tstr = text ? aegis_json_string(text) : "";
            char                      body[512];
            snprintf(body, sizeof(body),
                     "{\"content\":[{\"type\":\"text\",\"text\":\"echo: %s\"}],\"isError\":false}",
                     tstr ? tstr : "");
            respond(aegis_json_int(id), body);
        }
        aegis_json_value_destroy(msg);
    }
    return 0;
}
