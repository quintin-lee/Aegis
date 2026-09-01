# ── src/agent/CMakeLists.txt ───────────────────────────────────────────
add_library(aegis_agent agent.c goal.c loop.c state.c)
target_include_directories(aegis_agent PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>
                                 PRIVATE ${PROJECT_SOURCE_DIR}/src/internal ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(aegis_agent PUBLIC aegis_session aegis_model aegis_context aegis_tool aegis_event aegis_runtime aegis_common)
aegis_set_warnings(aegis_agent)
