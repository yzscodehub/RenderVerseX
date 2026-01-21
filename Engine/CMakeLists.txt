# =============================================================================
# Engine Module - Main engine and subsystem coordinator
# =============================================================================
# The Engine module coordinates all other modules and provides the main
# application lifecycle management.
#
# Module Architecture (after refactoring):
# - HAL: Hardware abstraction (Window, Input backend)
# - Runtime: Runtime services (Window, Input, Time, Camera subsystems)
# - World: Scene management (Scene, Spatial, Picking)
# - Render: Rendering (RenderGraph, RenderSubsystem)
# - Resource: Asset management (ResourceSubsystem)
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
    # Foundation
    RVX_Core
    
    # Hardware Abstraction
    RVX_HAL
    RVX_RHI
    
    # Runtime Services
    RVX_Runtime
    
    # World & Scene
    RVX_World
    
    # Rendering
    RVX_Render
    
    # Resources
    RVX_Resource
    
)

add_library(RVX::Engine ALIAS RVX_Engine)
