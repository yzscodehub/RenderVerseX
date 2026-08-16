# =============================================================================
# AnimationSceneAdapters Module - pure ECS adapters for animation runtime
# =============================================================================
add_library(RVX_AnimationSceneAdapters STATIC)

target_sources(RVX_AnimationSceneAdapters PRIVATE
    Private/ECS/AnimationEcsBridge.cpp
    Private/ECS/EcsAnimationAssetService.cpp
    Private/ECS/ResourceAnimationEcsEvaluator.cpp
)

target_include_directories(RVX_AnimationSceneAdapters PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(RVX_AnimationSceneAdapters PUBLIC
    RVX::Animation
    RVX::Core
    RVX::ECS
    RVX::Resource
    RVX::ResourceSceneAdapters
    RVX::Scene
)

target_compile_features(RVX_AnimationSceneAdapters PUBLIC cxx_std_20)

add_library(RVX::AnimationSceneAdapters ALIAS RVX_AnimationSceneAdapters)
