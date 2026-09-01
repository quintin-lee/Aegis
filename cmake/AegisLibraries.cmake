# ── AegisLibraries.cmake ─────────────────────────────────────────────────
# Fine-grained library split. Dependency order:
#   common → runtime → provider → tool → message → model → session → context → agent → coding → workflow
#   workflow (planner/task/scheduler/executor) is the heaviest, but optional for coding agent.
#   aegis_core is kept as an INTERFACE umbrella for backward compat.

# ── libaegis_common (already defined in main, but we ensure its properties)
# (defined in top-level before including this file)

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

# ── libaegis_event
add_library(aegis_event
    src/event/event.c
    src/event/event_bus.c
)
target_include_directories(aegis_event PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>
                                   PRIVATE ${PROJECT_SOURCE_DIR}/src/internal ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(aegis_event PUBLIC aegis_common)
aegis_set_warnings(aegis_event)

# ── libaegis_agent
add_library(aegis_agent
    src/agent/agent.c
    src/agent/goal.c
    src/agent/loop.c
    src/agent/state.c
)
target_include_directories(aegis_agent PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>
                                  PRIVATE ${PROJECT_SOURCE_DIR}/src/internal ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(aegis_agent PUBLIC aegis_session aegis_model aegis_context aegis_tool aegis_event aegis_common)
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

# ── libaegis_workflow (planner + task + scheduler + executor + memory + critic + checkpoint + plugin + observability + security + storage)
add_library(aegis_workflow
    src/task/task.c
    src/task/graph.c
    src/task/dependency.c
    src/scheduler/scheduler.c
    src/executor/executor.c
    src/planner/plan.c
    src/planner/planner.c
    src/strategy/strategy_registry.c
    src/strategy/autonomous_strategy.c
    src/memory/memory.c
    src/memory/memory_working.c
    src/memory/memory_episodic.c
    src/memory/memory_semantic.c
    src/memory/memory_procedural.c
    src/critic/critic.c
    src/reflection/reflection.c
    src/replanner/replanner.c
    src/storage/storage.c
    src/storage/storage_store.c
    src/checkpoint/checkpoint.c
    src/security/security.c
    src/plugin/plugin.c
    src/observability/log.c
    src/observability/metrics.c
    src/observability/trace.c
    strategies/plan_execute/plan_execute.c
    src/shell.c
    src/status.c
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
)
target_include_directories(aegis_workflow PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>
                                     PRIVATE ${PROJECT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/src/internal ${PROJECT_SOURCE_DIR}/src/autonomous ${PROJECT_SOURCE_DIR}/providers ${PROJECT_SOURCE_DIR}/strategies)
target_link_libraries(aegis_workflow PUBLIC aegis_provider aegis_tool aegis_agent aegis_context aegis_session aegis_message aegis_event aegis_common PRIVATE sqlite3 m)
aegis_set_warnings(aegis_workflow)

# ── libaegis_core (umbrella, keeps backward compat)
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
    aegis_agent
    aegis_coding
    aegis_workflow
)
