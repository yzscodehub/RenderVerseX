# =============================================================================
# RenderExtraction Module - Scene/World to render contract extraction
# =============================================================================
add_library(RVX_RenderExtraction STATIC)

target_sources(RVX_RenderExtraction PRIVATE
    Private/RenderFramePacketBuilder.cpp
    Private/RenderFeatureSceneBridge.cpp
    Private/RenderProxySceneBridge.cpp
    Private/SceneEnvironmentIBLBridge.cpp
    Private/SceneSkyboxPassBridge.cpp
    Private/WorldCameraBridge.cpp
)

target_include_directories(RVX_RenderExtraction PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Include>
    $<INSTALL_INTERFACE:include>
)

target_include_directories(RVX_RenderExtraction PRIVATE
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/Resource/Include>
)

target_link_libraries(RVX_RenderExtraction PUBLIC
    RVX::Core
    RVX::RenderContracts
    RVX::Resource
    RVX::Scene
    RVX::World
)

add_library(RVX::RenderExtraction ALIAS RVX_RenderExtraction)
