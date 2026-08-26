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

# ── clang-format ──────────────────────────────────────────────────────────────
if(AEGIS_FORMAT_CODE)
    find_program(CLANG_FORMAT_PROGRAM clang-format)
    if(CLANG_FORMAT_PROGRAM)
        message(STATUS "clang-format found: ${CLANG_FORMAT_PROGRAM}")

        # Collect all C/C++ source files
        file(GLOB_RECURSE AEGIS_ALL_SOURCES
            CONFIGURE_DEPENDS
            src/*.c
            src/**/*.c
            include/aegis/*.h
            include/aegis/**/*.h
            tests/unit/**/*.c
        )

        add_custom_target(format
            COMMAND ${CLANG_FORMAT_PROGRAM} -i -style=file ${AEGIS_ALL_SOURCES}
            COMMENT "Formatting sources with clang-format"
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
)
target_include_directories(aegis_common
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${PROJECT_SOURCE_DIR}/src/internal
)
target_link_libraries(aegis_common PRIVATE m pthread)
aegis_set_warnings(aegis_common)

# ── libaegis-core ────────────────────────────────────────────────────────────
add_library(aegis_core
    src/task.c
    src/scheduler.c
    src/runtime.c
    src/config.c
    src/event.c
    src/event_bus.c
    src/agent.c
    src/goal.c
    src/status.c
    src/dependency.c
    src/graph.c
)
target_include_directories(aegis_core
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${PROJECT_SOURCE_DIR}/src/internal
)
target_link_libraries(aegis_core PRIVATE aegis_common)
aegis_set_warnings(aegis_core)

# ── Executable (CLI shell) ──────────────────────────────────────────────────
add_executable(aegis_shell src/shell.c)
target_link_libraries(aegis_shell PRIVATE aegis_core)
aegis_set_warnings(aegis_shell)

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
endif()

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
