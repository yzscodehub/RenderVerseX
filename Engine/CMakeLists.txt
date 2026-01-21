# =============================================================================
# Engine Module - Main engine and subsystem coordinator
# =============================================================================
add_library(RVX_Engine STATIC)

target_sources(RVX_Engine PRIVATE
    Private/Engine.cpp
)

target_include_directories(RVX_Engine PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(RVX_Engine PUBLIC
    RVX_Core
    RVX_Runtime
    RVX_Platform
    RVX_Input
    RVX_Camera
    RVX_RenderGraph
    RVX_RHI
)

add_library(RVX::Engine ALIAS RVX_Engine)
