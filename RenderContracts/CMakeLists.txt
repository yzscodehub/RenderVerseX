# =============================================================================
# RenderContracts Module - render extraction data contracts
# =============================================================================
add_library(RVX_RenderContracts STATIC)

target_sources(RVX_RenderContracts PRIVATE
    Private/RenderFramePacket.cpp
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
