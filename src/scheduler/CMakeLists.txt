# ── src/scheduler/CMakeLists.txt ───────────────────────────────────────
add_library(aegis_scheduler scheduler.c)
target_include_directories(aegis_scheduler PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>
                                      PRIVATE ${PROJECT_SOURCE_DIR}/src/internal ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(aegis_scheduler PUBLIC aegis_task aegis_common)
aegis_set_warnings(aegis_scheduler)
