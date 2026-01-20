#pragma once

#include "DX11Common.h"
#include "RHI/RHIPipeline.h"

namespace RVX
{
    // =============================================================================
    // Format Conversion: RHIFormat -> DXGI_FORMAT
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

    // =============================================================================
    // Typeless Formats (for depth textures with SRV)
    // =============================================================================
    inline DXGI_FORMAT GetTypelessFormat(DXGI_FORMAT format)
    {
        switch (format)
        {
            case DXGI_FORMAT_D16_UNORM:            return DXGI_FORMAT_R16_TYPELESS;
            case DXGI_FORMAT_D24_UNORM_S8_UINT:    return DXGI_FORMAT_R24G8_TYPELESS;
            case DXGI_FORMAT_D32_FLOAT:            return DXGI_FORMAT_R32_TYPELESS;
            case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return DXGI_FORMAT_R32G8X24_TYPELESS;
            default: return format;
        }
    }

    inline DXGI_FORMAT GetDepthSRVFormat(DXGI_FORMAT format)
    {
        switch (format)
        {
            case DXGI_FORMAT_D16_UNORM:
            case DXGI_FORMAT_R16_TYPELESS:         return DXGI_FORMAT_R16_UNORM;
            case DXGI_FORMAT_D24_UNORM_S8_UINT:
            case DXGI_FORMAT_R24G8_TYPELESS:       return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
            case DXGI_FORMAT_D32_FLOAT:
            case DXGI_FORMAT_R32_TYPELESS:         return DXGI_FORMAT_R32_FLOAT;
            case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
            case DXGI_FORMAT_R32G8X24_TYPELESS:    return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
            default: return format;
        }
    }

    // =============================================================================
    // Primitive Topology
    // =============================================================================
    inline D3D11_PRIMITIVE_TOPOLOGY ToD3D11PrimitiveTopology(RHIPrimitiveTopology topology)
    {
        switch (topology)
        {
            case RHIPrimitiveTopology::PointList:     return D3D11_PRIMITIVE_TOPOLOGY_POINTLIST;
            case RHIPrimitiveTopology::LineList:      return D3D11_PRIMITIVE_TOPOLOGY_LINELIST;
            case RHIPrimitiveTopology::LineStrip:     return D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP;
            case RHIPrimitiveTopology::TriangleList:  return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
            case RHIPrimitiveTopology::TriangleStrip: return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
            default: return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        }
    }

    // =============================================================================
    // Fill Mode
    // =============================================================================
    inline D3D11_FILL_MODE ToD3D11FillMode(RHIFillMode mode)
    {
        switch (mode)
        {
            case RHIFillMode::Solid:     return D3D11_FILL_SOLID;
            case RHIFillMode::Wireframe: return D3D11_FILL_WIREFRAME;
            default: return D3D11_FILL_SOLID;
        }
    }

    // =============================================================================
    // Cull Mode
    // =============================================================================
    inline D3D11_CULL_MODE ToD3D11CullMode(RHICullMode mode)
    {
        switch (mode)
        {
            case RHICullMode::None:  return D3D11_CULL_NONE;
            case RHICullMode::Front: return D3D11_CULL_FRONT;
            case RHICullMode::Back:  return D3D11_CULL_BACK;
            default: return D3D11_CULL_BACK;
        }
    }

    // =============================================================================
    // Compare Operation
    // =============================================================================
    inline D3D11_COMPARISON_FUNC ToD3D11ComparisonFunc(RHICompareOp op)
    {
        switch (op)
        {
            case RHICompareOp::Never:        return D3D11_COMPARISON_NEVER;
            case RHICompareOp::Less:         return D3D11_COMPARISON_LESS;
            case RHICompareOp::Equal:        return D3D11_COMPARISON_EQUAL;
            case RHICompareOp::LessEqual:    return D3D11_COMPARISON_LESS_EQUAL;
            case RHICompareOp::Greater:      return D3D11_COMPARISON_GREATER;
            case RHICompareOp::NotEqual:     return D3D11_COMPARISON_NOT_EQUAL;
            case RHICompareOp::GreaterEqual: return D3D11_COMPARISON_GREATER_EQUAL;
            case RHICompareOp::Always:       return D3D11_COMPARISON_ALWAYS;
            default: return D3D11_COMPARISON_LESS;
        }
    }

    // =============================================================================
    // Stencil Operation
    // =============================================================================
    inline D3D11_STENCIL_OP ToD3D11StencilOp(RHIStencilOp op)
    {
        switch (op)
        {
            case RHIStencilOp::Keep:           return D3D11_STENCIL_OP_KEEP;
            case RHIStencilOp::Zero:           return D3D11_STENCIL_OP_ZERO;
            case RHIStencilOp::Replace:        return D3D11_STENCIL_OP_REPLACE;
            case RHIStencilOp::IncrementClamp: return D3D11_STENCIL_OP_INCR_SAT;
            case RHIStencilOp::DecrementClamp: return D3D11_STENCIL_OP_DECR_SAT;
            case RHIStencilOp::Invert:         return D3D11_STENCIL_OP_INVERT;
            case RHIStencilOp::IncrementWrap:  return D3D11_STENCIL_OP_INCR;
            case RHIStencilOp::DecrementWrap:  return D3D11_STENCIL_OP_DECR;
            default: return D3D11_STENCIL_OP_KEEP;
        }
    }

    // =============================================================================
    // Blend Factor
    // =============================================================================
    inline D3D11_BLEND ToD3D11Blend(RHIBlendFactor factor)
    {
        switch (factor)
        {
            case RHIBlendFactor::Zero:             return D3D11_BLEND_ZERO;
            case RHIBlendFactor::One:              return D3D11_BLEND_ONE;
            case RHIBlendFactor::SrcColor:         return D3D11_BLEND_SRC_COLOR;
            case RHIBlendFactor::InvSrcColor:      return D3D11_BLEND_INV_SRC_COLOR;
            case RHIBlendFactor::SrcAlpha:         return D3D11_BLEND_SRC_ALPHA;
            case RHIBlendFactor::InvSrcAlpha:      return D3D11_BLEND_INV_SRC_ALPHA;
            case RHIBlendFactor::DstColor:         return D3D11_BLEND_DEST_COLOR;
            case RHIBlendFactor::InvDstColor:      return D3D11_BLEND_INV_DEST_COLOR;
            case RHIBlendFactor::DstAlpha:         return D3D11_BLEND_DEST_ALPHA;
            case RHIBlendFactor::InvDstAlpha:      return D3D11_BLEND_INV_DEST_ALPHA;
            case RHIBlendFactor::SrcAlphaSaturate: return D3D11_BLEND_SRC_ALPHA_SAT;
            case RHIBlendFactor::ConstantColor:    return D3D11_BLEND_BLEND_FACTOR;
            case RHIBlendFactor::InvConstantColor: return D3D11_BLEND_INV_BLEND_FACTOR;
            default: return D3D11_BLEND_ONE;
        }
    }

    // =============================================================================
    // Blend Operation
    // =============================================================================
    inline D3D11_BLEND_OP ToD3D11BlendOp(RHIBlendOp op)
    {
        switch (op)
        {
            case RHIBlendOp::Add:             return D3D11_BLEND_OP_ADD;
            case RHIBlendOp::Subtract:        return D3D11_BLEND_OP_SUBTRACT;
            case RHIBlendOp::ReverseSubtract: return D3D11_BLEND_OP_REV_SUBTRACT;
            case RHIBlendOp::Min:             return D3D11_BLEND_OP_MIN;
            case RHIBlendOp::Max:             return D3D11_BLEND_OP_MAX;
            default: return D3D11_BLEND_OP_ADD;
        }
    }

    // =============================================================================
    // Sampler Filter
    // =============================================================================
    inline D3D11_FILTER ToD3D11Filter(RHIFilterMode min, RHIFilterMode mag, RHIFilterMode mip, bool anisotropic)
    {
        if (anisotropic) return D3D11_FILTER_ANISOTROPIC;
        
        int filter = 0;
        if (min == RHIFilterMode::Linear) filter |= 0x10;
        if (mag == RHIFilterMode::Linear) filter |= 0x04;
        if (mip == RHIFilterMode::Linear) filter |= 0x01;
        return static_cast<D3D11_FILTER>(filter);
    }

    // =============================================================================
    // Address Mode
    // =============================================================================
    inline D3D11_TEXTURE_ADDRESS_MODE ToD3D11AddressMode(RHIAddressMode mode)
    {
        switch (mode)
        {
            case RHIAddressMode::Repeat:        return D3D11_TEXTURE_ADDRESS_WRAP;
            case RHIAddressMode::MirrorRepeat:  return D3D11_TEXTURE_ADDRESS_MIRROR;
            case RHIAddressMode::ClampToEdge:   return D3D11_TEXTURE_ADDRESS_CLAMP;
            case RHIAddressMode::ClampToBorder: return D3D11_TEXTURE_ADDRESS_BORDER;
            default: return D3D11_TEXTURE_ADDRESS_WRAP;
        }
    }

    // =============================================================================
    // Buffer Usage to D3D11 Bind Flags
    // =============================================================================
    inline UINT ToD3D11BindFlags(RHIBufferUsage usage)
    {
        UINT flags = 0;
        if (HasFlag(usage, RHIBufferUsage::Vertex))          flags |= D3D11_BIND_VERTEX_BUFFER;
        if (HasFlag(usage, RHIBufferUsage::Index))           flags |= D3D11_BIND_INDEX_BUFFER;
        if (HasFlag(usage, RHIBufferUsage::Constant))        flags |= D3D11_BIND_CONSTANT_BUFFER;
        if (HasFlag(usage, RHIBufferUsage::ShaderResource))  flags |= D3D11_BIND_SHADER_RESOURCE;
        if (HasFlag(usage, RHIBufferUsage::Structured))      flags |= D3D11_BIND_SHADER_RESOURCE;
        if (HasFlag(usage, RHIBufferUsage::UnorderedAccess)) flags |= D3D11_BIND_UNORDERED_ACCESS;
        return flags;
    }

    inline D3D11_USAGE ToD3D11Usage(RHIMemoryType memoryType)
    {
        switch (memoryType)
        {
            case RHIMemoryType::Upload:   return D3D11_USAGE_DYNAMIC;
            case RHIMemoryType::Readback: return D3D11_USAGE_STAGING;
            case RHIMemoryType::Default:
            default:                      return D3D11_USAGE_DEFAULT;
        }
    }

    inline UINT ToD3D11CPUAccessFlags(RHIMemoryType memoryType)
    {
        switch (memoryType)
        {
            case RHIMemoryType::Upload:   return D3D11_CPU_ACCESS_WRITE;
            case RHIMemoryType::Readback: return D3D11_CPU_ACCESS_READ;
            default:                      return 0;
        }
    }

    // =============================================================================
    // Texture Usage to D3D11 Bind Flags
    // =============================================================================
    inline UINT ToD3D11BindFlags(RHITextureUsage usage)
    {
        UINT flags = 0;
        if (HasFlag(usage, RHITextureUsage::ShaderResource))  flags |= D3D11_BIND_SHADER_RESOURCE;
        if (HasFlag(usage, RHITextureUsage::RenderTarget))    flags |= D3D11_BIND_RENDER_TARGET;
        if (HasFlag(usage, RHITextureUsage::DepthStencil))    flags |= D3D11_BIND_DEPTH_STENCIL;
        if (HasFlag(usage, RHITextureUsage::UnorderedAccess)) flags |= D3D11_BIND_UNORDERED_ACCESS;
        return flags;
    }

} // namespace RVX
