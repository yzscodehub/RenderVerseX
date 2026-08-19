# =============================================================================
# Engine Module - Main engine and subsystem coordinator
# =============================================================================
# The Engine module coordinates all other modules and provides the main
# application lifecycle management.
#
# Module Architecture (after refactoring):
# - HAL: Hardware abstraction (Window, Input backend)
# - Runtime: Runtime services (Window, Input, Time, Camera subsystems)
# - World: ownership of the single Scene ECS runtime
# - Render: Rendering (RenderGraph, RenderSubsystem)
# - Resource: Asset management (ResourceSubsystem)
# =============================================================================
add_library(RVX_Engine STATIC)

target_sources(RVX_Engine PRIVATE
    Private/Engine.cpp
    Private/ECS/EcsRenderFramePipeline.cpp
    Private/ECS/EcsRenderEntityRetirementCoordinator.cpp
    Private/ECS/EcsSceneRenderProofGateway.cpp
    Private/ECS/EcsRenderRuntimeComposition.cpp
    Private/ECS/WorldEcsRuntimeComposition.cpp
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
    RVX::Physics
    RVX_World
    
    # Rendering
    RVX_Render
    RVX_RenderContracts
    RVX_RenderExtraction
    
    # Resources
    RVX_Resource
    RVX_ResourceSceneAdapters
    RVX::AnimationSceneAdapters
    
)

target_link_libraries(RVX_Engine PRIVATE
    RVX::Audio
    RVX::Particle
    RVX::PhysicsSceneAdapters
    RVX::Scripting
    RVX::Terrain
    RVX::Water
)

add_library(RVX::Engine ALIAS RVX_Engine)
