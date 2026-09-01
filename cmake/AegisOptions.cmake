# ── AegisOptions.cmake ───────────────────────────────────────────────────
option(AEGIS_BUILD_TESTS   "Build unit/integration tests"        ON)
option(AEGIS_BUILD_BENCH   "Build benchmark executables"         OFF)
option(AEGIS_BUILD_DOCS    "Generate documentation (Doxygen)"    OFF)
option(AEGIS_FORMAT_CODE "Run clang-format on all sources before build" OFF)
option(AEGIS_WARNINGS_AS_ERRORS "Treat warnings as errors" OFF)
option(AEGIS_OPENAI_PROVIDER "Build OpenAI-compatible LLM provider (requires libcurl)" ON)
