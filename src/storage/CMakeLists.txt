# ── src/storage/CMakeLists.txt ─────────────────────────────────────────
add_library(aegis_storage storage.c storage_store.c)
target_include_directories(aegis_storage PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>
                                   PRIVATE ${PROJECT_SOURCE_DIR}/src/internal ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(aegis_storage PUBLIC aegis_common PRIVATE sqlite3)
aegis_set_warnings(aegis_storage)
