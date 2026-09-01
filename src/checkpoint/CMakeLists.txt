# ── src/checkpoint/CMakeLists.txt ──────────────────────────────────────
add_library(aegis_checkpoint checkpoint.c)
target_include_directories(aegis_checkpoint PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>
                                    PRIVATE ${PROJECT_SOURCE_DIR}/src/internal ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(aegis_checkpoint PUBLIC aegis_task aegis_common)
aegis_set_warnings(aegis_checkpoint)
