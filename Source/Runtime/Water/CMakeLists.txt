# =============================================================================
# Water Module - deterministic gameplay-side Gerstner contracts
# =============================================================================
# Provides:
# - Pure ECS water configuration and runtime bridge
# - Immutable Gerstner surface descriptors and pure gameplay queries
# - ECS bridge that publishes renderer-neutral Water packets
# =============================================================================
add_library(RVX_Water STATIC)

target_sources(RVX_Water PRIVATE
    Include/Water/ECS/WaterEcsBridge.h
    Include/Water/ECS/WaterEcsRuntimeGateway.h
    Include/Water/GerstnerSurface.h
    Include/Water/Water.h
    Include/Water/WaterTypes.h
    Private/GerstnerSurface.cpp
    Private/ECS/WaterEcsBridge.cpp
    Private/ECS/WaterEcsRuntimeGateway.cpp
)

target_include_directories(RVX_Water PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Include>
    $<INSTALL_INTERFACE:include>
    PRIVATE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Private>
)

target_link_libraries(RVX_Water PUBLIC
    RVX_Core
    RVX::RenderContracts
    RVX::Scene
)

add_library(RVX::Water ALIAS RVX_Water)

rvx_register_module_target(
    TARGET RVX_Water
    MODULE Water
    ALIASES RVX::Water
)
