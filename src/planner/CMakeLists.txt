# ── src/planner/CMakeLists.txt ─────────────────────────────────────────
add_library(aegis_planner
    plan.c planner.c
    ${PROJECT_SOURCE_DIR}/src/strategy/strategy_registry.c
    ${PROJECT_SOURCE_DIR}/src/strategy/autonomous_strategy.c
    ${PROJECT_SOURCE_DIR}/strategies/plan_execute/plan_execute.c
)
target_include_directories(aegis_planner PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>
                                    PRIVATE ${PROJECT_SOURCE_DIR}/src/internal ${PROJECT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/providers ${PROJECT_SOURCE_DIR}/strategies)
target_link_libraries(aegis_planner PUBLIC aegis_provider aegis_task aegis_common)
aegis_set_warnings(aegis_planner)
