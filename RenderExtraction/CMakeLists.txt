# =============================================================================
# RenderExtraction Module - frozen ECS values to render contracts
# =============================================================================
add_library(RVX_RenderExtraction STATIC)

target_sources(RVX_RenderExtraction PRIVATE
    Private/ECS/EcsFrameExtractor.cpp
    Private/EcsFrozenSceneBridge.cpp
)

target_include_directories(RVX_RenderExtraction PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(RVX_RenderExtraction PUBLIC
    RVX::Core
    RVX::ECS
    RVX::RenderContracts
    RVX::Scene
)

add_library(RVX::RenderExtraction ALIAS RVX_RenderExtraction)
