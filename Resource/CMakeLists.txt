# =============================================================================
# Resource Module - Complete resource management system
# =============================================================================
# Provides:
# - IResource base class for all resource types
# - ResourceHandle<T> type-safe smart handles
# - ResourceManager central facade
# - ResourceCache LRU caching with memory limits
# - ResourceRegistry metadata database
# - DependencyGraph dependency resolution
# - ResourceSubsystem engine integration
# - Concrete resource types (MeshResource, TextureResource, MaterialResource)
#
# This is the canonical resource management module. The Asset module is now
# a compatibility layer that forwards to this module.
# =============================================================================
add_library(RVX_Resource STATIC)

target_sources(RVX_Resource PRIVATE
    # Core resource management
    Private/IResource.cpp
    Private/ResourceCache.cpp
    Private/ResourceRegistry.cpp
    Private/DependencyGraph.cpp
    Private/ResourceManager.cpp
    Private/DefaultResources.cpp
    
    # Concrete resource types
    Private/Types/MeshResource.cpp
    Private/Types/TextureResource.cpp
    Private/Types/MaterialResource.cpp
    Private/Types/ModelResource.cpp

    # Loaders
    Private/Loader/TextureLoader.cpp
    Private/Loader/ModelLoader.cpp

    # Importers
    Private/Importer/GLTFImporter.cpp
)

target_include_directories(RVX_Resource PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Include>
    $<INSTALL_INTERFACE:include>
)

# Find tinygltf for glTF/GLB loading
find_package(unofficial-tinygltf CONFIG QUIET)
if(NOT unofficial-tinygltf_FOUND)
    # Try to find tinygltf without the unofficial prefix
    find_package(tinygltf CONFIG QUIET)
endif()

target_link_libraries(RVX_Resource PUBLIC
    RVX_Core
    RVX::Scene
)

# Link tinygltf if found
if(unofficial-tinygltf_FOUND)
    target_link_libraries(RVX_Resource PRIVATE unofficial::tinygltf::tinygltf)
    message(STATUS "RVX_Resource: Using unofficial-tinygltf")
elseif(tinygltf_FOUND)
    target_link_libraries(RVX_Resource PRIVATE tinygltf::tinygltf)
    message(STATUS "RVX_Resource: Using tinygltf")
else()
    # tinygltf is header-only, so we can include it directly if available
    find_path(TINYGLTF_INCLUDE_DIR tiny_gltf.h PATH_SUFFIXES tinygltf)
    if(TINYGLTF_INCLUDE_DIR)
        target_include_directories(RVX_Resource PRIVATE ${TINYGLTF_INCLUDE_DIR})
        message(STATUS "RVX_Resource: Using header-only tinygltf from ${TINYGLTF_INCLUDE_DIR}")
    else()
        message(WARNING "RVX_Resource: tinygltf not found. glTF loading will not be available.")
    endif()
endif()

add_library(RVX::Resource ALIAS RVX_Resource)
