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

# ── Subdirectories ───────────────────────────────────────────────────────
# Order matters: libraries are built bottom-up within each add_subdirectory,
# but top-level only adds subdirs; cmake/AegisLibraries.cmake still defines
# the INTERFACE umbrellas (aegis_workflow, aegis_core).
add_subdirectory(src/common)
add_subdirectory(src/runtime)
add_subdirectory(src/event)
add_subdirectory(src/provider)
add_subdirectory(src/tool)
add_subdirectory(src/message)
add_subdirectory(src/model)
add_subdirectory(src/session)
add_subdirectory(src/context)
add_subdirectory(src/task)
add_subdirectory(src/scheduler)
add_subdirectory(src/executor)
add_subdirectory(src/planner)
add_subdirectory(src/memory)
add_subdirectory(src/checkpoint)
add_subdirectory(src/security)
add_subdirectory(src/observability)
add_subdirectory(src/plugin)
add_subdirectory(src/storage)
add_subdirectory(src/critic)
add_subdirectory(src/autonomous)
add_subdirectory(src/agent)
add_subdirectory(src/coding)
add_subdirectory(src/skill)
add_subdirectory(src/extension)
# ── Optional providers ───────────────────────────────────────────────────
if(AEGIS_OPENAI_PROVIDER)
    find_package(CURL REQUIRED)
    message(STATUS "OpenAI provider enabled (libcurl found)")
    add_subdirectory(providers/llm/openai)
else()
    message(STATUS "OpenAI provider disabled (AEGIS_OPENAI_PROVIDER=OFF)")
endif()

# ── Fine-grained libraries ───────────────────────────────────────────────
 include(cmake/AegisLibraries.cmake)
# ── Executables ──────────────────────────────────────────────────────────
add_executable(aegis_shell src/autonomous/src/shell.c)
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
if(AEGIS_OPENAI_PROVIDER)
    target_link_libraries(aegis PRIVATE aegis_llm_openai)
    target_compile_definitions(aegis PRIVATE AEGIS_OPENAI_PROVIDER)
endif()
aegis_set_warnings(aegis)

# ── Tests ────────────────────────────────────────────────────────────────
include(cmake/AegisTests.cmake)

# ── Install ──────────────────────────────────────────────────────────────
include(cmake/AegisInstall.cmake)
