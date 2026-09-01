# ── src/executor/CMakeLists.txt ────────────────────────────────────────
add_library(aegis_executor executor.c)
target_include_directories(aegis_executor PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>
                                 PRIVATE ${PROJECT_SOURCE_DIR}/src/internal ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(aegis_executor PUBLIC aegis_task aegis_common PRIVATE m pthread)
aegis_set_warnings(aegis_executor)
