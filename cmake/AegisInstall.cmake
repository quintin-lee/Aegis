# ── AegisInstall.cmake ───────────────────────────────────────────────────
# Export targets for downstream consumers.

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/aegisConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)
configure_package_config_file(
    cmake/aegisConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/aegisConfig.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/aegis
)

set(_aegis_libs
    aegis_common aegis_runtime aegis_event
    aegis_provider aegis_tool aegis_message aegis_model
    aegis_session aegis_context aegis_task aegis_scheduler aegis_executor
    aegis_planner aegis_memory aegis_checkpoint aegis_security
    aegis_observability aegis_plugin aegis_storage
    aegis_critic aegis_autonomous aegis_agent aegis_coding
    aegis_skill aegis_extension
    aegis_workflow aegis_core
)

install(TARGETS ${_aegis_libs}
    EXPORT aegisTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(EXPORT aegisTargets
    FILE aegisTargets.cmake
    NAMESPACE aegis::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/aegis
)
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/aegisConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/aegisConfigVersion.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/aegis
)

# Install headers
install(DIRECTORY include/aegis DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
