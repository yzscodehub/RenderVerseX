# Asset Module - Resource management system
# Provides: Asset, AssetHandle, ResourceManager, Loaders

add_library(RVX_Asset STATIC
    # Core
    Private/Asset.cpp
    Private/AssetRegistry.cpp
    Private/AssetCache.cpp
    Private/ResourceManager.cpp
    Private/DependencyGraph.cpp
    
    # Types (placeholder, will be expanded)
    Private/Types/MeshAsset.cpp
    Private/Types/TextureAsset.cpp
    Private/Types/MaterialAsset.cpp
)

target_include_directories(RVX_Asset
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/Include
)

target_link_libraries(RVX_Asset
    PUBLIC
        RVX::Core
        Spatial
        RVX::Scene
)

# Alias for cleaner target names
add_library(RVX::Asset ALIAS RVX_Asset)

# Set C++20 standard
target_compile_features(RVX_Asset PUBLIC cxx_std_20)
