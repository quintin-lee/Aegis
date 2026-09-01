# ── AegisInstall.cmake ───────────────────────────────────────────────────
include(GNUInstallDirs)
# Install non-INTERFACE libraries and executables
install(TARGETS aegis_common aegis_shell
    EXPORT aegisTargets
    LIBRARY   DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE   DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME   DESTINATION ${CMAKE_INSTALL_BINDIR}
)
# Install INTERFACE umbrella (no LIBRARY/ARCHIVE/RUNTIME)
install(TARGETS aegis_core EXPORT aegisTargets)
# For split libs, install all aegis_* targets if they exist
get_property(_all_targets DIRECTORY ${CMAKE_SOURCE_DIR} PROPERTY BUILDSYSTEM_TARGETS)
foreach(_t IN LISTS _all_targets)
    if(_t MATCHES "^aegis_")
        if(NOT _t STREQUAL "aegis_common" AND NOT _t STREQUAL "aegis_core" AND NOT _t STREQUAL "aegis_shell" AND NOT _t STREQUAL "aegis")
            install(TARGETS ${_t} EXPORT aegisTargets
                LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
                ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
                RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
            )
        endif()
    endif()
endforeach()

configure_file(
    include/aegis/version.h.in
    "${PROJECT_BINARY_DIR}/include/aegis/version.h"
    @ONLY
)
target_include_directories(aegis_common
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_BINARY_DIR}/include>
)

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
