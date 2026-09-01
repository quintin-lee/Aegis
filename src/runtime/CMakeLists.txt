# ── src/runtime/CMakeLists.txt ─────────────────────────────────────────
add_library(aegis_runtime runtime.c config.c)
target_include_directories(aegis_runtime PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>
                                    PRIVATE ${PROJECT_SOURCE_DIR}/src/internal ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(aegis_runtime PUBLIC aegis_common PRIVATE m pthread)
aegis_set_warnings(aegis_runtime)
