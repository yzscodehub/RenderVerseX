#pragma once

#include "OpenGLCommon.h"
#include "RHI/RHIDefinitions.h"

namespace RVX
{
    // =============================================================================
    // Format Conversion: RHIFormat -> GL Internal Format, Format, Type
    // =============================================================================
    struct GLFormatInfo
    {
        GLenum internalFormat;  // GL internal format (e.g., GL_RGBA8)
        GLenum format;          // GL format (e.g., GL_RGBA)
        GLenum type;            // GL type (e.g., GL_UNSIGNED_BYTE)
        bool compressed;
    };

    inline GLFormatInfo ToGLFormat(RHIFormat format)
    {
        switch (format)
        {
            // 8-bit formats
            case RHIFormat::R8_UNORM:      return {GL_R8, GL_RED, GL_UNSIGNED_BYTE, false};
            case RHIFormat::R8_SNORM:      return {GL_R8_SNORM, GL_RED, GL_BYTE, false};
            case RHIFormat::R8_UINT:       return {GL_R8UI, GL_RED_INTEGER, GL_UNSIGNED_BYTE, false};
            case RHIFormat::R8_SINT:       return {GL_R8I, GL_RED_INTEGER, GL_BYTE, false};
            
            // 16-bit formats
            case RHIFormat::R16_FLOAT:     return {GL_R16F, GL_RED, GL_HALF_FLOAT, false};
            case RHIFormat::R16_UNORM:     return {GL_R16, GL_RED, GL_UNSIGNED_SHORT, false};
            case RHIFormat::R16_UINT:      return {GL_R16UI, GL_RED_INTEGER, GL_UNSIGNED_SHORT, false};
            case RHIFormat::R16_SINT:      return {GL_R16I, GL_RED_INTEGER, GL_SHORT, false};
            case RHIFormat::RG8_UNORM:     return {GL_RG8, GL_RG, GL_UNSIGNED_BYTE, false};
            case RHIFormat::RG8_SNORM:     return {GL_RG8_SNORM, GL_RG, GL_BYTE, false};
            case RHIFormat::RG8_UINT:      return {GL_RG8UI, GL_RG_INTEGER, GL_UNSIGNED_BYTE, false};
            case RHIFormat::RG8_SINT:      return {GL_RG8I, GL_RG_INTEGER, GL_BYTE, false};
            
            // 32-bit formats
            case RHIFormat::R32_FLOAT:     return {GL_R32F, GL_RED, GL_FLOAT, false};
            case RHIFormat::R32_UINT:      return {GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, false};
            case RHIFormat::R32_SINT:      return {GL_R32I, GL_RED_INTEGER, GL_INT, false};
            case RHIFormat::RG16_FLOAT:    return {GL_RG16F, GL_RG, GL_HALF_FLOAT, false};
            case RHIFormat::RG16_UNORM:    return {GL_RG16, GL_RG, GL_UNSIGNED_SHORT, false};
            case RHIFormat::RG16_UINT:     return {GL_RG16UI, GL_RG_INTEGER, GL_UNSIGNED_SHORT, false};
            case RHIFormat::RG16_SINT:     return {GL_RG16I, GL_RG_INTEGER, GL_SHORT, false};
            case RHIFormat::RGBA8_UNORM:   return {GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, false};
            case RHIFormat::RGBA8_UNORM_SRGB: return {GL_SRGB8_ALPHA8, GL_RGBA, GL_UNSIGNED_BYTE, false};
            case RHIFormat::RGBA8_SNORM:   return {GL_RGBA8_SNORM, GL_RGBA, GL_BYTE, false};
            case RHIFormat::RGBA8_UINT:    return {GL_RGBA8UI, GL_RGBA_INTEGER, GL_UNSIGNED_BYTE, false};
            case RHIFormat::RGBA8_SINT:    return {GL_RGBA8I, GL_RGBA_INTEGER, GL_BYTE, false};
            case RHIFormat::BGRA8_UNORM:   return {GL_RGBA8, GL_BGRA, GL_UNSIGNED_BYTE, false};
            case RHIFormat::BGRA8_UNORM_SRGB: return {GL_SRGB8_ALPHA8, GL_BGRA, GL_UNSIGNED_BYTE, false};
            case RHIFormat::RGB10A2_UNORM: return {GL_RGB10_A2, GL_RGBA, GL_UNSIGNED_INT_2_10_10_10_REV, false};
            case RHIFormat::RGB10A2_UINT:  return {GL_RGB10_A2UI, GL_RGBA_INTEGER, GL_UNSIGNED_INT_2_10_10_10_REV, false};
            case RHIFormat::RG11B10_FLOAT: return {GL_R11F_G11F_B10F, GL_RGB, GL_UNSIGNED_INT_10F_11F_11F_REV, false};
            
            // 96-bit formats (vertex data)
            case RHIFormat::RGB32_FLOAT:   return {GL_RGB32F, GL_RGB, GL_FLOAT, false};
            case RHIFormat::RGB32_UINT:    return {GL_RGB32UI, GL_RGB_INTEGER, GL_UNSIGNED_INT, false};
            case RHIFormat::RGB32_SINT:    return {GL_RGB32I, GL_RGB_INTEGER, GL_INT, false};
            
            // 64-bit formats
            case RHIFormat::RG32_FLOAT:    return {GL_RG32F, GL_RG, GL_FLOAT, false};
            case RHIFormat::RG32_UINT:     return {GL_RG32UI, GL_RG_INTEGER, GL_UNSIGNED_INT, false};
            case RHIFormat::RG32_SINT:     return {GL_RG32I, GL_RG_INTEGER, GL_INT, false};
            case RHIFormat::RGBA16_FLOAT:  return {GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT, false};
            case RHIFormat::RGBA16_UNORM:  return {GL_RGBA16, GL_RGBA, GL_UNSIGNED_SHORT, false};
            case RHIFormat::RGBA16_UINT:   return {GL_RGBA16UI, GL_RGBA_INTEGER, GL_UNSIGNED_SHORT, false};
            case RHIFormat::RGBA16_SINT:   return {GL_RGBA16I, GL_RGBA_INTEGER, GL_SHORT, false};
            
            // 128-bit formats
            case RHIFormat::RGBA32_FLOAT:  return {GL_RGBA32F, GL_RGBA, GL_FLOAT, false};
            case RHIFormat::RGBA32_UINT:   return {GL_RGBA32UI, GL_RGBA_INTEGER, GL_UNSIGNED_INT, false};
            case RHIFormat::RGBA32_SINT:   return {GL_RGBA32I, GL_RGBA_INTEGER, GL_INT, false};
            
            // Depth-Stencil formats
            case RHIFormat::D16_UNORM:        return {GL_DEPTH_COMPONENT16, GL_DEPTH_COMPONENT, GL_UNSIGNED_SHORT, false};
            case RHIFormat::D24_UNORM_S8_UINT: return {GL_DEPTH24_STENCIL8, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, false};
            case RHIFormat::D32_FLOAT:        return {GL_DEPTH_COMPONENT32F, GL_DEPTH_COMPONENT, GL_FLOAT, false};
            case RHIFormat::D32_FLOAT_S8_UINT: return {GL_DEPTH32F_STENCIL8, GL_DEPTH_STENCIL, GL_FLOAT_32_UNSIGNED_INT_24_8_REV, false};
            
            // Compressed formats (BC/DXT) - S3TC extension constants
            case RHIFormat::BC1_UNORM:      return {0x83F1, 0, 0, true}; // GL_COMPRESSED_RGBA_S3TC_DXT1_EXT
            case RHIFormat::BC1_UNORM_SRGB: return {0x8C4D, 0, 0, true}; // GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT1_EXT
            case RHIFormat::BC2_UNORM:      return {0x83F2, 0, 0, true}; // GL_COMPRESSED_RGBA_S3TC_DXT3_EXT
            case RHIFormat::BC2_UNORM_SRGB: return {0x8C4E, 0, 0, true}; // GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT3_EXT
            case RHIFormat::BC3_UNORM:      return {0x83F3, 0, 0, true}; // GL_COMPRESSED_RGBA_S3TC_DXT5_EXT
            case RHIFormat::BC3_UNORM_SRGB: return {0x8C4F, 0, 0, true}; // GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT
            case RHIFormat::BC4_UNORM:      return {GL_COMPRESSED_RED_RGTC1, 0, 0, true};
            case RHIFormat::BC4_SNORM:      return {GL_COMPRESSED_SIGNED_RED_RGTC1, 0, 0, true};
            case RHIFormat::BC5_UNORM:      return {GL_COMPRESSED_RG_RGTC2, 0, 0, true};
            case RHIFormat::BC5_SNORM:      return {GL_COMPRESSED_SIGNED_RG_RGTC2, 0, 0, true};
            case RHIFormat::BC6H_UF16:      return {GL_COMPRESSED_RGB_BPTC_UNSIGNED_FLOAT, 0, 0, true};
            case RHIFormat::BC6H_SF16:      return {GL_COMPRESSED_RGB_BPTC_SIGNED_FLOAT, 0, 0, true};
            case RHIFormat::BC7_UNORM:      return {GL_COMPRESSED_RGBA_BPTC_UNORM, 0, 0, true};
            case RHIFormat::BC7_UNORM_SRGB: return {GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM, 0, 0, true};
            
            default:
                return {0, 0, 0, false};
        }
    }

    // =============================================================================
    // Primitive Topology
    // =============================================================================
    inline GLenum ToGLPrimitiveMode(RHIPrimitiveTopology topology)
    {
        switch (topology)
        {
            case RHIPrimitiveTopology::PointList:     return GL_POINTS;
            case RHIPrimitiveTopology::LineList:      return GL_LINES;
            case RHIPrimitiveTopology::LineStrip:     return GL_LINE_STRIP;
            case RHIPrimitiveTopology::TriangleList:  return GL_TRIANGLES;
            case RHIPrimitiveTopology::TriangleStrip: return GL_TRIANGLE_STRIP;
            default:                                  return GL_TRIANGLES;
        }
    }

    // =============================================================================
    // Cull Mode
    // =============================================================================
    inline GLenum ToGLCullMode(RHICullMode mode)
    {
        switch (mode)
        {
            case RHICullMode::None:  return GL_NONE;
            case RHICullMode::Front: return GL_FRONT;
            case RHICullMode::Back:  return GL_BACK;
            default:                 return GL_BACK;
        }
    }

    // =============================================================================
    // Front Face
    // =============================================================================
    inline GLenum ToGLFrontFace(RHIFrontFace face)
    {
        switch (face)
        {
            case RHIFrontFace::CounterClockwise: return GL_CCW;
            case RHIFrontFace::Clockwise:        return GL_CW;
            default:                             return GL_CCW;
        }
    }

    // =============================================================================
    // Fill Mode
    // =============================================================================
    inline GLenum ToGLPolygonMode(RHIFillMode mode)
    {
        switch (mode)
        {
            case RHIFillMode::Solid:     return GL_FILL;
            case RHIFillMode::Wireframe: return GL_LINE;
            default:                     return GL_FILL;
        }
    }

    // =============================================================================
    // Compare Function
    // =============================================================================
    inline GLenum ToGLCompareFunc(RHICompareOp op)
    {
        switch (op)
        {
            case RHICompareOp::Never:        return GL_NEVER;
            case RHICompareOp::Less:         return GL_LESS;
            case RHICompareOp::Equal:        return GL_EQUAL;
            case RHICompareOp::LessEqual:    return GL_LEQUAL;
            case RHICompareOp::Greater:      return GL_GREATER;
            case RHICompareOp::NotEqual:     return GL_NOTEQUAL;
            case RHICompareOp::GreaterEqual: return GL_GEQUAL;
            case RHICompareOp::Always:       return GL_ALWAYS;
            default:                         return GL_LESS;
        }
    }

    // =============================================================================
    // Stencil Operation
    // =============================================================================
    inline GLenum ToGLStencilOp(RHIStencilOp op)
    {
        switch (op)
        {
            case RHIStencilOp::Keep:           return GL_KEEP;
            case RHIStencilOp::Zero:           return GL_ZERO;
            case RHIStencilOp::Replace:        return GL_REPLACE;
            case RHIStencilOp::IncrementClamp: return GL_INCR;
            case RHIStencilOp::DecrementClamp: return GL_DECR;
            case RHIStencilOp::Invert:         return GL_INVERT;
            case RHIStencilOp::IncrementWrap:  return GL_INCR_WRAP;
            case RHIStencilOp::DecrementWrap:  return GL_DECR_WRAP;
            default:                           return GL_KEEP;
        }
    }

    // =============================================================================
    // Blend Factor
    // =============================================================================
    inline GLenum ToGLBlendFactor(RHIBlendFactor factor)
    {
        switch (factor)
        {
            case RHIBlendFactor::Zero:             return GL_ZERO;
            case RHIBlendFactor::One:              return GL_ONE;
            case RHIBlendFactor::SrcColor:         return GL_SRC_COLOR;
            case RHIBlendFactor::InvSrcColor:      return GL_ONE_MINUS_SRC_COLOR;
            case RHIBlendFactor::SrcAlpha:         return GL_SRC_ALPHA;
            case RHIBlendFactor::InvSrcAlpha:      return GL_ONE_MINUS_SRC_ALPHA;
            case RHIBlendFactor::DstColor:         return GL_DST_COLOR;
            case RHIBlendFactor::InvDstColor:      return GL_ONE_MINUS_DST_COLOR;
            case RHIBlendFactor::DstAlpha:         return GL_DST_ALPHA;
            case RHIBlendFactor::InvDstAlpha:      return GL_ONE_MINUS_DST_ALPHA;
            case RHIBlendFactor::SrcAlphaSaturate: return GL_SRC_ALPHA_SATURATE;
            case RHIBlendFactor::ConstantColor:    return GL_CONSTANT_COLOR;
            case RHIBlendFactor::InvConstantColor: return GL_ONE_MINUS_CONSTANT_COLOR;
            default:                               return GL_ONE;
        }
    }

    // =============================================================================
    // Blend Operation
    // =============================================================================
    inline GLenum ToGLBlendOp(RHIBlendOp op)
    {
        switch (op)
        {
            case RHIBlendOp::Add:             return GL_FUNC_ADD;
            case RHIBlendOp::Subtract:        return GL_FUNC_SUBTRACT;
            case RHIBlendOp::ReverseSubtract: return GL_FUNC_REVERSE_SUBTRACT;
            case RHIBlendOp::Min:             return GL_MIN;
            case RHIBlendOp::Max:             return GL_MAX;
            default:                          return GL_FUNC_ADD;
        }
    }

    // =============================================================================
    // Sampler Address Mode
    // =============================================================================
    inline GLenum ToGLAddressMode(RHIAddressMode mode)
    {
        switch (mode)
        {
            case RHIAddressMode::Repeat:        return GL_REPEAT;
            case RHIAddressMode::MirrorRepeat:  return GL_MIRRORED_REPEAT;
            case RHIAddressMode::ClampToEdge:   return GL_CLAMP_TO_EDGE;
            case RHIAddressMode::ClampToBorder: return GL_CLAMP_TO_BORDER;
            default:                            return GL_REPEAT;
        }
    }

    // =============================================================================
    // Sampler Filter
    // =============================================================================
    inline GLenum ToGLMinFilter(RHIFilterMode minFilter, RHIFilterMode mipFilter)
    {
        if (minFilter == RHIFilterMode::Nearest)
        {
            return mipFilter == RHIFilterMode::Nearest ? 
                   GL_NEAREST_MIPMAP_NEAREST : GL_NEAREST_MIPMAP_LINEAR;
        }
        else
        {
            return mipFilter == RHIFilterMode::Nearest ? 
                   GL_LINEAR_MIPMAP_NEAREST : GL_LINEAR_MIPMAP_LINEAR;
        }
    }

    inline GLenum ToGLMagFilter(RHIFilterMode filter)
    {
        return filter == RHIFilterMode::Nearest ? GL_NEAREST : GL_LINEAR;
    }

    // =============================================================================
    // Index Type
    // =============================================================================
    inline GLenum ToGLIndexType(RHIFormat format)
    {
        switch (format)
        {
            case RHIFormat::R16_UINT: return GL_UNSIGNED_SHORT;
            case RHIFormat::R32_UINT: return GL_UNSIGNED_INT;
            default:                  return GL_UNSIGNED_INT;
        }
    }

    inline uint32 GetIndexSize(RHIFormat format)
    {
        switch (format)
        {
            case RHIFormat::R16_UINT: return 2;
            case RHIFormat::R32_UINT: return 4;
            default:                  return 4;
        }
    }

    // =============================================================================
    // Texture Target
    // =============================================================================
    inline GLenum ToGLTextureTarget(RHITextureDimension dim, bool isArray, bool isMultisample)
    {
        switch (dim)
        {
            case RHITextureDimension::Texture1D:
                return isArray ? GL_TEXTURE_1D_ARRAY : GL_TEXTURE_1D;
            case RHITextureDimension::Texture2D:
                if (isMultisample)
                    return isArray ? GL_TEXTURE_2D_MULTISAMPLE_ARRAY : GL_TEXTURE_2D_MULTISAMPLE;
                return isArray ? GL_TEXTURE_2D_ARRAY : GL_TEXTURE_2D;
            case RHITextureDimension::Texture3D:
                return GL_TEXTURE_3D;
            case RHITextureDimension::TextureCube:
                return isArray ? GL_TEXTURE_CUBE_MAP_ARRAY : GL_TEXTURE_CUBE_MAP;
            default:
                return GL_TEXTURE_2D;
        }
    }

    // =============================================================================
    // Buffer Usage to GL Flags
    // =============================================================================
    inline GLbitfield ToGLBufferStorageFlags(RHIBufferUsage usage, RHIMemoryType memoryType)
    {
        GLbitfield flags = 0;

        switch (memoryType)
        {
            case RHIMemoryType::Upload:
                // CPU writable, for staging uploads
                flags |= GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_DYNAMIC_STORAGE_BIT;
                break;
            case RHIMemoryType::Readback:
                // CPU readable, for reading back from GPU
                flags |= GL_MAP_READ_BIT | GL_MAP_PERSISTENT_BIT;
                break;
            case RHIMemoryType::Default:
            default:
                // GPU only - may still need dynamic updates via glBufferSubData
                if (HasFlag(usage, RHIBufferUsage::Constant))
                    flags |= GL_DYNAMIC_STORAGE_BIT;  // Constant buffers need frequent updates
                break;
        }

        return flags;
    }

    // =============================================================================
    // Shader Stage to GL Type
    // =============================================================================
    inline GLenum ToGLShaderType(RHIShaderStage stage)
    {
        switch (stage)
        {
            case RHIShaderStage::Vertex:   return GL_VERTEX_SHADER;
            case RHIShaderStage::Pixel:    return GL_FRAGMENT_SHADER;
            case RHIShaderStage::Geometry: return GL_GEOMETRY_SHADER;
            case RHIShaderStage::Hull:     return GL_TESS_CONTROL_SHADER;
            case RHIShaderStage::Domain:   return GL_TESS_EVALUATION_SHADER;
            case RHIShaderStage::Compute:  return GL_COMPUTE_SHADER;
            default:                       return 0;
        }
    }

    // =============================================================================
    // Vertex Format (for VAO)
    // =============================================================================
    struct GLVertexFormatInfo
    {
        GLint components;    // Number of components (1-4)
        GLenum type;         // GL type (e.g., GL_FLOAT)
        GLboolean normalized;// Whether to normalize
        GLuint size;         // Size in bytes
    };

    inline GLVertexFormatInfo ToGLVertexFormat(RHIFormat format)
    {
        switch (format)
        {
            case RHIFormat::R32_FLOAT:     return {1, GL_FLOAT, GL_FALSE, 4};
            case RHIFormat::RG32_FLOAT:    return {2, GL_FLOAT, GL_FALSE, 8};
            case RHIFormat::RGB32_FLOAT:   return {3, GL_FLOAT, GL_FALSE, 12};
            case RHIFormat::RGBA32_FLOAT:  return {4, GL_FLOAT, GL_FALSE, 16};
            
            case RHIFormat::R32_UINT:      return {1, GL_UNSIGNED_INT, GL_FALSE, 4};
            case RHIFormat::RG32_UINT:     return {2, GL_UNSIGNED_INT, GL_FALSE, 8};
            case RHIFormat::RGB32_UINT:    return {3, GL_UNSIGNED_INT, GL_FALSE, 12};
            case RHIFormat::RGBA32_UINT:   return {4, GL_UNSIGNED_INT, GL_FALSE, 16};
            
            case RHIFormat::R32_SINT:      return {1, GL_INT, GL_FALSE, 4};
            case RHIFormat::RG32_SINT:     return {2, GL_INT, GL_FALSE, 8};
            case RHIFormat::RGB32_SINT:    return {3, GL_INT, GL_FALSE, 12};
            case RHIFormat::RGBA32_SINT:   return {4, GL_INT, GL_FALSE, 16};
            
            case RHIFormat::RGBA8_UNORM:   return {4, GL_UNSIGNED_BYTE, GL_TRUE, 4};
            case RHIFormat::RGBA8_UINT:    return {4, GL_UNSIGNED_BYTE, GL_FALSE, 4};
            case RHIFormat::RGBA8_SINT:    return {4, GL_BYTE, GL_FALSE, 4};
            case RHIFormat::RGBA8_SNORM:   return {4, GL_BYTE, GL_TRUE, 4};
            
            case RHIFormat::RG16_FLOAT:    return {2, GL_HALF_FLOAT, GL_FALSE, 4};
            case RHIFormat::RGBA16_FLOAT:  return {4, GL_HALF_FLOAT, GL_FALSE, 8};
            
            default:                       return {0, 0, GL_FALSE, 0};
        }
    }

} // namespace RVX
