# =============================================================================
# Scene Module - Scene graph, mesh, material, and model management
# =============================================================================
add_library(RVX_Scene STATIC)

target_sources(RVX_Scene PRIVATE
    Private/VertexAttribute.cpp
    Private/Mesh.cpp
    Private/Material.cpp
    Private/Node.cpp
    Private/Model.cpp
    Private/SceneEntity.cpp
    Private/SceneManager.cpp
    Private/Component.cpp
    Private/ComponentFactory.cpp
    Private/Components/MeshRendererComponent.cpp
    Private/Components/LightComponent.cpp
)

target_include_directories(RVX_Scene PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(RVX_Scene PUBLIC
    RVX::Core
    Spatial
)

# Include Resource headers for components that use ResourceHandle
# Using PRIVATE to avoid circular dependency (Resource links to Scene)
target_include_directories(RVX_Scene PRIVATE
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/Resource/Include>
)

# Alias for cleaner target names
add_library(RVX::Scene ALIAS RVX_Scene)
