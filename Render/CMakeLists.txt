# =============================================================================
# Render Module - Render entry and swapchain management
# =============================================================================
add_library(RVX_Render STATIC)

target_sources(RVX_Render PRIVATE
    Private/RenderService.cpp
    Private/SwapChainManager.cpp
)

target_include_directories(RVX_Render PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(RVX_Render PUBLIC
    RVX_Core
    RVX_RHI
    RVX_RenderGraph
    RVX_Camera
)

add_library(RVX::Render ALIAS RVX_Render)
