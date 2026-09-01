# ── providers/llm/openai/CMakeLists.txt ───────────────────────────────────
find_package(CURL REQUIRED)

add_library(aegis_llm_openai STATIC
    openai_llm.c
    structured_openai.c
)
set_source_files_properties(structured_openai.c PROPERTIES COMPILE_DEFINITIONS AEGIS_OPENAI_TEST_API)
target_include_directories(aegis_llm_openai PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>
                                            PRIVATE ${PROJECT_SOURCE_DIR}/providers/llm/openai
                                                    ${CURL_INCLUDE_DIRS})
target_link_libraries(aegis_llm_openai PUBLIC aegis_common aegis_model aegis_message aegis_tool PRIVATE ${CURL_LIBRARIES})
aegis_set_warnings(aegis_llm_openai)
