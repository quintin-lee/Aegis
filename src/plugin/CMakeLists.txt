# ── src/plugin/CMakeLists.txt ──────────────────────────────────────────
add_library(aegis_plugin plugin.c)
target_include_directories(aegis_plugin PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>
                                 PRIVATE ${PROJECT_SOURCE_DIR}/src/internal ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(aegis_plugin PUBLIC aegis_common PRIVATE dl)
aegis_set_warnings(aegis_plugin)
