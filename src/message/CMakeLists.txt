# ── src/message/CMakeLists.txt ─────────────────────────────────────────
add_library(aegis_message role.c tool_call.c tool_result.c message.c)
target_include_directories(aegis_message PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>
                                    PRIVATE ${PROJECT_SOURCE_DIR}/src/internal ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(aegis_message PUBLIC aegis_common)
aegis_set_warnings(aegis_message)
