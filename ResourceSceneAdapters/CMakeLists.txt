# =============================================================================
# ResourceSceneAdapters Module - Resource model data to Scene actor adapters
# =============================================================================
add_library(RVX_ResourceSceneAdapters STATIC)

target_sources(RVX_ResourceSceneAdapters PRIVATE
    Private/ResourceSceneAdapters.cpp
    Private/ComponentFactoryDefaults.cpp
    Private/ModelResourceSceneInstantiation.cpp
    Private/SceneAssetInstantiation.cpp
)

target_include_directories(RVX_ResourceSceneAdapters PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(RVX_ResourceSceneAdapters PUBLIC
    RVX::Core
    RVX::Geometry
    RVX::Resource
    RVX::Scene
)

target_compile_features(RVX_ResourceSceneAdapters PUBLIC cxx_std_20)

add_library(RVX::ResourceSceneAdapters ALIAS RVX_ResourceSceneAdapters)
