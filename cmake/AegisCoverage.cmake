# ── AegisCoverage.cmake ──────────────────────────────────────────────────
# Coverage reporting support using gcov/lcov.
# Usage:
#   cmake -S . -B build-coverage -DAEGIS_ENABLE_COVERAGE=ON
#   cmake --build build-coverage
#   ctest --test-dir build-coverage
#   lcov --capture --directory build-coverage --output-file coverage.info
#   genhtml coverage.info --output-directory coverage-html

option(AEGIS_ENABLE_COVERAGE "Enable code coverage reporting (requires gcov/lcov)" OFF)

if(AEGIS_ENABLE_COVERAGE)
    message(STATUS "Code coverage enabled")

    # Add coverage to all compile/link commands globally
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} --coverage -O0 -g")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} --coverage")
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} --coverage")

    # Add coverage to all targets after they are defined
    function(aegis_enable_coverage target)
        target_compile_options(${target} PRIVATE --coverage -O0 -g)
        target_link_options(${target} PRIVATE --coverage)
    endfunction()

    find_program(LCOV_PATH lcov)
    find_program(GENHTML_PATH genhtml)

    if(LCOV_PATH AND GENHTML_PATH)
        add_custom_target(coverage
            COMMAND ${LCOV_PATH} --zerocounters --directory ${CMAKE_BINARY_DIR}
            COMMAND ${CMAKE_CTEST_COMMAND} --test-dir ${CMAKE_BINARY_DIR} --output-on-failure
            COMMAND ${LCOV_PATH} --capture --directory ${CMAKE_BINARY_DIR} --output-file coverage.info
            COMMAND ${GENHTML_PATH} coverage.info --output-directory coverage-html
            COMMENT "Running tests and generating coverage report"
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        )
        message(STATUS "Coverage target available: coverage")
    else()
        message(WARNING "lcov/genhtml not found; coverage target unavailable")
    endif()
endif()
