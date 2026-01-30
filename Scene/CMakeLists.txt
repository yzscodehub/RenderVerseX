# =============================================================================
# Scene Module - Scene graph, mesh, material, and model management
# =============================================================================
add_library(RVX_Scene STATIC)

target_sources(RVX_Scene PRIVATE
    # Core scene files
    Private/VertexAttribute.cpp
    Private/Mesh.cpp
    Private/Material.cpp
    Private/Node.cpp
    Private/Model.cpp
    Private/SceneEntity.cpp
    Private/SceneManager.cpp
    Private/Component.cpp
    Private/ComponentFactory.cpp
    Private/Prefab.cpp

    # Components
    Private/Components/MeshRendererComponent.cpp
    Private/Components/LightComponent.cpp
    Private/Components/AudioComponent.cpp
    Private/Components/CameraComponent.cpp
    Private/Components/ColliderComponent.cpp
    Private/Components/RigidBodyComponent.cpp
    Private/Components/AnimatorComponent.cpp
    Private/Components/SkeletonComponent.cpp
    Private/Components/SkyboxComponent.cpp
    Private/Components/ReflectionProbeComponent.cpp
    Private/Components/LightProbeComponent.cpp
    Private/Components/DecalComponent.cpp
    Private/Components/LODComponent.cpp
)

target_include_directories(RVX_Scene PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(RVX_Scene PUBLIC
    RVX::Core
    Spatial
)

# Include headers for components that use external resources
# Using PRIVATE to avoid circular dependency (Resource/Physics/Animation link to Scene)
target_include_directories(RVX_Scene PRIVATE
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/Resource/Include>
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/Audio/Include>
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/Physics/Include>
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/Animation/Include>
)

# Alias for cleaner target names
add_library(RVX::Scene ALIAS RVX_Scene)
