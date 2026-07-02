# =============================================================================
# RenderContracts Module - render extraction data contracts
# =============================================================================
add_library(RVX_RenderContracts INTERFACE)

target_include_directories(RVX_RenderContracts INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(RVX_RenderContracts INTERFACE
    RVX::Core
)

add_library(RVX::RenderContracts ALIAS RVX_RenderContracts)
