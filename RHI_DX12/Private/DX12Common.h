#pragma once

// =============================================================================
// DX12 Common Headers and Utilities
// =============================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include "Core/Types.h"
#include "Core/Log.h"
#include "Core/Assert.h"
#include "RHI/RHIDefinitions.h"

#ifdef RVX_USE_D3D12MA
#include <d3d12memalloc.h>
#endif

namespace RVX
{
    using Microsoft::WRL::ComPtr;

    // =============================================================================
    // DX12 Error Handling
    // =============================================================================
    inline bool DX12Succeeded(HRESULT hr)
    {
        return SUCCEEDED(hr);
    }

    inline void DX12Check(HRESULT hr, const char* message = "DX12 Error")
    {
        if (FAILED(hr))
        {
            RVX_RHI_ERROR("{}: HRESULT = 0x{:08X}", message, static_cast<uint32>(hr));
            RVX_ASSERT(false);
        }
    }

    #define DX12_CHECK(hr) DX12Check(hr, #hr)

    // =============================================================================
    // Format Conversion
    // =============================================================================
    inline DXGI_FORMAT ToDXGIFormat(RHIFormat format)
    {
        switch (format)
        {
            case RHIFormat::Unknown:           return DXGI_FORMAT_UNKNOWN;

            // 8-bit formats
            case RHIFormat::R8_UNORM:          return DXGI_FORMAT_R8_UNORM;
            case RHIFormat::R8_SNORM:          return DXGI_FORMAT_R8_SNORM;
            case RHIFormat::R8_UINT:           return DXGI_FORMAT_R8_UINT;
            case RHIFormat::R8_SINT:           return DXGI_FORMAT_R8_SINT;

            // 16-bit formats
            case RHIFormat::R16_FLOAT:         return DXGI_FORMAT_R16_FLOAT;
            case RHIFormat::R16_UNORM:         return DXGI_FORMAT_R16_UNORM;
            case RHIFormat::R16_UINT:          return DXGI_FORMAT_R16_UINT;
            case RHIFormat::R16_SINT:          return DXGI_FORMAT_R16_SINT;
            case RHIFormat::RG8_UNORM:         return DXGI_FORMAT_R8G8_UNORM;
            case RHIFormat::RG8_SNORM:         return DXGI_FORMAT_R8G8_SNORM;
            case RHIFormat::RG8_UINT:          return DXGI_FORMAT_R8G8_UINT;
            case RHIFormat::RG8_SINT:          return DXGI_FORMAT_R8G8_SINT;

            // 32-bit formats
            case RHIFormat::R32_FLOAT:         return DXGI_FORMAT_R32_FLOAT;
            case RHIFormat::R32_UINT:          return DXGI_FORMAT_R32_UINT;
            case RHIFormat::R32_SINT:          return DXGI_FORMAT_R32_SINT;
            case RHIFormat::RG16_FLOAT:        return DXGI_FORMAT_R16G16_FLOAT;
            case RHIFormat::RG16_UNORM:        return DXGI_FORMAT_R16G16_UNORM;
            case RHIFormat::RG16_UINT:         return DXGI_FORMAT_R16G16_UINT;
            case RHIFormat::RG16_SINT:         return DXGI_FORMAT_R16G16_SINT;
            case RHIFormat::RGBA8_UNORM:       return DXGI_FORMAT_R8G8B8A8_UNORM;
            case RHIFormat::RGBA8_UNORM_SRGB:  return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
            case RHIFormat::RGBA8_SNORM:       return DXGI_FORMAT_R8G8B8A8_SNORM;
            case RHIFormat::RGBA8_UINT:        return DXGI_FORMAT_R8G8B8A8_UINT;
            case RHIFormat::RGBA8_SINT:        return DXGI_FORMAT_R8G8B8A8_SINT;
            case RHIFormat::BGRA8_UNORM:       return DXGI_FORMAT_B8G8R8A8_UNORM;
            case RHIFormat::BGRA8_UNORM_SRGB:  return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
            case RHIFormat::RGB10A2_UNORM:     return DXGI_FORMAT_R10G10B10A2_UNORM;
            case RHIFormat::RGB10A2_UINT:      return DXGI_FORMAT_R10G10B10A2_UINT;
            case RHIFormat::RG11B10_FLOAT:     return DXGI_FORMAT_R11G11B10_FLOAT;

            // 96-bit formats (vertex data)
            case RHIFormat::RGB32_FLOAT:       return DXGI_FORMAT_R32G32B32_FLOAT;
            case RHIFormat::RGB32_UINT:        return DXGI_FORMAT_R32G32B32_UINT;
            case RHIFormat::RGB32_SINT:        return DXGI_FORMAT_R32G32B32_SINT;

            // 64-bit formats
            case RHIFormat::RG32_FLOAT:        return DXGI_FORMAT_R32G32_FLOAT;
            case RHIFormat::RG32_UINT:         return DXGI_FORMAT_R32G32_UINT;
            case RHIFormat::RG32_SINT:         return DXGI_FORMAT_R32G32_SINT;
            case RHIFormat::RGBA16_FLOAT:      return DXGI_FORMAT_R16G16B16A16_FLOAT;
            case RHIFormat::RGBA16_UNORM:      return DXGI_FORMAT_R16G16B16A16_UNORM;
            case RHIFormat::RGBA16_UINT:       return DXGI_FORMAT_R16G16B16A16_UINT;
            case RHIFormat::RGBA16_SINT:       return DXGI_FORMAT_R16G16B16A16_SINT;

            // 128-bit formats
            case RHIFormat::RGBA32_FLOAT:      return DXGI_FORMAT_R32G32B32A32_FLOAT;
            case RHIFormat::RGBA32_UINT:       return DXGI_FORMAT_R32G32B32A32_UINT;
            case RHIFormat::RGBA32_SINT:       return DXGI_FORMAT_R32G32B32A32_SINT;

            // Depth formats
            case RHIFormat::D16_UNORM:         return DXGI_FORMAT_D16_UNORM;
            case RHIFormat::D24_UNORM_S8_UINT: return DXGI_FORMAT_D24_UNORM_S8_UINT;
            case RHIFormat::D32_FLOAT:         return DXGI_FORMAT_D32_FLOAT;
            case RHIFormat::D32_FLOAT_S8_UINT: return DXGI_FORMAT_D32_FLOAT_S8X24_UINT;

            // BC formats
            case RHIFormat::BC1_UNORM:         return DXGI_FORMAT_BC1_UNORM;
            case RHIFormat::BC1_UNORM_SRGB:    return DXGI_FORMAT_BC1_UNORM_SRGB;
            case RHIFormat::BC2_UNORM:         return DXGI_FORMAT_BC2_UNORM;
            case RHIFormat::BC2_UNORM_SRGB:    return DXGI_FORMAT_BC2_UNORM_SRGB;
            case RHIFormat::BC3_UNORM:         return DXGI_FORMAT_BC3_UNORM;
            case RHIFormat::BC3_UNORM_SRGB:    return DXGI_FORMAT_BC3_UNORM_SRGB;
            case RHIFormat::BC4_UNORM:         return DXGI_FORMAT_BC4_UNORM;
            case RHIFormat::BC4_SNORM:         return DXGI_FORMAT_BC4_SNORM;
            case RHIFormat::BC5_UNORM:         return DXGI_FORMAT_BC5_UNORM;
            case RHIFormat::BC5_SNORM:         return DXGI_FORMAT_BC5_SNORM;
            case RHIFormat::BC6H_UF16:         return DXGI_FORMAT_BC6H_UF16;
            case RHIFormat::BC6H_SF16:         return DXGI_FORMAT_BC6H_SF16;
            case RHIFormat::BC7_UNORM:         return DXGI_FORMAT_BC7_UNORM;
            case RHIFormat::BC7_UNORM_SRGB:    return DXGI_FORMAT_BC7_UNORM_SRGB;

            default:
                RVX_RHI_ERROR("Unknown RHIFormat: {}", static_cast<int>(format));
                return DXGI_FORMAT_UNKNOWN;
        }
    }

    inline DXGI_FORMAT GetTypelessFormat(DXGI_FORMAT format)
    {
        switch (format)
        {
            case DXGI_FORMAT_D16_UNORM:           return DXGI_FORMAT_R16_TYPELESS;
            case DXGI_FORMAT_D24_UNORM_S8_UINT:   return DXGI_FORMAT_R24G8_TYPELESS;
            case DXGI_FORMAT_D32_FLOAT:           return DXGI_FORMAT_R32_TYPELESS;
            case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return DXGI_FORMAT_R32G8X24_TYPELESS;
            default: return format;
        }
    }

    inline DXGI_FORMAT GetDepthSRVFormat(DXGI_FORMAT format)
    {
        switch (format)
        {
            case DXGI_FORMAT_D16_UNORM:
            case DXGI_FORMAT_R16_TYPELESS:        return DXGI_FORMAT_R16_UNORM;
            case DXGI_FORMAT_D24_UNORM_S8_UINT:
            case DXGI_FORMAT_R24G8_TYPELESS:      return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
            case DXGI_FORMAT_D32_FLOAT:
            case DXGI_FORMAT_R32_TYPELESS:        return DXGI_FORMAT_R32_FLOAT;
            case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
            case DXGI_FORMAT_R32G8X24_TYPELESS:   return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
            default: return format;
        }
    }

    // =============================================================================
    // Resource State Conversion
    // =============================================================================
    inline D3D12_RESOURCE_STATES ToD3D12ResourceState(RHIResourceState state)
    {
        switch (state)
        {
            case RHIResourceState::Undefined:
            case RHIResourceState::Common:         return D3D12_RESOURCE_STATE_COMMON;
            case RHIResourceState::VertexBuffer:   return D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
            case RHIResourceState::IndexBuffer:    return D3D12_RESOURCE_STATE_INDEX_BUFFER;
            case RHIResourceState::ConstantBuffer: return D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
            case RHIResourceState::ShaderResource: return D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            case RHIResourceState::UnorderedAccess: return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            case RHIResourceState::RenderTarget:   return D3D12_RESOURCE_STATE_RENDER_TARGET;
            case RHIResourceState::DepthWrite:     return D3D12_RESOURCE_STATE_DEPTH_WRITE;
            case RHIResourceState::DepthRead:      return D3D12_RESOURCE_STATE_DEPTH_READ;
            case RHIResourceState::CopyDest:       return D3D12_RESOURCE_STATE_COPY_DEST;
            case RHIResourceState::CopySource:     return D3D12_RESOURCE_STATE_COPY_SOURCE;
            case RHIResourceState::Present:        return D3D12_RESOURCE_STATE_PRESENT;
            case RHIResourceState::IndirectArgument: return D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
            default:
                RVX_RHI_ERROR("Unknown RHIResourceState: {}", static_cast<int>(state));
                return D3D12_RESOURCE_STATE_COMMON;
        }
    }

    // =============================================================================
    // Heap Type Conversion
    // =============================================================================
    inline D3D12_HEAP_TYPE ToD3D12HeapType(RHIMemoryType memoryType)
    {
        switch (memoryType)
        {
            case RHIMemoryType::Default:  return D3D12_HEAP_TYPE_DEFAULT;
            case RHIMemoryType::Upload:   return D3D12_HEAP_TYPE_UPLOAD;
            case RHIMemoryType::Readback: return D3D12_HEAP_TYPE_READBACK;
            default:                      return D3D12_HEAP_TYPE_DEFAULT;
        }
    }

    // =============================================================================
    // Primitive Topology Conversion
    // =============================================================================
    inline D3D12_PRIMITIVE_TOPOLOGY_TYPE ToD3D12PrimitiveTopologyType(RHIPrimitiveTopology topology)
    {
        switch (topology)
        {
            case RHIPrimitiveTopology::PointList:     return D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
            case RHIPrimitiveTopology::LineList:
            case RHIPrimitiveTopology::LineStrip:     return D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
            case RHIPrimitiveTopology::TriangleList:
            case RHIPrimitiveTopology::TriangleStrip: return D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            default: return D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        }
    }

    inline D3D_PRIMITIVE_TOPOLOGY ToD3DPrimitiveTopology(RHIPrimitiveTopology topology)
    {
        switch (topology)
        {
            case RHIPrimitiveTopology::PointList:     return D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
            case RHIPrimitiveTopology::LineList:      return D3D_PRIMITIVE_TOPOLOGY_LINELIST;
            case RHIPrimitiveTopology::LineStrip:     return D3D_PRIMITIVE_TOPOLOGY_LINESTRIP;
            case RHIPrimitiveTopology::TriangleList:  return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
            case RHIPrimitiveTopology::TriangleStrip: return D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
            default: return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        }
    }

} // namespace RVX
