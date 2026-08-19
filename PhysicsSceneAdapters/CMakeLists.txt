# =============================================================================
# PhysicsSceneAdapters Module - pure ECS to Built-in Physics composition
# =============================================================================
add_library(RVX_PhysicsSceneAdapters STATIC)

target_sources(RVX_PhysicsSceneAdapters PRIVATE
    Private/ECS/PhysicsEcsBridge.cpp
)

target_include_directories(RVX_PhysicsSceneAdapters PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(RVX_PhysicsSceneAdapters PUBLIC
    RVX::ECS
    RVX::Physics
    RVX::Scene
)

target_compile_features(RVX_PhysicsSceneAdapters PUBLIC cxx_std_20)

add_library(RVX::PhysicsSceneAdapters ALIAS RVX_PhysicsSceneAdapters)
