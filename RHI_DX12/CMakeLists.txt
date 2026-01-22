# =============================================================================
# RHI_DX12 Module - DirectX 12 backend implementation
# =============================================================================
add_library(RVX_RHI_DX12 STATIC)

target_sources(RVX_RHI_DX12 PRIVATE
    Private/DX12Device.cpp
    Private/DX12CommandContext.cpp
    Private/DX12CommandAllocatorPool.cpp
    Private/DX12Resources.cpp
    Private/DX12Pipeline.cpp
    Private/DX12PipelineCache.cpp
    Private/DX12Query.cpp
    Private/DX12DescriptorHeap.cpp
    Private/DX12SwapChain.cpp
)

target_include_directories(RVX_RHI_DX12 PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Include>
    $<INSTALL_INTERFACE:include>
)

target_include_directories(RVX_RHI_DX12 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/Private
)

target_link_libraries(RVX_RHI_DX12 PUBLIC
    RVX::RHI
    RVX::ShaderCompiler
)

# Windows SDK libraries
target_link_libraries(RVX_RHI_DX12 PRIVATE
    d3d12
    dxgi
    dxguid
)

# D3D12 Memory Allocator
if(TARGET unofficial::D3D12MemoryAllocator)
    target_link_libraries(RVX_RHI_DX12 PRIVATE unofficial::D3D12MemoryAllocator)
    target_compile_definitions(RVX_RHI_DX12 PRIVATE RVX_USE_D3D12MA=1)
elseif(TARGET D3D12MemoryAllocator)
    # Compatibility with environments that export a non-namespaced target.
    target_link_libraries(RVX_RHI_DX12 PRIVATE D3D12MemoryAllocator)
    target_compile_definitions(RVX_RHI_DX12 PRIVATE RVX_USE_D3D12MA=1)
endif()

add_library(RVX::RHI_DX12 ALIAS RVX_RHI_DX12)
