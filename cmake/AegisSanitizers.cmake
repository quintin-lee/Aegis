# ── AegisSanitizers.cmake ────────────────────────────────────────────────
# Usage:
#   cmake -S . -B build-asan  -DCMAKE_C_FLAGS="-fsanitize=address,undefined -g"
#   cmake -S . -B build-tsan  -DCMAKE_C_COMPILER=clang -DCMAKE_C_FLAGS="-fsanitize=thread -g"
# Sanitizers are driven via CMAKE_C_FLAGS / LD_PRELOAD, not via a target.

option(AEGIS_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(AEGIS_ENABLE_TSAN "Enable ThreadSanitizer" OFF)
option(AEGIS_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)

if(AEGIS_ENABLE_ASAN)
    add_compile_options(-fsanitize=address -g)
    add_link_options(-fsanitize=address)
endif()
if(AEGIS_ENABLE_UBSAN)
    add_compile_options(-fsanitize=undefined -g)
    add_link_options(-fsanitize=undefined)
endif()
if(AEGIS_ENABLE_TSAN)
    add_compile_options(-fsanitize=thread -g)
    add_link_options(-fsanitize=thread)
endif()
