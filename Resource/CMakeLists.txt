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
# - Concrete resource types (MeshResource, TextureResource, MaterialResource, AudioResource)
# - Hot-reload support with file watching
# - FBX import via ufbx
# - HDR/EXR loading with IBL precomputation
# - Audio loading via miniaudio
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
    Private/Types/AudioResource.cpp

    # Loaders
    Private/Loader/TextureLoader.cpp
    Private/Loader/ModelLoader.cpp
    Private/Loader/AudioLoader.cpp
    Private/Loader/HDRTextureLoader.cpp

    # Importers
    Private/Importer/GLTFImporter.cpp
    Private/Importer/FBXImporter.cpp

    # Hot-reload system
    Private/FileWatcher.cpp
    Private/HotReloadManager.cpp
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

# Find ufbx for FBX loading
# ufbx is not in vcpkg, so we use FetchContent to download it
option(RVX_ENABLE_FBX "Enable FBX loading via ufbx" ON)

if(RVX_ENABLE_FBX)
    include(FetchContent)
    
    # Skip update step when offline (use cached version if available)
    set(FETCHCONTENT_UPDATES_DISCONNECTED ON)
    
    FetchContent_Declare(
        ufbx
        GIT_REPOSITORY https://github.com/ufbx/ufbx.git
        GIT_TAG        v0.14.3
        GIT_SHALLOW    TRUE
    )
    
    FetchContent_GetProperties(ufbx)
    if(NOT ufbx_POPULATED)
        message(STATUS "RVX_Resource: Fetching ufbx...")
        FetchContent_Populate(ufbx)
    endif()

    if(EXISTS "${ufbx_SOURCE_DIR}/ufbx.h" AND EXISTS "${ufbx_SOURCE_DIR}/ufbx.c")
        target_sources(RVX_Resource PRIVATE "${ufbx_SOURCE_DIR}/ufbx.c")
        target_include_directories(RVX_Resource PRIVATE "${ufbx_SOURCE_DIR}")
        target_compile_definitions(RVX_Resource PRIVATE HAS_UFBX=1)
        message(STATUS "RVX_Resource: Using ufbx from ${ufbx_SOURCE_DIR}")
    else()
        message(WARNING "RVX_Resource: ufbx not found. FBX loading will not be available.")
        message(STATUS "RVX_Resource: To disable FBX support, use -DRVX_ENABLE_FBX=OFF")
    endif()
else()
    message(STATUS "RVX_Resource: FBX loading disabled (RVX_ENABLE_FBX=OFF)")
endif()

# Find tinyexr for EXR loading (header-only)
find_path(TINYEXR_INCLUDE_DIR tinyexr.h PATH_SUFFIXES tinyexr)
if(TINYEXR_INCLUDE_DIR)
    target_include_directories(RVX_Resource PRIVATE ${TINYEXR_INCLUDE_DIR})
    target_compile_definitions(RVX_Resource PRIVATE HAS_TINYEXR=1)
    message(STATUS "RVX_Resource: Using tinyexr from ${TINYEXR_INCLUDE_DIR}")
    
    # tinyexr may need miniz
    find_path(MINIZ_INCLUDE_DIR miniz.h PATH_SUFFIXES miniz)
    if(MINIZ_INCLUDE_DIR)
        target_include_directories(RVX_Resource PRIVATE ${MINIZ_INCLUDE_DIR})
        message(STATUS "RVX_Resource: Using miniz from ${MINIZ_INCLUDE_DIR}")
    else()
        message(WARNING "RVX_Resource: miniz not found. EXR loading may fail.")
    endif()
else()
    message(STATUS "RVX_Resource: tinyexr not found. EXR loading will not be available (HDR still works).")
endif()

# miniaudio from vcpkg (header-only)
find_path(MINIAUDIO_INCLUDE_DIR miniaudio.h)
if(MINIAUDIO_INCLUDE_DIR)
    target_include_directories(RVX_Resource PRIVATE ${MINIAUDIO_INCLUDE_DIR})
    message(STATUS "RVX_Resource: Using miniaudio from ${MINIAUDIO_INCLUDE_DIR}")
else()
    message(WARNING "RVX_Resource: miniaudio not found. Audio resource loading will not be available.")
endif()

add_library(RVX::Resource ALIAS RVX_Resource)
