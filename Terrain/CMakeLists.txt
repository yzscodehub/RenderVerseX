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
    PRIVATE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Private>
)

target_link_libraries(RVX_Terrain PUBLIC
    RVX_Core
    RVX_RHI
    RVX::Render
    RVX::Scene
)

add_library(RVX::Terrain ALIAS RVX_Terrain)
