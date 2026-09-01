# ── src/event/CMakeLists.txt ───────────────────────────────────────────
add_library(aegis_event event.c event_bus.c)
target_include_directories(aegis_event PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>
                                  PRIVATE ${PROJECT_SOURCE_DIR}/src/internal ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(aegis_event PUBLIC aegis_common)
aegis_set_warnings(aegis_event)
