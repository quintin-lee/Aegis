# ── AegisWarnings.cmake ──────────────────────────────────────────────────
function(aegis_set_warnings target)
    if(AEGIS_WARNINGS_AS_ERRORS)
        set(_WARN_FLAGS -Wall -Wextra -Wpedantic -Werror)
    else()
        set(_WARN_FLAGS -Wall -Wextra -Wpedantic)
    endif()
    target_compile_options(${target} PRIVATE ${_WARN_FLAGS})
endfunction()
