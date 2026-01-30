# =============================================================================
# Water Module - Water surface rendering and simulation
# =============================================================================
# Provides:
# - WaterComponent (scene entity water attachment)
# - WaterSurface (water mesh and properties)
# - WaterSimulation (wave and physics simulation)
# - Caustics (underwater light patterns)
# - Underwater (underwater post-processing effects)
# - WaterRenderer (water rendering pass)
# =============================================================================
add_library(RVX_Water STATIC)

target_sources(RVX_Water PRIVATE
    # Core
    Private/WaterRenderer.cpp
    Private/WaterSimulation.cpp
    Private/Caustics.cpp
    Private/WaterComponent.cpp
    Private/WaterSurface.cpp
    Private/Underwater.cpp
)

target_include_directories(RVX_Water PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Include>
    $<INSTALL_INTERFACE:include>
)

# Private access to Engine, World, Resource, Scene headers
target_include_directories(RVX_Water PRIVATE
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/Engine/Include>
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/World/Include>
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/Resource/Include>
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/Scene/Include>
)

target_link_libraries(RVX_Water PUBLIC
    RVX_Core
    RVX_RHI
    RVX::Render
    RVX::Scene
    Spatial
)

add_library(RVX::Water ALIAS RVX_Water)
