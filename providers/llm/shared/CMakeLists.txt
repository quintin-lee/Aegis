# ── providers/llm/shared/CMakeLists.txt ─────────────────────────────────
#
# Provider-agnostic HTTP/JSON transport infrastructure shared by the LLM
# provider backends (OpenAI, Anthropic, ...). Links CURL PUBLIC so any
# consumer using llm_shared.h (which includes <curl/curl.h> for the
# progress-callback signature) gets curl types transitively.

find_package(CURL REQUIRED)

add_library(aegis_llm_shared STATIC
    json_builder.c
    sse_http.c
)
target_include_directories(aegis_llm_shared PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>
                                            PRIVATE ${CURL_INCLUDE_DIRS})
target_link_libraries(aegis_llm_shared PUBLIC aegis_common aegis_model ${CURL_LIBRARIES})
aegis_set_warnings(aegis_llm_shared)
