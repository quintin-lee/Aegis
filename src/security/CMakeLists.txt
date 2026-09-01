# ── src/security/CMakeLists.txt ────────────────────────────────────────
add_library(aegis_security security.c)
target_include_directories(aegis_security PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>
                                 PRIVATE ${PROJECT_SOURCE_DIR}/src/internal ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(aegis_security PUBLIC aegis_tool aegis_common)
aegis_set_warnings(aegis_security)
