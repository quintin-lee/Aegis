# ── src/model/CMakeLists.txt ───────────────────────────────────────────
add_library(aegis_model response.c stream.c model.c)
target_include_directories(aegis_model PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>
                                 PRIVATE ${PROJECT_SOURCE_DIR}/src/internal ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(aegis_model PUBLIC aegis_message aegis_tool aegis_common)
aegis_set_warnings(aegis_model)
