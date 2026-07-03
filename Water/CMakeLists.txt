# =============================================================================
# Water Module - Water surface rendering and simulation
# =============================================================================
# Provides:
# - WaterComponent (scene entity water attachment)
# - WaterSurface (water mesh and properties)
# - WaterSimulation (wave and physics simulation)
# - Caustics (underwater light patterns)
# - Underwater (underwater post-processing effects)
# - Runtime water data used by Render-owned water passes
# =============================================================================
add_library(RVX_Water STATIC)

target_sources(RVX_Water PRIVATE
    # Core
    Private/WaterSimulation.cpp
    Private/Caustics.cpp
    Private/WaterComponent.cpp
    Private/WaterSurface.cpp
    Private/Underwater.cpp
)

target_include_directories(RVX_Water PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Include>
    $<INSTALL_INTERFACE:include>
    PRIVATE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Private>
)

target_link_libraries(RVX_Water PUBLIC
    RVX_Core
    RVX::RenderContracts
    RVX::Scene
    PRIVATE
        RVX_RHI
)

add_library(RVX::Water ALIAS RVX_Water)
