# ── providers/llm/openai/CMakeLists.txt ───────────────────────────────────
find_package(CURL REQUIRED)

add_library(aegis_llm_openai STATIC
    openai_llm.c
)
target_include_directories(aegis_llm_openai PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>
                                            PRIVATE ${PROJECT_SOURCE_DIR}/providers/llm/openai
                                                    ${CURL_INCLUDE_DIRS})
target_link_libraries(aegis_llm_openai PUBLIC aegis_common PRIVATE ${CURL_LIBRARIES})
aegis_set_warnings(aegis_llm_openai)
