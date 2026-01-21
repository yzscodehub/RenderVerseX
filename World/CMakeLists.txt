# =============================================================================
# World Module - World container, spatial subsystem, and picking
# =============================================================================
# Integrates:
# - Scene module (SceneManager, SceneEntity, etc.)
# - Spatial module (BVH, spatial queries)
# - Picking functionality
# =============================================================================
add_library(RVX_World STATIC)

target_sources(RVX_World PRIVATE
    Private/World.cpp
    Private/SpatialSubsystem.cpp
    Private/PickingService.cpp
)

target_include_directories(RVX_World PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(RVX_World PUBLIC
    RVX_Core
    RVX_Scene
    Spatial
    RVX_Runtime
)

add_library(RVX::World ALIAS RVX_World)
