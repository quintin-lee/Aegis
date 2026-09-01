# ── AegisLibraries.cmake ─────────────────────────────────────────────────
# Fine-grained library split.
# Dependency order:
#   common → event → runtime → provider → tool → message → model → session → context → task → scheduler → executor → planner → memory → checkpoint → security → observability → plugin → storage → critic → autonomous → agent → coding → workflow(core umbrella)
#   workflow is now split into sub-libs; aegis_workflow is kept as INTERFACE umbrella for backward compat.
#   aegis_core is the top INTERFACE that links everything.

# ── libaegis_common (defined in top-level)

# ── libaegis_event
add_library(aegis_event
    src/event/event.c
    src/event/event_bus.c
)
target_include_directories(aegis_event PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>
                                   PRIVATE ${PROJECT_SOURCE_DIR}/src/internal ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(aegis_event PUBLIC aegis_common)
aegis_set_warnings(aegis_event)

# ── libaegis_runtime
add_library(aegis_runtime
    src/runtime/runtime.c
    src/runtime/config.c
)
target_include_directories(aegis_runtime PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>
                                      PRIVATE ${PROJECT_SOURCE_DIR}/src/internal ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(aegis_runtime PUBLIC aegis_common PRIVATE m pthread)
aegis_set_warnings(aegis_runtime)

# ── libaegis_provider
add_library(aegis_provider
    src/provider/provider.c
    src/provider/provider_registry.c
    src/provider/llm.c
    src/provider/embedding.c
    providers/llm/mock/llm_mock.c
    providers/embedding/embedding_hash.c
    providers/storage/sqlite/storage_sqlite.c
)
target_include_directories(aegis_provider PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>
                                         PRIVATE ${PROJECT_SOURCE_DIR}/src/internal ${PROJECT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/providers ${PROJECT_SOURCE_DIR}/strategies)
target_link_libraries(aegis_provider PUBLIC aegis_common PRIVATE sqlite3 m)
aegis_set_warnings(aegis_provider)

# ── libaegis_tool
add_library(aegis_tool
    src/tool/tool.c
    src/tool/tool_call.c
    src/tool/tool_registry.c
    src/tool/tool_schema.c
)
target_include_directories(aegis_tool PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>
                                  PRIVATE ${PROJECT_SOURCE_DIR}/src/internal ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(aegis_tool PUBLIC aegis_common)
aegis_set_warnings(aegis_tool)

# ── libaegis_message
add_library(aegis_message
    src/message/role.c
    src/message/tool_call.c
    src/message/tool_result.c
    src/message/message.c
)
target_include_directories(aegis_message PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>
                                    PRIVATE ${PROJECT_SOURCE_DIR}/src/internal ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(aegis_message PUBLIC aegis_common)
aegis_set_warnings(aegis_message)

# ── libaegis_model
add_library(aegis_model
    src/model/response.c
    src/model/stream.c
    src/model/model.c
)
target_include_directories(aegis_model PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>
                                  PRIVATE ${PROJECT_SOURCE_DIR}/src/internal ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(aegis_model PUBLIC aegis_message aegis_tool aegis_common)
aegis_set_warnings(aegis_model)

# ── libaegis_session
add_library(aegis_session
    src/session/session.c
)
target_include_directories(aegis_session PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>
                                    PRIVATE ${PROJECT_SOURCE_DIR}/src/internal ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(aegis_session PUBLIC aegis_message aegis_common)
aegis_set_warnings(aegis_session)

# ── libaegis_context
add_library(aegis_context
    src/context/context.c
)
target_include_directories(aegis_context PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>
                                    PRIVATE ${PROJECT_SOURCE_DIR}/src/internal ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(aegis_context PUBLIC aegis_message aegis_common)
aegis_set_warnings(aegis_context)

# ── libaegis_task
add_library(aegis_task
    src/task/task.c
    src/task/graph.c
    src/task/dependency.c
)
target_include_directories(aegis_task PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>
                                 PRIVATE ${PROJECT_SOURCE_DIR}/src/internal ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(aegis_task PUBLIC aegis_common)
aegis_set_warnings(aegis_task)

# ── libaegis_scheduler
add_library(aegis_scheduler
    src/scheduler/scheduler.c
)
target_include_directories(aegis_scheduler PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>
                                      PRIVATE ${PROJECT_SOURCE_DIR}/src/internal ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(aegis_scheduler PUBLIC aegis_task aegis_common)
aegis_set_warnings(aegis_scheduler)

# ── libaegis_executor
add_library(aegis_executor
    src/executor/executor.c
)
target_include_directories(aegis_executor PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>
                                     PRIVATE ${PROJECT_SOURCE_DIR}/src/internal ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(aegis_executor PUBLIC aegis_task aegis_common PRIVATE m pthread)
aegis_set_warnings(aegis_executor)

# ── libaegis_planner
add_library(aegis_planner
    src/planner/plan.c
    src/planner/planner.c
    src/strategy/strategy_registry.c
    src/strategy/autonomous_strategy.c
    strategies/plan_execute/plan_execute.c
)
target_include_directories(aegis_planner PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>
                                    PRIVATE ${PROJECT_SOURCE_DIR}/src/internal ${PROJECT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/providers ${PROJECT_SOURCE_DIR}/strategies)
target_link_libraries(aegis_planner PUBLIC aegis_provider aegis_task aegis_common)
aegis_set_warnings(aegis_planner)

# ── libaegis_memory
add_library(aegis_memory
    src/memory/memory.c
    src/memory/memory_working.c
    src/memory/memory_episodic.c
    src/memory/memory_semantic.c
    src/memory/memory_procedural.c
)
target_include_directories(aegis_memory PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>
                                   PRIVATE ${PROJECT_SOURCE_DIR}/src/internal ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(aegis_memory PUBLIC aegis_common)
aegis_set_warnings(aegis_memory)

# ── libaegis_checkpoint
add_library(aegis_checkpoint
    src/checkpoint/checkpoint.c
)
target_include_directories(aegis_checkpoint PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>
                                      PRIVATE ${PROJECT_SOURCE_DIR}/src/internal ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(aegis_checkpoint PUBLIC aegis_task aegis_common)
aegis_set_warnings(aegis_checkpoint)

# ── libaegis_security
add_library(aegis_security
    src/security/security.c
)
target_include_directories(aegis_security PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>
                                     PRIVATE ${PROJECT_SOURCE_DIR}/src/internal ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(aegis_security PUBLIC aegis_tool aegis_common)
aegis_set_warnings(aegis_security)

# ── libaegis_observability
add_library(aegis_observability
    src/observability/log.c
    src/observability/metrics.c
    src/observability/trace.c
)
target_include_directories(aegis_observability PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>
                                          PRIVATE ${PROJECT_SOURCE_DIR}/src/internal ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(aegis_observability PUBLIC aegis_common)
aegis_set_warnings(aegis_observability)

# ── libaegis_plugin
add_library(aegis_plugin
    src/plugin/plugin.c
)
target_include_directories(aegis_plugin PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>
                                   PRIVATE ${PROJECT_SOURCE_DIR}/src/internal ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(aegis_plugin PUBLIC aegis_common PRIVATE dl)
aegis_set_warnings(aegis_plugin)

# ── libaegis_storage
add_library(aegis_storage
    src/storage/storage.c
    src/storage/storage_store.c
)
target_include_directories(aegis_storage PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>
                                    PRIVATE ${PROJECT_SOURCE_DIR}/src/internal ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(aegis_storage PUBLIC aegis_common PRIVATE sqlite3)
aegis_set_warnings(aegis_storage)

# ── libaegis_critic
add_library(aegis_critic
    src/critic/critic.c
    src/reflection/reflection.c
    src/replanner/replanner.c
)
target_include_directories(aegis_critic PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>
                                   PRIVATE ${PROJECT_SOURCE_DIR}/src/internal ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(aegis_critic PUBLIC aegis_task aegis_common)
aegis_set_warnings(aegis_critic)

# ── libaegis_autonomous
add_library(aegis_autonomous
    src/autonomous_state.c
    src/autonomous_agent.c
    src/autonomous/state_machine.c
    src/autonomous/lifecycle.c
    src/autonomous/checkpoint.c
    src/autonomous/recovery.c
    src/autonomous/planning.c
    src/autonomous/execution.c
    src/autonomous/evaluation.c
    src/autonomous/reflection.c
    src/autonomous/replanning.c
    src/autonomous/loop.c
    src/shell.c
)
target_include_directories(aegis_autonomous PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>
                                      PRIVATE ${PROJECT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/src/internal ${PROJECT_SOURCE_DIR}/src/autonomous ${PROJECT_SOURCE_DIR}/providers ${PROJECT_SOURCE_DIR}/strategies)
target_link_libraries(aegis_autonomous PUBLIC aegis_planner aegis_scheduler aegis_executor aegis_task aegis_checkpoint aegis_security aegis_critic aegis_common PRIVATE m pthread)
aegis_set_warnings(aegis_autonomous)

# ── libaegis_agent
add_library(aegis_agent
    src/agent/agent.c
    src/agent/goal.c
    src/agent/loop.c
    src/agent/state.c
)
target_include_directories(aegis_agent PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>
                                  PRIVATE ${PROJECT_SOURCE_DIR}/src/internal ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(aegis_agent PUBLIC aegis_session aegis_model aegis_context aegis_tool aegis_event aegis_runtime aegis_common)
aegis_set_warnings(aegis_agent)

# ── libaegis_coding
add_library(aegis_coding
    src/coding/mutations.c
    src/coding/coding_tools.c
    src/coding/coding_agent.c
    src/skill/skill.c
    src/skill/registry.c
    src/skill/loader.c
    src/extension/extension.c
)
target_include_directories(aegis_coding PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>
                                   PRIVATE ${PROJECT_SOURCE_DIR}/src/internal ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(aegis_coding PUBLIC aegis_agent aegis_tool aegis_session aegis_common)
aegis_set_warnings(aegis_coding)

# ── libaegis_workflow (umbrella for workflow sub-libs, kept for compat)
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

# ── libaegis_core (top umbrella, keeps backward compat for tests and apps)
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
