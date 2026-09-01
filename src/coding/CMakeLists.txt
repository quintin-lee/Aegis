# ── src/coding/CMakeLists.txt ──────────────────────────────────────────
add_library(aegis_coding mutations.c coding_tools.c coding_agent.c)
target_include_directories(aegis_coding PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>
                                   PRIVATE ${PROJECT_SOURCE_DIR}/src/internal ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(aegis_coding PUBLIC aegis_agent aegis_tool aegis_session aegis_common aegis_skill)
