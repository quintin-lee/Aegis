# ── src/tool/CMakeLists.txt ────────────────────────────────────────────
add_library(aegis_tool tool.c tool_call.c tool_registry.c tool_schema.c)
target_include_directories(aegis_tool PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>
                                 PRIVATE ${PROJECT_SOURCE_DIR}/src/internal ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(aegis_tool PUBLIC aegis_common)
aegis_set_warnings(aegis_tool)
