# =============================================================================
# World Module - pure Scene ECS runtime ownership and camera authoring
# =============================================================================
# Engine owns execution composition, fixed-step policy, and rendering.  World
# owns only SceneEcsRuntime, WorldEcsCameraService, and its value config.
# =============================================================================
add_library(RVX_World STATIC)

target_sources(RVX_World PRIVATE
    Private/ECS/WorldEcsCameraService.cpp
    Private/World.cpp
)

target_include_directories(RVX_World PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(RVX_World PUBLIC
    RVX_Core
    RVX::Physics
    RVX_Scene
)

add_library(RVX::World ALIAS RVX_World)
