# =============================================================================
# RHI_DX11 Module - DirectX 11 backend implementation
# =============================================================================
add_library(RVX_RHI_DX11 STATIC)

# Header files (for IDE visibility)
set(DX11_HEADERS
    Include/DX11/DX11Device.h
    Private/DX11Common.h
    Private/DX11Conversions.h
    Private/DX11Debug.h
    Private/DX11Device.h
    Private/DX11Resources.h
    Private/DX11Pipeline.h
    Private/DX11BindingRemapper.h
    Private/DX11StateCache.h
    Private/DX11CommandContext.h
    Private/DX11SwapChain.h
)

# Source files
set(DX11_SOURCES
    Private/DX11Debug.cpp
    Private/DX11Device.cpp
    Private/DX11Resources.cpp
    Private/DX11Pipeline.cpp
    Private/DX11BindingRemapper.cpp
    Private/DX11StateCache.cpp
    Private/DX11CommandContext.cpp
    Private/DX11SwapChain.cpp
)

target_sources(RVX_RHI_DX11 PRIVATE
    ${DX11_HEADERS}
    ${DX11_SOURCES}
)

target_include_directories(RVX_RHI_DX11 PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Include>
    $<INSTALL_INTERFACE:include>
)

target_include_directories(RVX_RHI_DX11 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/Private
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
    d3dcompiler  # For shader reflection (optional)
)

# Debug definitions
target_compile_definitions(RVX_RHI_DX11 PRIVATE
    $<$<CONFIG:Debug>:RVX_DX11_DEBUG=1>
)

# Enable DX11 in RHI module
target_compile_definitions(RVX_RHI_DX11 PUBLIC
    RVX_ENABLE_DX11=1
)

add_library(RVX::RHI_DX11 ALIAS RVX_RHI_DX11)

# =============================================================================
# Source Groups for IDE
# =============================================================================
source_group("Include" FILES Include/DX11/DX11Device.h)
source_group("Private\\Headers" FILES
    Private/DX11Common.h
    Private/DX11Conversions.h
    Private/DX11Debug.h
    Private/DX11Device.h
    Private/DX11Resources.h
    Private/DX11Pipeline.h
    Private/DX11BindingRemapper.h
    Private/DX11StateCache.h
    Private/DX11CommandContext.h
    Private/DX11SwapChain.h
)
source_group("Private\\Sources" FILES
    Private/DX11Debug.cpp
    Private/DX11Device.cpp
    Private/DX11Resources.cpp
    Private/DX11Pipeline.cpp
    Private/DX11BindingRemapper.cpp
    Private/DX11StateCache.cpp
    Private/DX11CommandContext.cpp
    Private/DX11SwapChain.cpp
)
