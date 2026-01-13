# =============================================================================
# Core Module - Foundation types and utilities
# =============================================================================
add_library(RVX_Core STATIC)

target_sources(RVX_Core PRIVATE
    Private/Memory.cpp
    Private/Log.cpp
)

target_include_directories(RVX_Core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(RVX_Core PUBLIC
    spdlog::spdlog
)

# Alias for cleaner target names
add_library(RVX::Core ALIAS RVX_Core)
