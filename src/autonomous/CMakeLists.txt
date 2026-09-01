# ── src/autonomous/CMakeLists.txt ──────────────────────────────────────
add_library(aegis_autonomous
    autonomous_state.c
    autonomous_agent.c
    memory_mock.c
    state_machine.c lifecycle.c checkpoint.c recovery.c
    planning.c execution.c evaluation.c reflection.c replanning.c loop.c
    src/shell.c src/status.c
)
target_include_directories(aegis_autonomous PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>
                                      PRIVATE ${PROJECT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/src/internal ${PROJECT_SOURCE_DIR}/src/autonomous ${PROJECT_SOURCE_DIR}/providers ${PROJECT_SOURCE_DIR}/strategies)
target_link_libraries(aegis_autonomous PUBLIC aegis_planner aegis_scheduler aegis_executor aegis_task aegis_checkpoint aegis_security aegis_critic aegis_common aegis_planner PRIVATE m pthread)
aegis_set_warnings(aegis_autonomous)
