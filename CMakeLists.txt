cmake_minimum_required(VERSION 3.20)

project(aegis
    VERSION 0.1.0
    DESCRIPTION "Modular autonomous agent runtime (C11)"
    LANGUAGES C
)

# ── Policy ────────────────────────────────────────────────────────────────────
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# ── Options ───────────────────────────────────────────────────────────────────
option(AEGIS_BUILD_TESTS   "Build unit/integration tests"        ON)
option(AEGIS_BUILD_BENCH   "Build benchmark executables"         OFF)
option(AEGIS_BUILD_DOCS    "Generate documentation (Doxygen)"    OFF)
option(AEGIS_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)

# ── Compiler warnings ────────────────────────────────────────────────────────
function(aegis_set_warnings target)
    if(AEGIS_WARNINGS_AS_ERRORS)
        set(_WARN_FLAGS -Wall -Wextra -Wpedantic -Werror)
    else()
        set(_WARN_FLAGS -Wall -Wextra -Wpedantic)
    endif()
    target_compile_options(${target} PRIVATE ${_WARN_FLAGS})
endfunction()

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
    src/agent.c
    src/status.c
    src/task.c
    src/scheduler.c
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

    aegis_add_test(unit_agent      tests/unit/test_agent.c)
    aegis_add_test(unit_task       tests/unit/test_task.c)
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
install(DIRECTORY include/aegis DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

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
