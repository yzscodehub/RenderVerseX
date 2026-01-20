# =============================================================================
# Animation Module - Skeletal animation, keyframes, and interpolation
# =============================================================================
add_library(RVX_Animation INTERFACE)

# Animation module is header-only for now
target_include_directories(RVX_Animation INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(RVX_Animation INTERFACE
    RVX::Core
)

# Alias for cleaner target names
add_library(RVX::Animation ALIAS RVX_Animation)
