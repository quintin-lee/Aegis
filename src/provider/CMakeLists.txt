# ── src/provider/CMakeLists.txt ────────────────────────────────────────
add_library(aegis_provider
    provider.c provider_registry.c llm.c embedding.c
    ${PROJECT_SOURCE_DIR}/providers/llm/mock/llm_mock.c
    ${PROJECT_SOURCE_DIR}/providers/embedding/embedding_hash.c
    ${PROJECT_SOURCE_DIR}/providers/storage/sqlite/storage_sqlite.c
)
target_include_directories(aegis_provider PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>
                                     PRIVATE ${PROJECT_SOURCE_DIR}/src/internal ${PROJECT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/providers ${PROJECT_SOURCE_DIR}/strategies)
target_link_libraries(aegis_provider PUBLIC aegis_common PRIVATE sqlite3 m)
aegis_set_warnings(aegis_provider)
