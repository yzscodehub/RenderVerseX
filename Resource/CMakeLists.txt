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
# - Concrete resource types (MeshResource, TextureResource, ShaderResource, MaterialResource, AudioResource)
# - Hot-reload support with file watching
# - FBX import via ufbx
# - HDR/EXR loading with IBL precomputation
# - Audio loading via miniaudio
#
# This is the canonical resource management module. The Asset module is now
# a compatibility layer that forwards to this module.
# =============================================================================
add_library(RVX_ResourceRuntime STATIC)

target_sources(RVX_ResourceRuntime PRIVATE
    # Core resource management
    Private/IResource.cpp
    Private/ResourceContentIdentity.cpp
    Private/CookManifest.cpp
    Private/ResourceLoadOperation.cpp
    Private/PreparedResourceBundle.cpp
    Private/ResourceCache.cpp
    Private/ResourceRegistry.cpp
    Private/DependencyGraph.cpp
    Private/RuntimeResourcePolicy.cpp
    Private/RenderUploadRequestBuilder.cpp
    Private/ResourceRetirementLedger.cpp
    Private/ResourceManager.cpp
    Private/ResourceRuntime/ResourceLoaderCatalog.cpp
    Private/ResourceSubsystem.cpp
    Private/ModelTextureStreamingService.cpp
    Private/DefaultResources.cpp
    Private/Cooked/CookedAnimationArtifact.cpp
    Private/Cooked/CookedAudioArtifact.cpp
    Private/Cooked/CookedEnvironmentArtifact.cpp
    Private/Cooked/CookedModelArtifact.cpp
    
    # Concrete resource types
    Private/Types/MeshResource.cpp
    Private/Types/TextureResource.cpp
    Private/Types/ShaderResource.cpp
    Private/Types/MaterialResource.cpp
    Private/Types/MaterialInstanceResource.cpp
    Private/Types/ModelResource.cpp
    Private/Types/AudioResource.cpp
    Private/Types/AnimationResource.cpp
    Private/Types/EnvironmentResource.cpp

    # Loaders
    Private/Loader/MeshLoader.cpp
    Private/Loader/ShaderLoader.cpp
    Private/Loader/TextureLoader.cpp
    Private/Loader/ModelLoader.cpp
    Private/Loader/AnimationLoader.cpp
    Private/Loader/CookedAudioLoader.cpp
    Private/Loader/CookedEnvironmentLoader.cpp
    Private/Loader/HDRTextureLoader.cpp
    Private/Loader/EnvironmentLoader.cpp

    # Importers
    Private/Importer/GLTFImporter.cpp
    Private/Importer/FBXImporter.cpp

)

target_include_directories(RVX_ResourceRuntime PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Include>
    $<INSTALL_INTERFACE:include>
)

# Find tinygltf for glTF/GLB loading
find_package(unofficial-tinygltf CONFIG QUIET)
if(NOT unofficial-tinygltf_FOUND)
    # Try to find tinygltf without the unofficial prefix
    find_package(tinygltf CONFIG QUIET)
endif()

target_link_libraries(RVX_ResourceRuntime PUBLIC
    RVX::AssetContracts
    RVX_Core
    RVX::Animation
    RVX::Geometry
    RVX::RenderContracts
)

target_link_libraries(RVX_ResourceRuntime PRIVATE
    RVX::ShaderContracts
)

# Link tinygltf if found
if(unofficial-tinygltf_FOUND)
    target_link_libraries(RVX_ResourceRuntime PRIVATE unofficial::tinygltf::tinygltf)
    message(STATUS "RVX_ResourceRuntime: Using unofficial-tinygltf")
elseif(tinygltf_FOUND)
    target_link_libraries(RVX_ResourceRuntime PRIVATE tinygltf::tinygltf)
    message(STATUS "RVX_ResourceRuntime: Using tinygltf")
else()
    # tinygltf is header-only, so we can include it directly if available
    find_path(TINYGLTF_INCLUDE_DIR tiny_gltf.h PATH_SUFFIXES tinygltf)
    if(TINYGLTF_INCLUDE_DIR)
        target_include_directories(RVX_ResourceRuntime PRIVATE ${TINYGLTF_INCLUDE_DIR})
        message(STATUS "RVX_ResourceRuntime: Using header-only tinygltf from ${TINYGLTF_INCLUDE_DIR}")
    else()
        message(WARNING "RVX_ResourceRuntime: tinygltf not found. glTF loading will not be available.")
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
        message(STATUS "RVX_ResourceRuntime: Fetching ufbx...")
        FetchContent_Populate(ufbx)
    endif()

    if(EXISTS "${ufbx_SOURCE_DIR}/ufbx.h" AND EXISTS "${ufbx_SOURCE_DIR}/ufbx.c")
        target_sources(RVX_ResourceRuntime PRIVATE "${ufbx_SOURCE_DIR}/ufbx.c")
        target_include_directories(RVX_ResourceRuntime PRIVATE "${ufbx_SOURCE_DIR}")
        target_compile_definitions(RVX_ResourceRuntime PRIVATE HAS_UFBX=1)
        message(STATUS "RVX_ResourceRuntime: Using ufbx from ${ufbx_SOURCE_DIR}")
    else()
        message(WARNING "RVX_ResourceRuntime: ufbx not found. FBX loading will not be available.")
        message(STATUS "RVX_ResourceRuntime: To disable FBX support, use -DRVX_ENABLE_FBX=OFF")
    endif()
else()
    message(STATUS "RVX_ResourceRuntime: FBX loading disabled (RVX_ENABLE_FBX=OFF)")
endif()

# Find tinyexr for EXR loading (header-only)
find_path(TINYEXR_INCLUDE_DIR tinyexr.h PATH_SUFFIXES tinyexr)
if(TINYEXR_INCLUDE_DIR)
    target_include_directories(RVX_ResourceRuntime PRIVATE ${TINYEXR_INCLUDE_DIR})
    target_compile_definitions(RVX_ResourceRuntime PRIVATE HAS_TINYEXR=1)
    message(STATUS "RVX_ResourceRuntime: Using tinyexr from ${TINYEXR_INCLUDE_DIR}")
    
    # tinyexr uses miniz for ZIP compression/decompression.
    find_package(miniz CONFIG QUIET)
    if(miniz_FOUND)
        target_link_libraries(RVX_ResourceRuntime PRIVATE miniz::miniz)
        message(STATUS "RVX_ResourceRuntime: Using miniz target")
    else()
        find_path(MINIZ_INCLUDE_DIR miniz.h PATH_SUFFIXES miniz)
        if(MINIZ_INCLUDE_DIR)
            target_include_directories(RVX_ResourceRuntime PRIVATE ${MINIZ_INCLUDE_DIR})
            message(WARNING "RVX_ResourceRuntime: miniz library target not found. EXR loading may fail to link.")
        else()
            message(WARNING "RVX_ResourceRuntime: miniz not found. EXR loading may fail.")
        endif()
    endif()
else()
    message(STATUS "RVX_ResourceRuntime: tinyexr not found. EXR loading will not be available (HDR still works).")
endif()

add_library(RVX::ResourceRuntime ALIAS RVX_ResourceRuntime)

rvx_register_module_target(
    TARGET RVX_ResourceRuntime
    MODULE ResourceRuntime
    ALIASES RVX::ResourceRuntime
)
