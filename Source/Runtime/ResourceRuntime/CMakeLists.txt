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
# - Concrete runtime resource types and strict cooked artifact readers
# - Runtime scheduling, publication, caching, and retirement orchestration
#
# Source parsers, importers, hot-reload monitoring, and source decode
# implementations belong to the Developer-side AssetImport/ResourceSource
# composition and are intentionally absent from this target.
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
    Private/Cooked/CookedMaterialArtifact.cpp
    Private/Cooked/CookedModelArtifact.cpp
    Private/Cooked/CookedParticleEffectArtifact.cpp
    Private/Cooked/CookedTextureArtifact.cpp

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
    Private/Loader/CookedTextureLoader.cpp
    Private/Loader/CookedMaterialPreparation.cpp
    Private/Loader/CookedMaterialLoader.cpp
    Private/Loader/CookedModelLoader.cpp
    Private/Loader/AnimationLoader.cpp
    Private/Loader/CookedAudioLoader.cpp
    Private/Loader/CookedEnvironmentLoader.cpp
)

target_include_directories(RVX_ResourceRuntime PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Include>
    $<INSTALL_INTERFACE:include>
)

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

add_library(RVX::ResourceRuntime ALIAS RVX_ResourceRuntime)

rvx_register_module_target(
    TARGET RVX_ResourceRuntime
    MODULE ResourceRuntime
    ALIASES RVX::ResourceRuntime
)
