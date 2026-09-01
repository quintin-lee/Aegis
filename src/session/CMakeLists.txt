# ── src/session/CMakeLists.txt ─────────────────────────────────────────
add_library(aegis_session session.c)
target_include_directories(aegis_session PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>
                                    PRIVATE ${PROJECT_SOURCE_DIR}/src/internal ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(aegis_session PUBLIC aegis_message aegis_common)
aegis_set_warnings(aegis_session)
