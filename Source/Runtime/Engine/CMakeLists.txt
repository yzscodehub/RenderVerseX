# =============================================================================
# Engine Module - Main engine and subsystem coordinator
# =============================================================================
# The Engine module is the top-level Runtime composition. Developer and
# Shipping roots inject its move-only dependencies; Engine never discovers
# source importers or shader compilers through hidden fallback state.
#
# RenderGraph is an independent link target owned by the Render Manifest
# module. World owns ECS state; Engine owns the per-World Runtime composition.
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
    RVX_RuntimeServices
    
    # World & Scene
    RVX::Physics
    RVX_World
    
    # Rendering
    RVX::Render
    RVX::RenderComposition
    RVX::RenderContracts
    RVX::RenderExtraction
    RVX::ShaderContracts
    
    # Resources
    RVX::ResourceRuntime
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

rvx_register_module_target(
    TARGET RVX_Engine
    MODULE Engine
    ALIASES RVX::Engine
)

if(RVX_BUILD_SHIPPING_PRODUCTS)
    add_library(RVXShippingProduct STATIC
        Private/ShippingRuntimeComposition.cpp
    )

    target_include_directories(RVXShippingProduct PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Include>
        $<INSTALL_INTERFACE:include>
    )

    target_link_libraries(RVXShippingProduct
        PUBLIC
            RVX::Engine
        PRIVATE
            RVX::ResourceRuntime
            RVX::ShaderRuntime
    )

    target_compile_features(RVXShippingProduct PUBLIC cxx_std_20)

    rvx_register_module_target(
        TARGET RVXShippingProduct
        MODULE Engine
    )
endif()
