cmake_minimum_required(VERSION 3.15)
project(ufbx VERSION 0.14.3 LANGUAGES C)

add_library(ufbx STATIC ufbx.c)
add_library(ufbx::ufbx ALIAS ufbx)
target_compile_features(ufbx PUBLIC c_std_99)
set_target_properties(ufbx PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED YES
    C_EXTENSIONS NO)
target_compile_definitions(ufbx PUBLIC UFBX_REAL_IS_FLOAT=1)
target_include_directories(ufbx PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<INSTALL_INTERFACE:include>)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(TARGETS ufbx EXPORT ufbxTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES ufbx.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT ufbxTargets
    FILE ufbxTargets.cmake
    NAMESPACE ufbx::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ufbx)

configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/ufbx-config.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/ufbx-config.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ufbx)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/ufbx-config-version.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/ufbx-config.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/ufbx-config-version.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ufbx)
