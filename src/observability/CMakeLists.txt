# ── src/observability/CMakeLists.txt ───────────────────────────────────
add_library(aegis_observability log.c metrics.c trace.c)
target_include_directories(aegis_observability PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>
                                          PRIVATE ${PROJECT_SOURCE_DIR}/src/internal ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(aegis_observability PUBLIC aegis_common)
aegis_set_warnings(aegis_observability)
