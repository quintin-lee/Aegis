# ── src/extension/CMakeLists.txt ───────────────────────────────────────
add_library(aegis_extension extension.c)
target_include_directories(aegis_extension PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>
                                 PRIVATE ${PROJECT_SOURCE_DIR}/src/internal ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(aegis_extension PUBLIC aegis_common)
aegis_set_warnings(aegis_extension)
