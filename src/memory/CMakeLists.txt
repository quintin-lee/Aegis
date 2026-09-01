# ── src/memory/CMakeLists.txt ──────────────────────────────────────────
add_library(aegis_memory
    memory.c memory_working.c memory_episodic.c memory_semantic.c memory_procedural.c
)
target_include_directories(aegis_memory PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>
                                   PRIVATE ${PROJECT_SOURCE_DIR}/src/internal ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(aegis_memory PUBLIC aegis_common)
aegis_set_warnings(aegis_memory)
