# ── src/mcp/CMakeLists.txt ──────────────────────────────────────────────
add_library(aegis_json STATIC mcp_json.c)
target_include_directories(aegis_json PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src/mcp>)
target_link_libraries(aegis_json PUBLIC aegis_common)
aegis_set_warnings(aegis_json)

if(AEGIS_MCP)
    add_library(aegis_mcp STATIC mcp_stdio.c mcp_client.c)
    target_include_directories(aegis_mcp PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src/mcp>)
    target_link_libraries(aegis_mcp PUBLIC aegis_json aegis_tool aegis_common)
    aegis_set_warnings(aegis_mcp)
endif()
