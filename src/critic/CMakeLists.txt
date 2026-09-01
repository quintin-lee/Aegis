# ── src/critic/CMakeLists.txt ──────────────────────────────────────────
add_library(aegis_critic critic.c ../reflection/reflection.c ../replanner/replanner.c)
target_include_directories(aegis_critic PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>
                                   PRIVATE ${PROJECT_SOURCE_DIR}/src/internal ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(aegis_critic PUBLIC aegis_task aegis_common)
aegis_set_warnings(aegis_critic)
