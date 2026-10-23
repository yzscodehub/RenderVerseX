# =============================================================================
# RenderContracts Module - render extraction data contracts
# =============================================================================
add_library(RVX_RenderContracts STATIC)

target_sources(RVX_RenderContracts PRIVATE
    Private/RenderMaterial.cpp
    Private/RenderFramePacketV5.cpp
    Private/RenderFeatureChannelRegistry.cpp
    Private/RenderSceneUpdate.cpp
    Private/ResourceUploadRequest.cpp
)

target_include_directories(RVX_RenderContracts PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(RVX_RenderContracts PUBLIC
    RVX::Core
)

add_library(RVX::RenderContracts ALIAS RVX_RenderContracts)

rvx_register_module_target(
    TARGET RVX_RenderContracts
    MODULE RenderContracts
    ALIASES RVX::RenderContracts
)
