# ── AegisLibraries.cmake ─────────────────────────────────────────────────
# Umbrella INTERFACE targets for backward-compat builds and link ordering.
# Actual library definitions live in src/<module>/CMakeLists.txt.

# workflow umbrella: aggregates all workflow sub-libs
add_library(aegis_workflow INTERFACE)
target_link_libraries(aegis_workflow INTERFACE
    aegis_task
    aegis_scheduler
    aegis_executor
    aegis_planner
    aegis_memory
    aegis_checkpoint
    aegis_security
    aegis_observability
    aegis_plugin
    aegis_storage
    aegis_critic
    aegis_autonomous
)

# core umbrella: top-level, keeps backward compat for tests/apps that link aegis_core
add_library(aegis_core INTERFACE)
target_link_libraries(aegis_core INTERFACE
    aegis_common
    aegis_runtime
    aegis_event
    aegis_provider
    aegis_tool
    aegis_message
    aegis_model
    aegis_session
    aegis_context
    aegis_task
    aegis_scheduler
    aegis_executor
    aegis_planner
    aegis_memory
    aegis_checkpoint
    aegis_security
    aegis_observability
    aegis_plugin
    aegis_storage
    aegis_critic
    aegis_autonomous
    aegis_agent
    aegis_coding
    aegis_workflow
)
