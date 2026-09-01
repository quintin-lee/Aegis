cmake_minimum_required(VERSION 3.20)

# ── Version ──────────────────────────────────────────────────────────────
include(cmake/AegisVersion.cmake)

# ── Options ──────────────────────────────────────────────────────────────
include(cmake/AegisOptions.cmake)
include(cmake/AegisWarnings.cmake)
include(cmake/AegisSanitizers.cmake)

# ── clang-format ─────────────────────────────────────────────────────────
find_program(CLANG_FORMAT_PROGRAM clang-format)
if(CLANG_FORMAT_PROGRAM)
    message(STATUS "clang-format found: ${CLANG_FORMAT_PROGRAM}")
    file(GLOB_RECURSE AEGIS_ALL_SOURCES
        CONFIGURE_DEPENDS
        src/*.c
        src/**/*.c
        include/aegis/*.h
        include/aegis/**/*.h
        apps/**/*.c
        apps/**/*.h
        tests/**/*.c
        tests/**/*.h
    )
    add_custom_target(format
        COMMAND ${CLANG_FORMAT_PROGRAM} -i -style=file ${AEGIS_ALL_SOURCES}
        COMMENT "Formatting all sources with clang-format"
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    add_custom_target(format-check
        COMMAND ${CLANG_FORMAT_PROGRAM} --dry-run --Werror -style=file ${AEGIS_ALL_SOURCES}
        COMMENT "Checking format without modifying files"
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
else()
    message(STATUS "clang-format not found; format targets unavailable")
endif()

# ── libaegis_common ──────────────────────────────────────────────────────
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

# ── Fine-grained libraries ───────────────────────────────────────────────
include(cmake/AegisLibraries.cmake)

# ── Executables ──────────────────────────────────────────────────────────
add_executable(aegis_shell src/shell.c)
target_link_libraries(aegis_shell PRIVATE aegis_core)
aegis_set_warnings(aegis_shell)

add_executable(aegis
    apps/aegis/main.c
    apps/aegis/cli_helpers.c
    apps/aegis/cli_init.c
    apps/aegis/cli_run.c
    apps/aegis/cli_status.c
    apps/aegis/cli_cancel.c
    apps/aegis/cli_inspect.c
    apps/aegis/cli_interactive.c
)
target_link_libraries(aegis PRIVATE aegis_core)
target_compile_definitions(aegis PRIVATE AEGIS_VERSION_STRING="${AEGIS_VERSION_STRING}")
aegis_set_warnings(aegis)

# ── Tests ────────────────────────────────────────────────────────────────
include(cmake/AegisTests.cmake)

# ── Install ──────────────────────────────────────────────────────────────
include(cmake/AegisInstall.cmake)
