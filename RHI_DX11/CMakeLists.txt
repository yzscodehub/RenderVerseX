# =============================================================================
# RHI_DX11 Module - DirectX 11 backend implementation
# =============================================================================
add_library(RVX_RHI_DX11 STATIC)

target_sources(RVX_RHI_DX11 PRIVATE
    Private/DX11Device.cpp
    Private/DX11CommandContext.cpp
    Private/DX11Resources.cpp
    Private/DX11Pipeline.cpp
    Private/DX11BindingRemapper.cpp
    Private/DX11SwapChain.cpp
)

target_include_directories(RVX_RHI_DX11 PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(RVX_RHI_DX11 PUBLIC
    RVX::RHI
    RVX::ShaderCompiler
)

# Windows SDK libraries
target_link_libraries(RVX_RHI_DX11 PRIVATE
    d3d11
    dxgi
    dxguid
)

add_library(RVX::RHI_DX11 ALIAS RVX_RHI_DX11)
