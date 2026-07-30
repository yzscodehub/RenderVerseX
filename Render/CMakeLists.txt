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
    Private/Runtime/RenderFrameMailbox.cpp
    Private/Runtime/RenderReleaseQueue.cpp
    Private/Runtime/RenderResourceGateway.cpp
    Private/Runtime/RenderResourceReservationDirectory.cpp
    Private/Runtime/RenderResourceStatusTable.cpp
    Private/Runtime/RenderUploadQueue.cpp
    Private/Runtime/RenderThreadGuard.cpp
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

add_library(RVX_Render STATIC)

target_sources(RVX_Render PRIVATE
    # Core
    Private/SwapChainManager.cpp
    Private/RenderSubsystem.cpp
    # Runtime shell
    Private/Runtime/RenderThreadRuntime.cpp
    Private/GPUUploadService.cpp
    Private/PipelineCache.cpp
    # Context
    Private/Context/RenderContext.cpp
    Private/Context/FrameSynchronizer.cpp
    # Renderer
    Private/Renderer/ViewData.cpp
    Private/Renderer/RenderScene.cpp
    Private/Renderer/RenderDrawItem.cpp
    Private/Renderer/RenderFrameResourceBinder.cpp
    Private/Renderer/RenderPassRegistry.cpp
    Private/Renderer/SceneRenderer.cpp
    # Diagnostics
    Private/Diagnostics/RenderFeatureReport.cpp
    Private/Diagnostics/RenderToolArtifacts.cpp
    # Ray tracing
    Private/RayTracing/RayTracingScene.cpp
    Private/RayTracing/RayTracingSceneManager.cpp
    # Passes
    Private/Passes/OpaquePass.cpp
    Private/Passes/DepthPrepass.cpp
    Private/Passes/TransparentPass.cpp
    Private/Passes/CameraVelocityPass.cpp
    Private/Passes/ObjectVelocityPass.cpp
    Private/Passes/ParticleFeaturePass.cpp
    Private/Passes/SkyboxPass.cpp
    Private/Passes/ShadowPass.cpp
    Private/Passes/TerrainPass.cpp
    Private/Passes/RayTracedShadowPass.cpp
    Private/Passes/RayTracedReflectionPass.cpp
    Private/Passes/RayTracedReflectionDenoisePass.cpp
    Private/Passes/RayTracedReflectionCompositePass.cpp
    Private/Passes/GBufferPass.cpp
    Private/Passes/WaterPass.cpp
    # Graph (merged from RenderGraph module)
    Private/Graph/RenderGraph.cpp
    Private/Graph/RenderGraphCompiler.cpp
    Private/Graph/RenderGraphExecutor.cpp
    Private/Graph/TransientResourcePool.cpp
    Private/Graph/ResourceViewCache.cpp
    # Material
    Private/Material/MaterialBinder.cpp
    Private/Material/MaterialClassification.cpp
    Private/Material/MaterialSystem.cpp
    Private/Material/MaterialTemplate.cpp
    Private/Material/MaterialInstance.cpp
    # Lighting
    Private/Lighting/LightManager.cpp
    Private/Lighting/ClusteredLighting.cpp
    # PostProcess
    Private/PostProcess/PostProcessStack.cpp
    Private/PostProcess/ToneMapping.cpp
    Private/PostProcess/Bloom.cpp
    Private/PostProcess/FXAA.cpp
    Private/PostProcess/SSAO.cpp
    Private/PostProcess/SSR.cpp
    Private/PostProcess/TAA.cpp
    Private/PostProcess/DOF.cpp
    Private/PostProcess/MotionBlur.cpp
    Private/PostProcess/ColorGrading.cpp
    Private/PostProcess/Vignette.cpp
    Private/PostProcess/ChromaticAberration.cpp
    Private/PostProcess/FilmGrain.cpp
    Private/PostProcess/VolumetricLighting.cpp
    # Decal
    Private/Decal/DecalRenderer.cpp
    # Sky
    Private/Sky/AtmosphericScattering.cpp
    # Debug
    Private/Debug/DebugRenderer.cpp
    Private/Debug/GPUProfiler.cpp
    # GPU Driven
    Private/GPUDriven/GPUCulling.cpp
)

target_include_directories(RVX_Render PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Include>
    $<INSTALL_INTERFACE:include>
)

target_include_directories(RVX_Render PRIVATE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Private>
)

target_link_libraries(RVX_Render PUBLIC
    RVX_Core
    RVX::RenderContracts
    RVX_RHI
    Spatial
    RVX::ShaderCompiler
)

target_link_libraries(RVX_Render PRIVATE
    RVX::RHI_BackendFactory
    RVX_RenderRuntimeCore
)

add_library(RVX::Render ALIAS RVX_Render)

# Backward compatibility alias for code that still uses RVX::RenderGraph
add_library(RVX::RenderGraph ALIAS RVX_Render)
