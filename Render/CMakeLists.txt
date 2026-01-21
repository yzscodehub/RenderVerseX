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
add_library(RVX_Render STATIC)

target_sources(RVX_Render PRIVATE
    # Core
    Private/RenderService.cpp
    Private/SwapChainManager.cpp
    Private/RenderSubsystem.cpp
    Private/GPUResourceManager.cpp
    Private/PipelineCache.cpp
    # Context
    Private/Context/RenderContext.cpp
    Private/Context/FrameSynchronizer.cpp
    # Renderer
    Private/Renderer/ViewData.cpp
    Private/Renderer/RenderScene.cpp
    Private/Renderer/SceneRenderer.cpp
    # Passes
    Private/Passes/OpaquePass.cpp
    # Graph (merged from RenderGraph module)
    Private/Graph/RenderGraph.cpp
    Private/Graph/RenderGraphCompiler.cpp
    Private/Graph/RenderGraphExecutor.cpp
    Private/Graph/FrameResourceManager.cpp
)

target_include_directories(RVX_Render PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Private>
    $<INSTALL_INTERFACE:include>
)

# Private access to Engine, World, Resource, Scene headers for subsystem integration
# Note: Engine links to Render, so we only add include dirs, not link dependencies
target_include_directories(RVX_Render PRIVATE
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/Engine/Include>
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/World/Include>
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/Resource/Include>
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/Scene/Include>
)

target_link_libraries(RVX_Render PUBLIC
    RVX_Core
    RVX_RHI
    RVX_Runtime
    Spatial
    RVX::ShaderCompiler
)

add_library(RVX::Render ALIAS RVX_Render)

# Backward compatibility alias for code that still uses RVX::RenderGraph
add_library(RVX::RenderGraph ALIAS RVX_Render)
