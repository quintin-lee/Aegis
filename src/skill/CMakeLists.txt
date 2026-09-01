# ── src/skill/CMakeLists.txt ───────────────────────────────────────────
add_library(aegis_skill skill.c registry.c loader.c)
target_include_directories(aegis_skill PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>
                                 PRIVATE ${PROJECT_SOURCE_DIR}/src/internal ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(aegis_skill PUBLIC aegis_common)
aegis_set_warnings(aegis_skill)
