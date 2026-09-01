# ── src/common/CMakeLists.txt ──────────────────────────────────────────
add_library(aegis_common
    error.c result.c allocator.c queue.c hashmap.c buffer.c
    string.c thread.c mutex.c uuid.c atomic.c vector.c list.c time.c
    cancellation/cancellation.c
    ../autonomous/src/status.c
)
target_include_directories(aegis_common PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>
                                   PRIVATE ${PROJECT_SOURCE_DIR}/src/internal ${PROJECT_SOURCE_DIR}/providers ${PROJECT_SOURCE_DIR}/strategies)
target_link_libraries(aegis_common PRIVATE m pthread)
aegis_set_warnings(aegis_common)
