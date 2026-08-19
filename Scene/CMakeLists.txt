# =============================================================================
# Scene Module - Scene graph, mesh, material, and model management
# =============================================================================
add_library(RVX_Scene STATIC)

target_sources(RVX_Scene PRIVATE
    Private/ECS/FrozenSceneSnapshot.cpp
    Private/ECS/SceneFeatureSnapshotStore.cpp
    Private/ECS/SceneSkinningSnapshotStore.cpp
    Private/ECS/SceneEcsRuntime.cpp
    Private/ECS/SceneSpatialIndex.cpp
    Private/ECS/TransformHierarchy.cpp
)

target_include_directories(RVX_Scene PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(RVX_Scene PUBLIC
    RVX::Core
    RVX::ECS
    RVX::Geometry
    RVX::RenderContracts
)

# Alias for cleaner target names
add_library(RVX::Scene ALIAS RVX_Scene)
