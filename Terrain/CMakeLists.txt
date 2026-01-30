# =============================================================================
# Terrain Module - Heightmap-based terrain rendering and collision
# =============================================================================
# Provides:
# - TerrainComponent (scene entity terrain attachment)
# - Heightmap (terrain height data management)
# - TerrainLOD (level-of-detail system with CDLOD/Clipmap support)
# - TerrainMaterial (multi-layer terrain texturing)
# - TerrainCollider (physics collision for terrain)
# - TerrainRenderer (terrain rendering pass)
# =============================================================================
add_library(RVX_Terrain STATIC)

target_sources(RVX_Terrain PRIVATE
    # Core
    Private/TerrainRenderer.cpp
    Private/TerrainLOD.cpp
    Private/TerrainCollider.cpp
    Private/Heightmap.cpp
    Private/TerrainComponent.cpp
    Private/TerrainMaterial.cpp
)

target_include_directories(RVX_Terrain PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Include>
    $<INSTALL_INTERFACE:include>
)

# Private access to Engine, World, Resource, Scene headers
target_include_directories(RVX_Terrain PRIVATE
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/Engine/Include>
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/World/Include>
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/Resource/Include>
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/Scene/Include>
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/Geometry/Include>
)

target_link_libraries(RVX_Terrain PUBLIC
    RVX_Core
    RVX_RHI
    RVX::Render
    RVX::Scene
    Spatial
)

add_library(RVX::Terrain ALIAS RVX_Terrain)
