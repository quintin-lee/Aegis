cmake_minimum_required(VERSION 3.20)

# ── Version — single source of truth ───────────────────────────────────────
# Read VERSION file BEFORE project() so the value is available for project(VERSION ...)
file(READ "${CMAKE_SOURCE_DIR}/VERSION" AEGIS_VERSION_STRING)
string(STRIP "${AEGIS_VERSION_STRING}" AEGIS_VERSION_STRING)
message(STATUS "aegis ${AEGIS_VERSION_STRING}")

project(aegis
    VERSION ${AEGIS_VERSION_STRING}
)

# Parse into major/minor/patch for version.h generation
string(REPLACE "." ";" _AEGIS_VER_LIST ${AEGIS_VERSION_STRING})
list(GET _AEGIS_VER_LIST 0 AEGIS_VERSION_MAJOR)
list(GET _AEGIS_VER_LIST 1 AEGIS_VERSION_MINOR)
list(GET _AEGIS_VER_LIST 2 AEGIS_VERSION_PATCH)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# ── Options ───────────────────────────────────────────────────────────────────
option(AEGIS_BUILD_TESTS   "Build unit/integration tests"        ON)
option(AEGIS_BUILD_BENCH   "Build benchmark executables"         OFF)
option(AEGIS_BUILD_DOCS    "Generate documentation (Doxygen)"    OFF)
option(AEGIS_FORMAT_CODE "Run clang-format on all sources before build" ON)

# ── Compiler warnings ────────────────────────────────────────────────────────
function(aegis_set_warnings target)
    if(AEGIS_WARNINGS_AS_ERRORS)
        set(_WARN_FLAGS -Wall -Wextra -Wpedantic -Werror)
    else()
        set(_WARN_FLAGS -Wall -Wextra -Wpedantic)
    endif()
    target_compile_options(${target} PRIVATE ${_WARN_FLAGS})
endfunction()

# ── clang-format (pre-build auto-format) ─────────────────────────────────────
# When AEGIS_FORMAT_CODE is ON (default), clang-format runs on every source
# file before compilation. The pre_build_format target ensures format runs
# before any build target; all targets depend on it.
if(AEGIS_FORMAT_CODE)
    find_program(CLANG_FORMAT_PROGRAM clang-format)
    if(CLANG_FORMAT_PROGRAM)
        message(STATUS "clang-format found: ${CLANG_FORMAT_PROGRAM}")

        # Collect all C/C++ source files (re-read on each configure via CONFIGURE_DEPENDS).
        file(GLOB_RECURSE AEGIS_ALL_SOURCES
            CONFIGURE_DEPENDS
            src/*.c
            src/**/*.c
            include/aegis/*.h
            include/aegis/**/*.h
            tests/unit/**/*.c
        )

        # Stamp file in build tree — triggers format only when sources change.
        set(_AEGIS_FORMAT_STAMP "${CMAKE_BINARY_DIR}/.clang-format-stamp")

        add_custom_command(
            OUTPUT "${_AEGIS_FORMAT_STAMP}"
            COMMAND ${CLANG_FORMAT_PROGRAM} -i -style=file ${AEGIS_ALL_SOURCES}
            COMMAND ${CMAKE_COMMAND} -E touch "${_AEGIS_FORMAT_STAMP}"
            COMMENT "Running clang-format on all sources before build"
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            DEPENDS ${AEGIS_ALL_SOURCES}
        )

        # Target that depends on the stamp; all build targets depend on this.
        add_custom_target(pre_build_format DEPENDS "${_AEGIS_FORMAT_STAMP}")

        # Manual re-format target — delete stamp to force re-format next build.
        add_custom_target(format_all
            COMMAND ${CLANG_FORMAT_PROGRAM} -i -style=file ${AEGIS_ALL_SOURCES}
            COMMAND ${CMAKE_COMMAND} -E touch "${_AEGIS_FORMAT_STAMP}"
            COMMENT "Formatting all sources with clang-format"
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        )
    else()
        message(WARNING "clang-format not found; skipping auto-format (disable with -DAEGIS_FORMAT_CODE=OFF)")
    endif()
else()
    message(STATUS "clang-format auto-format disabled (AEGIS_FORMAT_CODE=OFF)")
endif()

# ── libaegis-foundation ─────────────────────────────────────────────────────
add_library(aegis_common
    src/common/error.c
    src/common/result.c
    src/common/allocator.c
    src/common/queue.c
    src/common/hashmap.c
    src/common/buffer.c
    src/common/string.c
    src/common/thread.c
    src/common/mutex.c
    src/common/uuid.c
    src/common/atomic.c
    src/common/vector.c
    src/common/list.c
    src/common/time.c
    src/common/cancellation/cancellation.c
)
target_include_directories(aegis_common
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${PROJECT_SOURCE_DIR}/src/internal
        ${PROJECT_SOURCE_DIR}/providers
        ${PROJECT_SOURCE_DIR}/strategies
)
target_link_libraries(aegis_common PRIVATE m pthread)
aegis_set_warnings(aegis_common)
if(AEGIS_FORMAT_CODE AND CLANG_FORMAT_PROGRAM)
    add_dependencies(aegis_common pre_build_format)
endif()

# ── libaegis-core ────────────────────────────────────────────────────────────
add_library(aegis_core
    # Agent & Runtime
    src/agent/agent.c
    src/agent/goal.c
    src/runtime/runtime.c
    src/runtime/config.c
    # Task & Graph
    src/task/task.c
    src/task/graph.c
    src/task/dependency.c
    # Scheduler
    src/scheduler/scheduler.c
    # Executor
    src/executor/executor.c
    # Tool
    src/tool/tool.c
    src/tool/tool_call.c
    src/tool/tool_registry.c
    src/tool/tool_schema.c
    # Provider
    src/provider/provider.c
    src/provider/provider_registry.c
    src/provider/llm.c
    src/provider/embedding.c
    # Planner
    src/planner/plan.c
    src/planner/planner.c
    # Strategy
    src/strategy/strategy_registry.c
    # Context
    src/context/context.c
    # Memory
    src/memory/memory.c
    src/memory/memory_working.c
    src/memory/memory_episodic.c
    src/memory/memory_semantic.c
    src/memory/memory_procedural.c
    # Critic
    src/critic/critic.c
    # Reflection
    src/reflection/reflection.c
    # Replanner
    src/replanner/replanner.c
    # Storage
    src/storage/storage.c
    src/storage/storage_store.c
    # Checkpoint
    src/checkpoint/checkpoint.c
    # Security
    src/security/security.c
    # Event
    src/event/event.c
    src/event/event_bus.c
    # Plugin
    src/plugin/plugin.c
    # Observability
    src/observability/log.c
    src/observability/metrics.c
    src/observability/trace.c
    # Provider implementations
    providers/llm/mock/llm_mock.c
    providers/embedding/embedding_hash.c
    providers/storage/sqlite/storage_sqlite.c
    strategies/plan_execute/plan_execute.c
    # CLI
    src/shell.c
    src/status.c
    src/autonomous_state.c
    src/autonomous_state.c
    src/autonomous_agent.c
)
target_include_directories(aegis_core
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${PROJECT_SOURCE_DIR}/src/internal
        ${PROJECT_SOURCE_DIR}/providers
        ${PROJECT_SOURCE_DIR}/strategies
)
target_link_libraries(aegis_core PRIVATE aegis_common sqlite3 m)
aegis_set_warnings(aegis_core)
if(AEGIS_FORMAT_CODE AND CLANG_FORMAT_PROGRAM)
    add_dependencies(aegis_core pre_build_format)
endif()

# ── Executable (CLI shell) ──────────────────────────────────────────────────
add_executable(aegis_shell src/shell.c)
target_link_libraries(aegis_shell PRIVATE aegis_core)
aegis_set_warnings(aegis_shell)
if(AEGIS_FORMAT_CODE AND CLANG_FORMAT_PROGRAM)
    add_dependencies(aegis_shell pre_build_format)
endif()

# ── Aegis CLI (Application layer, public API only) ──────────────────────────
add_executable(aegis
    src/cli_main.c
    src/cli_init.c
    src/cli_run.c
    src/cli_status.c
    src/cli_cancel.c
    src/cli_inspect.c
)
target_link_libraries(aegis PRIVATE aegis_core)
target_compile_definitions(aegis PRIVATE AEGIS_VERSION_STRING="${AEGIS_VERSION_STRING}")
aegis_set_warnings(aegis)
if(AEGIS_FORMAT_CODE AND CLANG_FORMAT_PROGRAM)
    add_dependencies(aegis pre_build_format)
endif()

# ── Tests ────────────────────────────────────────────────────────────────────
if(AEGIS_BUILD_TESTS)
    enable_testing()

    find_package(GTest QUIET)
    if(NOT GTest_FOUND)
        message(STATUS "GTest not found; compiling GoogleTest as subproject")
        include(FetchContent)
        FetchContent_Declare(
            googletest
            GIT_REPOSITORY https://github.com/google/googletest.git
            GIT_TAG        v1.14.0
        )
        set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
        FetchContent_MakeAvailable(googletest)
    endif()

    macro(aegis_add_test name)
        add_executable(${name} ${ARGN})
        target_link_libraries(${name} PRIVATE aegis_core GTest::gtest_main)
        aegis_set_warnings(${name})
        add_test(NAME ${name} COMMAND ${name})
        if(AEGIS_FORMAT_CODE AND CLANG_FORMAT_PROGRAM)
            add_dependencies(${name} pre_build_format)
        endif()
    endmacro()

    aegis_add_test(unit_scheduler  tests/unit/test_scheduler.c)
    aegis_add_test(unit_error      tests/unit/common/test_error.c)
    aegis_add_test(unit_allocator  tests/unit/common/test_allocator.c)
    aegis_add_test(unit_queue      tests/unit/common/test_queue.c)
    aegis_add_test(unit_hashmap    tests/unit/common/test_hashmap.c)
    aegis_add_test(unit_buffer     tests/unit/common/test_buffer.c)
    aegis_add_test(unit_string     tests/unit/common/test_string.c)
    aegis_add_test(unit_mutex      tests/unit/common/test_mutex.c)
    aegis_add_test(unit_uuid       tests/unit/common/test_uuid.c)
    aegis_add_test(unit_atomic     tests/unit/common/test_atomic.c)
    aegis_add_test(unit_vector     tests/unit/common/test_vector.c)
    aegis_add_test(unit_list       tests/unit/common/test_list.c)
    aegis_add_test(unit_runtime        tests/unit/test_runtime.c)
    aegis_add_test(unit_runtime_concurrent tests/unit/test_runtime_concurrent.c)
    aegis_add_test(unit_agent        tests/unit/test_agent.c)
    aegis_add_test(unit_agent_concurrent tests/unit/test_agent_concurrent.c)
    aegis_add_test(unit_event_bus        tests/unit/test_event_bus.c)
    aegis_add_test(unit_task             tests/unit/test_task.c)
    aegis_add_test(unit_task_graph       tests/unit/test_task_graph.c)
    aegis_add_test(unit_task_race_regression tests/unit/test_task_race_regression.c)
    aegis_add_test(unit_task_concurrent  tests/unit/test_task_concurrent.c)
    aegis_add_test(unit_scheduler_concurrent tests/unit/test_scheduler_concurrent.c)
    aegis_add_test(unit_executor             tests/unit/test_executor.c)
    aegis_add_test(unit_executor_concurrent  tests/unit/test_executor_concurrent.c)
    aegis_add_test(unit_tool             tests/unit/test_tool.c)
    aegis_add_test(unit_tool_executor    tests/unit/test_tool_executor.c)
    aegis_add_test(unit_tool_concurrent  tests/unit/test_tool_concurrent.c)
    aegis_add_test(unit_provider             tests/unit/test_provider.c)
    aegis_add_test(unit_provider_interfaces  tests/unit/test_provider_interfaces.c)
    aegis_add_test(unit_provider_concurrent  tests/unit/test_provider_concurrent.c)
    aegis_add_test(unit_planner              tests/unit/test_planner.c)
    aegis_add_test(unit_planner_llm          tests/unit/test_planner_llm.c)
    aegis_add_test(unit_memory             tests/unit/test_memory.c)

    aegis_add_test(unit_context            tests/unit/test_context.c)
    aegis_add_test(unit_storage            tests/unit/test_storage.c)
    aegis_add_test(unit_checkpoint         tests/unit/test_checkpoint.c)
    aegis_add_test(unit_log                tests/unit/test_log.c)
    aegis_add_test(unit_metrics            tests/unit/test_metrics.c)
    aegis_add_test(unit_trace              tests/unit/test_trace.c)
    aegis_add_test(unit_plugin             tests/unit/test_plugin.c)
    target_link_libraries(unit_plugin PRIVATE aegis_core dl pthread)
    aegis_add_test(unit_security           tests/unit/test_security.c)
    target_link_libraries(unit_security PRIVATE pthread)
    aegis_add_test(unit_provider_llm       tests/unit/test_provider_llm.c)
    aegis_add_test(unit_provider_storage   tests/unit/test_provider_storage.c)
    aegis_add_test(unit_provider_embedding tests/unit/test_provider_embedding.c)
    aegis_add_test(system_autonomous        tests/system/test_autonomous_closed_loop.c)
    target_link_libraries(system_autonomous PRIVATE pthread)
    aegis_add_test(unit_autonomous_tool tests/unit/test_autonomous_tool_integration.c)
    add_executable(integration_cli tests/integration/test_cli.c)
    target_link_libraries(integration_cli PRIVATE aegis_core)
    aegis_set_warnings(integration_cli)
    add_test(NAME integration_cli COMMAND integration_cli)
    if(AEGIS_FORMAT_CODE AND CLANG_FORMAT_PROGRAM)
        add_dependencies(integration_cli pre_build_format)
    endif()
endif()

# ── Mock plugin shared object ────────────────────────────────────────────────
add_library(mock_plugin SHARED tests/plugin/plugin_mock.c)
target_include_directories(mock_plugin PRIVATE ${PROJECT_SOURCE_DIR}/include
                                                ${PROJECT_SOURCE_DIR}/src/internal)
target_link_libraries(mock_plugin PRIVATE aegis_core)
set_target_properties(mock_plugin PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/tests/plugin"
)

# ── Benchmarks ───────────────────────────────────────────────────────────────
if(AEGIS_BUILD_BENCH)
    find_package(benchmark QUIET)
    if(NOT benchmark_FOUND)
        message(STATUS "google/benchmark not found; skipping benchmarks")
    else()
        add_executable(bench_scheduler benchmarks/bench_scheduler.c)
        target_link_libraries(bench_scheduler PRIVATE aegis_core benchmark::benchmark)
        aegis_set_warnings(bench_scheduler)
    endif()
endif()

# ── Install ──────────────────────────────────────────────────────────────────
include(GNUInstallDirs)
install(TARGETS aegis_common aegis_core aegis_shell
    EXPORT aegisTargets
    LIBRARY   DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE   DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME   DESTINATION ${CMAKE_INSTALL_BINDIR}
)
# Generate version.h from template
configure_file(
    include/aegis/version.h.in
    "${PROJECT_BINARY_DIR}/include/aegis/version.h"
    @ONLY
)
target_include_directories(aegis_common
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_BINARY_DIR}/include>
)
# ── CMake package config ─────────────────────────────────────────────────────
include(CMakePackageConfigHelpers)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/aegisConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)
configure_package_config_file(
    cmake/aegisConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/aegisConfig.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/aegis
)
install(EXPORT aegisTargets
    FILE aegisTargets.cmake
    NAMESPACE aegis::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/aegis
)
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/aegisConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/aegisConfigVersion.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/aegis
)
