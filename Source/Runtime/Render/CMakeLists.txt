# =============================================================================
# Render Module - Rendering coordination and subsystem
# =============================================================================
# Provides:
# - RenderSubsystem (main render coordinator)
# - RenderContext (RHI device and swap chain management)
# - FrameSynchronizer (multi-frame GPU synchronization)
# - RenderGraph (frame graph and automatic resource management)
# - SceneRenderer, ViewData, RenderScene
# - IRenderPass interface and built-in passes
# =============================================================================
add_library(RVX_RenderRuntimeCore STATIC)

find_package(Threads REQUIRED)

target_sources(RVX_RenderRuntimeCore PRIVATE
    Private/Runtime/DedicatedRenderExecutor.cpp
    Private/Runtime/RenderControlMailbox.cpp
    Private/Runtime/RenderDiagnosticsPublisher.cpp
    Private/Runtime/RenderReleaseQueue.cpp
    Private/Runtime/RenderResourceGateway.cpp
    Private/Runtime/RenderResourceReservationDirectory.cpp
    Private/Runtime/RenderResourceStatusTable.cpp
    Private/Runtime/RenderSceneUpdateQueue.cpp
    Private/Runtime/RenderUploadQueue.cpp
    Private/Runtime/RenderThreadGuard.cpp
    Private/Runtime/RenderViewHistoryCoordinator.cpp
    Private/Material/MaterialGPUEncoding.cpp
    Private/Resources/RenderRetirementQueue.cpp
    Private/Resources/RenderResourceRegistry.cpp
    Private/Resources/RenderSubmissionResourceBatch.cpp
    Private/Resources/RenderSubmissionTracker.cpp
    Private/Resources/RenderUploadProcessor.cpp
)

if(APPLE)
    target_sources(RVX_RenderRuntimeCore PRIVATE
        Private/Runtime/RenderThreadPlatformApple.mm
    )
    set_source_files_properties(
        Private/Runtime/RenderThreadPlatformApple.mm
        PROPERTIES COMPILE_OPTIONS "-fno-objc-arc"
    )
    find_library(RVX_FOUNDATION_FRAMEWORK Foundation REQUIRED)
    target_link_libraries(RVX_RenderRuntimeCore PRIVATE
        ${RVX_FOUNDATION_FRAMEWORK}
    )
else()
    target_sources(RVX_RenderRuntimeCore PRIVATE
        Private/Runtime/RenderThreadPlatform.cpp
    )
endif()

target_include_directories(RVX_RenderRuntimeCore PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Include>
)

target_include_directories(RVX_RenderRuntimeCore PRIVATE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Private>
)

target_link_libraries(RVX_RenderRuntimeCore PUBLIC
    RVX::Core
    RVX::RenderContracts
    RVX::RHI
)

target_link_libraries(RVX_RenderRuntimeCore PRIVATE
    Threads::Threads
)

add_library(RVX::RenderRuntimeCore ALIAS RVX_RenderRuntimeCore)

# =============================================================================
# RenderGraph - frame graph compilation, execution, and transient resources
# =============================================================================
add_library(RVX_RenderGraph STATIC)

target_sources(RVX_RenderGraph PRIVATE
    Private/Graph/RenderGraph.cpp
    Private/Graph/RenderGraphCompiler.cpp
    Private/Graph/RenderGraphExecutor.cpp
    Private/Graph/RenderGraphExecution.cpp
    Private/Graph/TransientResourcePool.cpp
    Private/Graph/ResourceViewCache.cpp
)

target_include_directories(RVX_RenderGraph PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Include>
    $<INSTALL_INTERFACE:include>
)

target_include_directories(RVX_RenderGraph PRIVATE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Private>
)

target_link_libraries(RVX_RenderGraph PUBLIC
    RVX::Core
    RVX::RHI
    RVX_RenderRuntimeCore
)

add_library(RVX::RenderGraph ALIAS RVX_RenderGraph)

function(rvx_configure_render_component target)
    target_include_directories(${target} PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Include>
        $<INSTALL_INTERFACE:include>
    )

    target_include_directories(${target} PRIVATE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Private>
    )
endfunction()

# =============================================================================
# RenderFoundation - shared renderer state and execution planning
# =============================================================================
# Foundation owns data and policies shared by the raster and optional feature
# targets.  It deliberately contains no feature composition or pass ordering.
add_library(RVX_RenderFoundation STATIC)

target_sources(RVX_RenderFoundation PRIVATE
    Private/Runtime/RenderThreadRuntime.cpp
    Private/GPUUploadService.cpp
    Private/PipelineCache.cpp
    Private/Context/RenderContext.cpp
    Private/Context/RenderFrameTimingOwner.cpp
    Private/Context/FrameSynchronizer.cpp
    Private/Renderer/ViewData.cpp
    Private/Renderer/RenderDrawPacket.cpp
    Private/Renderer/RenderDrawPacketCache.cpp
    Private/Renderer/RenderScene.cpp
    Private/Renderer/RenderSceneDatabase.cpp
    Private/Renderer/RenderDrawItem.cpp
    Private/Renderer/RenderDrawPlan.cpp
    Private/Animation/RenderDeformationFrameResources.cpp
    Private/GPUScene/GPUSceneDatabase.cpp
    Private/GPUScene/GPUSceneUpdate.cpp
    Private/GPUScene/GPUSceneUploader.cpp
    Private/Visibility/RenderVisibility.cpp
    Private/Policy/RenderFramePlanCompiler.cpp
    Private/Policy/RenderPolicyResolver.cpp
    Private/Submission/RenderSubmissionStrategy.cpp
    Private/Submission/RenderInstanceBatchPlan.cpp
    Private/Submission/RasterInstanceStream.cpp
    Private/Submission/DirectRasterReadbackQualification.cpp
    Private/Resources/FrameConstantUploadArena.cpp
    Private/Passes/DirectDrawPacketBatch.cpp
    Private/Passes/MeshPassProcessor.cpp
    Private/Passes/DepthMeshPassProcessor.cpp
    Private/Passes/OpaqueMeshPassProcessor.cpp
    Private/Passes/TransparentMeshPassProcessor.cpp
    Private/Passes/ShadowMeshPassProcessor.cpp
    Private/Passes/MaterialTextureGraphBindings.cpp
    Private/Material/MaterialBinder.cpp
    Private/Material/MaterialClassification.cpp
    Private/Material/MaterialSystem.cpp
    Private/Lighting/LightManager.cpp
    Private/Lighting/ClusteredLighting.cpp
    Private/Lighting/LightingFramePlan.cpp
    Private/Lighting/LocalShadowFramePlan.cpp
    Private/Lighting/LocalShadowAtlas.cpp
    Private/Reflection/ReflectionFramePlan.cpp
    Private/Translucency/TranslucencyFramePlan.cpp
    Private/GPUDriven/GPUCulling.cpp
)

rvx_configure_render_component(RVX_RenderFoundation)

target_link_libraries(RVX_RenderFoundation PUBLIC
    RVX::Core
    RVX::HAL
    RVX::RenderContracts
    RVX::RenderGraph
    RVX::RHI
    RVX::ShaderContracts
    RVX::RenderRuntimeCore
)

target_link_libraries(RVX_RenderFoundation PRIVATE
    RVX::RHI_BackendFactory
    Threads::Threads
)

add_library(RVX::RenderFoundation ALIAS RVX_RenderFoundation)

# =============================================================================
# Renderer feature targets
# =============================================================================
# Each feature may consume the foundation and RenderGraph contracts, but never
# another feature target. RenderComposition is the sole feature aggregator.
add_library(RVX_RenderSurface STATIC)
target_sources(RVX_RenderSurface PRIVATE
    Private/Passes/OpaquePass.cpp
    Private/Submission/OpaqueRasterSubmission.cpp
    Private/Passes/DepthPrepass.cpp
    Private/Passes/TransparentPass.cpp
    Private/Passes/SkyboxPass.cpp
    Private/Passes/TerrainPass.cpp
    Private/Passes/UIPass.cpp
    Private/Decal/DecalRenderer.cpp
    Private/Sky/AtmosphericScattering.cpp
    Private/Debug/DebugRenderer.cpp
    Private/Debug/GPUProfiler.cpp
)
rvx_configure_render_component(RVX_RenderSurface)
target_link_libraries(RVX_RenderSurface PUBLIC
    RVX::RenderFoundation
    RVX::RenderGraph
)
add_library(RVX::RenderSurface ALIAS RVX_RenderSurface)

add_library(RVX_RenderLighting STATIC)
target_sources(RVX_RenderLighting PRIVATE
    Private/Passes/ShadowPass.cpp
    Private/Passes/LocalShadowPass.cpp
    Private/Passes/DeferredLightingPass.cpp
    Private/Lighting/FroxelVolumetricLighting.cpp
    Private/Lighting/GroundTruthAmbientOcclusion.cpp
    Private/Lighting/VolumetricLightingFramePlan.cpp
)
rvx_configure_render_component(RVX_RenderLighting)
target_link_libraries(RVX_RenderLighting PUBLIC
    RVX::RenderFoundation
    RVX::RenderGraph
)
add_library(RVX::RenderLighting ALIAS RVX_RenderLighting)

add_library(RVX_RenderAnimationDeform STATIC)
target_sources(RVX_RenderAnimationDeform PRIVATE
    Private/Passes/CameraVelocityPass.cpp
    Private/Passes/ObjectVelocityPass.cpp
)
rvx_configure_render_component(RVX_RenderAnimationDeform)
target_link_libraries(RVX_RenderAnimationDeform PUBLIC
    RVX::RenderFoundation
    RVX::RenderGraph
)
add_library(RVX::RenderAnimationDeform ALIAS RVX_RenderAnimationDeform)

add_library(RVX_RenderPostProcess STATIC)
target_sources(RVX_RenderPostProcess PRIVATE
    Private/PostProcess/Exposure.cpp
    Private/PostProcess/PostProcessStack.cpp
    Private/PostProcess/ScreenSpaceOutline.cpp
    Private/PostProcess/ToneMapping.cpp
    Private/PostProcess/Bloom.cpp
    Private/PostProcess/FXAA.cpp
    Private/PostProcess/SSR.cpp
    Private/PostProcess/TAA.cpp
    Private/PostProcess/DOF.cpp
    Private/PostProcess/MotionBlur.cpp
    Private/PostProcess/ColorGrading.cpp
    Private/PostProcess/Vignette.cpp
    Private/PostProcess/ChromaticAberration.cpp
    Private/PostProcess/FilmGrain.cpp
)
rvx_configure_render_component(RVX_RenderPostProcess)
target_link_libraries(RVX_RenderPostProcess PUBLIC
    RVX::RenderFoundation
    RVX::RenderGraph
)
add_library(RVX::RenderPostProcess ALIAS RVX_RenderPostProcess)

add_library(RVX_RenderParticles STATIC)
target_sources(RVX_RenderParticles PRIVATE
    Private/Particles/ParticleBillboardDrawPlan.cpp
    Private/Passes/ParticleFeaturePass.cpp
)
rvx_configure_render_component(RVX_RenderParticles)
target_link_libraries(RVX_RenderParticles PUBLIC
    RVX::RenderFoundation
    RVX::RenderGraph
)
add_library(RVX::RenderParticles ALIAS RVX_RenderParticles)

add_library(RVX_RenderWater STATIC)
target_sources(RVX_RenderWater PRIVATE
    Private/Passes/WaterFeaturePass.cpp
)
rvx_configure_render_component(RVX_RenderWater)
target_link_libraries(RVX_RenderWater PUBLIC
    RVX::RenderFoundation
    RVX::RenderGraph
)
add_library(RVX::RenderWater ALIAS RVX_RenderWater)

add_library(RVX_RenderRayTracing STATIC)
target_sources(RVX_RenderRayTracing PRIVATE
    Private/RayTracing/HybridRayTracingFramePlan.cpp
    Private/RayTracing/RayTracingScene.cpp
    Private/RayTracing/RayTracingSceneManager.cpp
    Private/Passes/RayTracedShadowPass.cpp
    Private/Passes/RayTracedReflectionPass.cpp
    Private/Passes/RayTracedReflectionDenoisePass.cpp
    Private/Passes/RayTracedReflectionCompositePass.cpp
)
rvx_configure_render_component(RVX_RenderRayTracing)
target_link_libraries(RVX_RenderRayTracing PUBLIC
    RVX::RenderFoundation
    RVX::RenderGraph
)
add_library(RVX::RenderRayTracing ALIAS RVX_RenderRayTracing)

# =============================================================================
# RenderComposition - the one place that assembles renderer features
# =============================================================================
add_library(RVX_RenderComposition STATIC)
target_sources(RVX_RenderComposition PRIVATE
    Private/Renderer/RenderPassRegistry.cpp
    Private/Renderer/SceneRenderer.cpp
    Private/Features/DefaultRenderComposition.cpp
    Private/Features/DefaultForwardComposition.cpp
    Private/Features/DefaultRenderPassBundle.cpp
    Private/Features/AmbientOcclusionFeatureAdapter.cpp
    Private/Features/DeferredRecipeFeatureAdapter.cpp
    Private/Features/LegacyForwardFeatureAdapter.cpp
    Private/Features/ReflectionFeatureAdapter.cpp
    Private/Features/TranslucencyFeatureAdapter.cpp
    Private/Features/VolumetricLightingFeatureAdapter.cpp
    Private/Features/PostProcessPipelineFeatureAdapter.cpp
    Private/Features/RenderFeatureRegistry.cpp
    Private/Diagnostics/RenderFeatureReport.cpp
    Private/Diagnostics/RenderToolArtifacts.cpp
)
rvx_configure_render_component(RVX_RenderComposition)
target_link_libraries(RVX_RenderComposition PUBLIC
    RVX::RenderFoundation
    RVX::RenderGraph
    RVX::RenderSurface
    RVX::RenderLighting
    RVX::RenderAnimationDeform
    RVX::RenderPostProcess
    RVX::RenderParticles
    RVX::RenderWater
    RVX::RenderRayTracing
)
add_library(RVX::RenderComposition ALIAS RVX_RenderComposition)

# =============================================================================
# Render facade - public subsystem and presentation entry points only
# =============================================================================
add_library(RVX_Render STATIC)
target_sources(RVX_Render PRIVATE
    Private/RenderSubsystem.cpp
    Private/SwapChainManager.cpp
)
rvx_configure_render_component(RVX_Render)
target_link_libraries(RVX_Render PUBLIC
    RVX::RenderComposition
)
add_library(RVX::Render ALIAS RVX_Render)

rvx_register_module_target(
    TARGET RVX_RenderRuntimeCore
    MODULE Render
    ALIASES RVX::RenderRuntimeCore
)
rvx_register_module_target(
    TARGET RVX_RenderGraph
    MODULE Render
    ALIASES RVX::RenderGraph
)
rvx_register_module_target(
    TARGET RVX_RenderFoundation
    MODULE Render
    ALIASES RVX::RenderFoundation
)
rvx_register_module_target(
    TARGET RVX_RenderSurface
    MODULE Render
    ALIASES RVX::RenderSurface
)
rvx_register_module_target(
    TARGET RVX_RenderLighting
    MODULE Render
    ALIASES RVX::RenderLighting
)
rvx_register_module_target(
    TARGET RVX_RenderAnimationDeform
    MODULE Render
    ALIASES RVX::RenderAnimationDeform
)
rvx_register_module_target(
    TARGET RVX_RenderPostProcess
    MODULE Render
    ALIASES RVX::RenderPostProcess
)
rvx_register_module_target(
    TARGET RVX_RenderParticles
    MODULE Render
    ALIASES RVX::RenderParticles
)
rvx_register_module_target(
    TARGET RVX_RenderWater
    MODULE Render
    ALIASES RVX::RenderWater
)
rvx_register_module_target(
    TARGET RVX_RenderRayTracing
    MODULE Render
    ALIASES RVX::RenderRayTracing
)
rvx_register_module_target(
    TARGET RVX_RenderComposition
    MODULE Render
    ALIASES RVX::RenderComposition
)
rvx_register_module_target(
    TARGET RVX_Render
    MODULE Render
    ALIASES RVX::Render
)
