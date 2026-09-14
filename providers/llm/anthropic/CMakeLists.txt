# ── providers/llm/anthropic/CMakeLists.txt ──────────────────────────────
find_package(CURL REQUIRED)

add_library(aegis_llm_anthropic STATIC
    structured_anthropic.c
)
set_source_files_properties(structured_anthropic.c PROPERTIES COMPILE_DEFINITIONS AEGIS_ANTHROPIC_TEST_API)
target_include_directories(aegis_llm_anthropic PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>
                                            PRIVATE ${PROJECT_SOURCE_DIR}/providers/llm/anthropic
                                                    ${PROJECT_SOURCE_DIR}/providers/llm/shared
                                                    ${CURL_INCLUDE_DIRS})
target_link_libraries(aegis_llm_anthropic PUBLIC aegis_common aegis_model aegis_message aegis_tool aegis_llm_shared PRIVATE ${CURL_LIBRARIES})
aegis_set_warnings(aegis_llm_anthropic)
