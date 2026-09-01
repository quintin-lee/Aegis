# ── src/task/CMakeLists.txt ────────────────────────────────────────────
add_library(aegis_task task.c graph.c dependency.c)
target_include_directories(aegis_task PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>
                                 PRIVATE ${PROJECT_SOURCE_DIR}/src/internal ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(aegis_task PUBLIC aegis_common)
aegis_set_warnings(aegis_task)
