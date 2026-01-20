#pragma once

#include "MetalCommon.h"

namespace RVX
{
    // =============================================================================
    // Format Conversion: RHIFormat -> MTLPixelFormat
    // =============================================================================
    inline MTLPixelFormat ToMTLPixelFormat(RHIFormat format)
    {
        switch (format)
        {
            case RHIFormat::Unknown:          return MTLPixelFormatInvalid;
            
            // 8-bit formats
            case RHIFormat::R8_UNORM:         return MTLPixelFormatR8Unorm;
            case RHIFormat::R8_SNORM:         return MTLPixelFormatR8Snorm;
            case RHIFormat::R8_UINT:          return MTLPixelFormatR8Uint;
            case RHIFormat::R8_SINT:          return MTLPixelFormatR8Sint;
            
            // 16-bit formats
            case RHIFormat::R16_FLOAT:        return MTLPixelFormatR16Float;
            case RHIFormat::R16_UNORM:        return MTLPixelFormatR16Unorm;
            case RHIFormat::R16_UINT:         return MTLPixelFormatR16Uint;
            case RHIFormat::R16_SINT:         return MTLPixelFormatR16Sint;
            case RHIFormat::RG8_UNORM:        return MTLPixelFormatRG8Unorm;
            case RHIFormat::RG8_SNORM:        return MTLPixelFormatRG8Snorm;
            case RHIFormat::RG8_UINT:         return MTLPixelFormatRG8Uint;
            case RHIFormat::RG8_SINT:         return MTLPixelFormatRG8Sint;
            
            // 32-bit formats
            case RHIFormat::R32_FLOAT:        return MTLPixelFormatR32Float;
            case RHIFormat::R32_UINT:         return MTLPixelFormatR32Uint;
            case RHIFormat::R32_SINT:         return MTLPixelFormatR32Sint;
            case RHIFormat::RG16_FLOAT:       return MTLPixelFormatRG16Float;
            case RHIFormat::RG16_UNORM:       return MTLPixelFormatRG16Unorm;
            case RHIFormat::RG16_UINT:        return MTLPixelFormatRG16Uint;
            case RHIFormat::RG16_SINT:        return MTLPixelFormatRG16Sint;
            case RHIFormat::RGBA8_UNORM:      return MTLPixelFormatRGBA8Unorm;
            case RHIFormat::RGBA8_UNORM_SRGB: return MTLPixelFormatRGBA8Unorm_sRGB;
            case RHIFormat::RGBA8_SNORM:      return MTLPixelFormatRGBA8Snorm;
            case RHIFormat::RGBA8_UINT:       return MTLPixelFormatRGBA8Uint;
            case RHIFormat::RGBA8_SINT:       return MTLPixelFormatRGBA8Sint;
            case RHIFormat::BGRA8_UNORM:      return MTLPixelFormatBGRA8Unorm;
            case RHIFormat::BGRA8_UNORM_SRGB: return MTLPixelFormatBGRA8Unorm_sRGB;
            case RHIFormat::RGB10A2_UNORM:    return MTLPixelFormatRGB10A2Unorm;
            case RHIFormat::RGB10A2_UINT:     return MTLPixelFormatRGB10A2Uint;
            case RHIFormat::RG11B10_FLOAT:    return MTLPixelFormatRG11B10Float;
            
            // 64-bit formats
            case RHIFormat::RG32_FLOAT:       return MTLPixelFormatRG32Float;
            case RHIFormat::RG32_UINT:        return MTLPixelFormatRG32Uint;
            case RHIFormat::RG32_SINT:        return MTLPixelFormatRG32Sint;
            case RHIFormat::RGBA16_FLOAT:     return MTLPixelFormatRGBA16Float;
            case RHIFormat::RGBA16_UNORM:     return MTLPixelFormatRGBA16Unorm;
            case RHIFormat::RGBA16_UINT:      return MTLPixelFormatRGBA16Uint;
            case RHIFormat::RGBA16_SINT:      return MTLPixelFormatRGBA16Sint;
            
            // 128-bit formats
            case RHIFormat::RGBA32_FLOAT:     return MTLPixelFormatRGBA32Float;
            case RHIFormat::RGBA32_UINT:      return MTLPixelFormatRGBA32Uint;
            case RHIFormat::RGBA32_SINT:      return MTLPixelFormatRGBA32Sint;
            
            // Depth-Stencil formats
            case RHIFormat::D16_UNORM:        return MTLPixelFormatDepth16Unorm;
            case RHIFormat::D32_FLOAT:        return MTLPixelFormatDepth32Float;
            // D24_UNORM_S8_UINT is only available on Intel Macs, not Apple Silicon
            // Always use D32_FLOAT_S8_UINT as fallback which is universally supported
            case RHIFormat::D24_UNORM_S8_UINT: return MTLPixelFormatDepth32Float_Stencil8;
            case RHIFormat::D32_FLOAT_S8_UINT: return MTLPixelFormatDepth32Float_Stencil8;
            
            // Compressed formats (BC - macOS only)
#if RVX_PLATFORM_MACOS
            case RHIFormat::BC1_UNORM:        return MTLPixelFormatBC1_RGBA;
            case RHIFormat::BC1_UNORM_SRGB:   return MTLPixelFormatBC1_RGBA_sRGB;
            case RHIFormat::BC2_UNORM:        return MTLPixelFormatBC2_RGBA;
            case RHIFormat::BC2_UNORM_SRGB:   return MTLPixelFormatBC2_RGBA_sRGB;
            case RHIFormat::BC3_UNORM:        return MTLPixelFormatBC3_RGBA;
            case RHIFormat::BC3_UNORM_SRGB:   return MTLPixelFormatBC3_RGBA_sRGB;
            case RHIFormat::BC4_UNORM:        return MTLPixelFormatBC4_RUnorm;
            case RHIFormat::BC4_SNORM:        return MTLPixelFormatBC4_RSnorm;
            case RHIFormat::BC5_UNORM:        return MTLPixelFormatBC5_RGUnorm;
            case RHIFormat::BC5_SNORM:        return MTLPixelFormatBC5_RGSnorm;
            case RHIFormat::BC6H_UF16:        return MTLPixelFormatBC6H_RGBUfloat;
            case RHIFormat::BC6H_SF16:        return MTLPixelFormatBC6H_RGBFloat;
            case RHIFormat::BC7_UNORM:        return MTLPixelFormatBC7_RGBAUnorm;
            case RHIFormat::BC7_UNORM_SRGB:   return MTLPixelFormatBC7_RGBAUnorm_sRGB;
#endif
            
            default:                          return MTLPixelFormatInvalid;
        }
    }

    // =============================================================================
    // Vertex Format Conversion
    // =============================================================================
    inline MTLVertexFormat ToMTLVertexFormat(RHIFormat format)
    {
        switch (format)
        {
            case RHIFormat::R32_FLOAT:      return MTLVertexFormatFloat;
            case RHIFormat::RG32_FLOAT:     return MTLVertexFormatFloat2;
            case RHIFormat::RGB32_FLOAT:    return MTLVertexFormatFloat3;
            case RHIFormat::RGBA32_FLOAT:   return MTLVertexFormatFloat4;
            case RHIFormat::R32_UINT:       return MTLVertexFormatUInt;
            case RHIFormat::RG32_UINT:      return MTLVertexFormatUInt2;
            case RHIFormat::RGB32_UINT:     return MTLVertexFormatUInt3;
            case RHIFormat::RGBA32_UINT:    return MTLVertexFormatUInt4;
            case RHIFormat::R32_SINT:       return MTLVertexFormatInt;
            case RHIFormat::RG32_SINT:      return MTLVertexFormatInt2;
            case RHIFormat::RGB32_SINT:     return MTLVertexFormatInt3;
            case RHIFormat::RGBA32_SINT:    return MTLVertexFormatInt4;
            case RHIFormat::RGBA8_UNORM:    return MTLVertexFormatUChar4Normalized;
            case RHIFormat::RGBA8_UINT:     return MTLVertexFormatUChar4;
            default:                        return MTLVertexFormatInvalid;
        }
    }

    // =============================================================================
    // Primitive Topology
    // =============================================================================
    inline MTLPrimitiveType ToMTLPrimitiveType(RHIPrimitiveTopology topology)
    {
        switch (topology)
        {
            case RHIPrimitiveTopology::PointList:     return MTLPrimitiveTypePoint;
            case RHIPrimitiveTopology::LineList:      return MTLPrimitiveTypeLine;
            case RHIPrimitiveTopology::LineStrip:     return MTLPrimitiveTypeLineStrip;
            case RHIPrimitiveTopology::TriangleList:  return MTLPrimitiveTypeTriangle;
            case RHIPrimitiveTopology::TriangleStrip: return MTLPrimitiveTypeTriangleStrip;
            default:                                   return MTLPrimitiveTypeTriangle;
        }
    }

    // =============================================================================
    // Cull Mode
    // =============================================================================
    inline MTLCullMode ToMTLCullMode(RHICullMode mode)
    {
        switch (mode)
        {
            case RHICullMode::None:  return MTLCullModeNone;
            case RHICullMode::Front: return MTLCullModeFront;
            case RHICullMode::Back:  return MTLCullModeBack;
            default:                 return MTLCullModeBack;
        }
    }

    // =============================================================================
    // Compare Function
    // =============================================================================
    inline MTLCompareFunction ToMTLCompareFunction(RHICompareOp op)
    {
        switch (op)
        {
            case RHICompareOp::Never:        return MTLCompareFunctionNever;
            case RHICompareOp::Less:         return MTLCompareFunctionLess;
            case RHICompareOp::Equal:        return MTLCompareFunctionEqual;
            case RHICompareOp::LessEqual:    return MTLCompareFunctionLessEqual;
            case RHICompareOp::Greater:      return MTLCompareFunctionGreater;
            case RHICompareOp::NotEqual:     return MTLCompareFunctionNotEqual;
            case RHICompareOp::GreaterEqual: return MTLCompareFunctionGreaterEqual;
            case RHICompareOp::Always:       return MTLCompareFunctionAlways;
            default:                         return MTLCompareFunctionLess;
        }
    }

    // =============================================================================
    // Blend Factor
    // =============================================================================
    inline MTLBlendFactor ToMTLBlendFactor(RHIBlendFactor factor)
    {
        switch (factor)
        {
            case RHIBlendFactor::Zero:             return MTLBlendFactorZero;
            case RHIBlendFactor::One:              return MTLBlendFactorOne;
            case RHIBlendFactor::SrcColor:         return MTLBlendFactorSourceColor;
            case RHIBlendFactor::InvSrcColor:      return MTLBlendFactorOneMinusSourceColor;
            case RHIBlendFactor::SrcAlpha:         return MTLBlendFactorSourceAlpha;
            case RHIBlendFactor::InvSrcAlpha:      return MTLBlendFactorOneMinusSourceAlpha;
            case RHIBlendFactor::DstColor:         return MTLBlendFactorDestinationColor;
            case RHIBlendFactor::InvDstColor:      return MTLBlendFactorOneMinusDestinationColor;
            case RHIBlendFactor::DstAlpha:         return MTLBlendFactorDestinationAlpha;
            case RHIBlendFactor::InvDstAlpha:      return MTLBlendFactorOneMinusDestinationAlpha;
            case RHIBlendFactor::SrcAlphaSaturate: return MTLBlendFactorSourceAlphaSaturated;
            case RHIBlendFactor::ConstantColor:    return MTLBlendFactorBlendColor;
            case RHIBlendFactor::InvConstantColor: return MTLBlendFactorOneMinusBlendColor;
            default:                               return MTLBlendFactorOne;
        }
    }

    // =============================================================================
    // Blend Operation
    // =============================================================================
    inline MTLBlendOperation ToMTLBlendOperation(RHIBlendOp op)
    {
        switch (op)
        {
            case RHIBlendOp::Add:             return MTLBlendOperationAdd;
            case RHIBlendOp::Subtract:        return MTLBlendOperationSubtract;
            case RHIBlendOp::ReverseSubtract: return MTLBlendOperationReverseSubtract;
            case RHIBlendOp::Min:             return MTLBlendOperationMin;
            case RHIBlendOp::Max:             return MTLBlendOperationMax;
            default:                          return MTLBlendOperationAdd;
        }
    }

    // =============================================================================
    // Sampler Address Mode
    // =============================================================================
    inline MTLSamplerAddressMode ToMTLSamplerAddressMode(RHIAddressMode mode)
    {
        switch (mode)
        {
            case RHIAddressMode::Repeat:        return MTLSamplerAddressModeRepeat;
            case RHIAddressMode::MirrorRepeat:  return MTLSamplerAddressModeMirrorRepeat;
            case RHIAddressMode::ClampToEdge:   return MTLSamplerAddressModeClampToEdge;
            case RHIAddressMode::ClampToBorder: return MTLSamplerAddressModeClampToBorderColor;
            default:                            return MTLSamplerAddressModeRepeat;
        }
    }

    // =============================================================================
    // Sampler Filter
    // =============================================================================
    inline MTLSamplerMinMagFilter ToMTLSamplerFilter(RHIFilterMode filter)
    {
        switch (filter)
        {
            case RHIFilterMode::Nearest: return MTLSamplerMinMagFilterNearest;
            case RHIFilterMode::Linear:  return MTLSamplerMinMagFilterLinear;
            default:                     return MTLSamplerMinMagFilterLinear;
        }
    }

    // =============================================================================
    // Load Action
    // =============================================================================
    inline MTLLoadAction ToMTLLoadAction(RHILoadOp op)
    {
        switch (op)
        {
            case RHILoadOp::Load:     return MTLLoadActionLoad;
            case RHILoadOp::Clear:    return MTLLoadActionClear;
            case RHILoadOp::DontCare: return MTLLoadActionDontCare;
            default:                  return MTLLoadActionDontCare;
        }
    }

    // =============================================================================
    // Store Action
    // =============================================================================
    inline MTLStoreAction ToMTLStoreAction(RHIStoreOp op)
    {
        switch (op)
        {
            case RHIStoreOp::Store:    return MTLStoreActionStore;
            case RHIStoreOp::DontCare: return MTLStoreActionDontCare;
            default:                   return MTLStoreActionStore;
        }
    }

    // =============================================================================
    // Index Type
    // =============================================================================
    inline MTLIndexType ToMTLIndexType(RHIFormat format)
    {
        switch (format)
        {
            case RHIFormat::R16_UINT: return MTLIndexTypeUInt16;
            case RHIFormat::R32_UINT: return MTLIndexTypeUInt32;
            default:                  return MTLIndexTypeUInt32;
        }
    }

} // namespace RVX
