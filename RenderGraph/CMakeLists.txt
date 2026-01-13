# =============================================================================
# RenderGraph Module - Frame graph and automatic resource management
# =============================================================================
add_library(RVX_RenderGraph STATIC)

target_sources(RVX_RenderGraph PRIVATE
    Private/RenderGraph.cpp
    Private/RenderGraphCompiler.cpp
    Private/RenderGraphExecutor.cpp
    Private/FrameResourceManager.cpp
)

target_include_directories(RVX_RenderGraph PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(RVX_RenderGraph PUBLIC
    RVX::RHI
    RVX::ShaderCompiler
)

add_library(RVX::RenderGraph ALIAS RVX_RenderGraph)
