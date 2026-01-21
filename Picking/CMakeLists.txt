# =============================================================================
# Picking Module - DEPRECATED: Use World module instead
# =============================================================================
# This module is maintained for backward compatibility.
# New code should use:
# - World/SpatialSubsystem.h for raycasting
# - World/PickingService.h for convenience picking
# =============================================================================
add_library(RVX_Picking STATIC)

target_sources(RVX_Picking PRIVATE
    Private/PickingSystem.cpp
)

target_include_directories(RVX_Picking PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Private>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(RVX_Picking PUBLIC
    RVX_Core
    RVX_Scene
    RVX_Runtime
    Spatial
    RVX_World
)

add_library(RVX::Picking ALIAS RVX_Picking)
