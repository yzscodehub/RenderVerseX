#pragma once

#include "Core/Types.h"
#include <string>

namespace RVX
{
    // =============================================================================
    // Backend Type
    // =============================================================================
    enum class RHIBackendType : uint8
    {
        None = 0,
        Auto,       // Automatically select the best backend for the platform
        DX11,
        DX12,
        Vulkan,
        Metal,
        OpenGL
    };

    inline const char* ToString(RHIBackendType type)
    {
        switch (type)
        {
            case RHIBackendType::Auto:   return "Auto";
            case RHIBackendType::DX11:   return "DirectX 11";
            case RHIBackendType::DX12:   return "DirectX 12";
            case RHIBackendType::Vulkan: return "Vulkan";
            case RHIBackendType::Metal:  return "Metal";
            case RHIBackendType::OpenGL: return "OpenGL";
            default:                     return "Unknown";
        }
    }

    /**
     * @brief Select the best RHI backend for the current platform
     * @return The recommended backend type
     */
    inline RHIBackendType SelectBestBackend()
    {
#if defined(_WIN32)
        return RHIBackendType::DX12;  // Windows优先DX12
#elif defined(__APPLE__)
        return RHIBackendType::Metal;
#else
        return RHIBackendType::Vulkan;
#endif
    }

    // =============================================================================
    // Resource Formats
    // =============================================================================
    enum class RHIFormat : uint8
    {
        Unknown = 0,

        // 8-bit formats
        R8_UNORM,
        R8_SNORM,
        R8_UINT,
        R8_SINT,

        // 16-bit formats
        R16_FLOAT,
        R16_UNORM,
        R16_UINT,
        R16_SINT,
        RG8_UNORM,
        RG8_SNORM,
        RG8_UINT,
        RG8_SINT,

        // 32-bit formats
        R32_FLOAT,
        R32_UINT,
        R32_SINT,
        RG16_FLOAT,
        RG16_UNORM,
        RG16_UINT,
        RG16_SINT,
        RGBA8_UNORM,
        RGBA8_UNORM_SRGB,
        RGBA8_SNORM,
        RGBA8_UINT,
        RGBA8_SINT,
        BGRA8_UNORM,
        BGRA8_UNORM_SRGB,
        RGB10A2_UNORM,
        RGB10A2_UINT,
        RG11B10_FLOAT,

        // 96-bit formats (used for vertex data)
        RGB32_FLOAT,
        RGB32_UINT,
        RGB32_SINT,

        // 64-bit formats
        RG32_FLOAT,
        RG32_UINT,
        RG32_SINT,
        RGBA16_FLOAT,
        RGBA16_UNORM,
        RGBA16_UINT,
        RGBA16_SINT,

        // 128-bit formats
        RGBA32_FLOAT,
        RGBA32_UINT,
        RGBA32_SINT,

        // Depth-Stencil formats
        D16_UNORM,
        D24_UNORM_S8_UINT,
        D32_FLOAT,
        D32_FLOAT_S8_UINT,

        // Compressed formats (BC)
        BC1_UNORM,
        BC1_UNORM_SRGB,
        BC2_UNORM,
        BC2_UNORM_SRGB,
        BC3_UNORM,
        BC3_UNORM_SRGB,
        BC4_UNORM,
        BC4_SNORM,
        BC5_UNORM,
        BC5_SNORM,
        BC6H_UF16,
        BC6H_SF16,
        BC7_UNORM,
        BC7_UNORM_SRGB,

        Count
    };

    uint32 GetFormatBytesPerPixel(RHIFormat format);
    bool IsDepthFormat(RHIFormat format);
    bool IsStencilFormat(RHIFormat format);
    bool IsCompressedFormat(RHIFormat format);
    bool IsSRGBFormat(RHIFormat format);

    // =============================================================================
    // Resource Usage Flags
    // =============================================================================
    enum class RHIBufferUsage : uint32
    {
        None            = 0,
        Vertex          = 1 << 0,
        Index           = 1 << 1,
        Constant        = 1 << 2,
        Structured      = 1 << 3,
        IndirectArgs    = 1 << 4,
        ShaderResource  = 1 << 5,
        UnorderedAccess = 1 << 6,
        CopySrc         = 1 << 7,
        CopyDst         = 1 << 8,
    };

    inline RHIBufferUsage operator|(RHIBufferUsage a, RHIBufferUsage b)
    {
        return static_cast<RHIBufferUsage>(static_cast<uint32>(a) | static_cast<uint32>(b));
    }

    inline RHIBufferUsage operator&(RHIBufferUsage a, RHIBufferUsage b)
    {
        return static_cast<RHIBufferUsage>(static_cast<uint32>(a) & static_cast<uint32>(b));
    }

    inline bool HasFlag(RHIBufferUsage flags, RHIBufferUsage flag)
    {
        return (static_cast<uint32>(flags) & static_cast<uint32>(flag)) != 0;
    }

    enum class RHITextureUsage : uint32
    {
        None            = 0,
        ShaderResource  = 1 << 0,
        RenderTarget    = 1 << 1,
        DepthStencil    = 1 << 2,
        UnorderedAccess = 1 << 3,
        CopySrc         = 1 << 4,
        CopyDst         = 1 << 5,
        Transient       = 1 << 6,  // Memoryless/transient render target (content not preserved between passes)
    };

    inline RHITextureUsage operator|(RHITextureUsage a, RHITextureUsage b)
    {
        return static_cast<RHITextureUsage>(static_cast<uint32>(a) | static_cast<uint32>(b));
    }

    inline RHITextureUsage operator&(RHITextureUsage a, RHITextureUsage b)
    {
        return static_cast<RHITextureUsage>(static_cast<uint32>(a) & static_cast<uint32>(b));
    }

    inline bool HasFlag(RHITextureUsage flags, RHITextureUsage flag)
    {
        return (static_cast<uint32>(flags) & static_cast<uint32>(flag)) != 0;
    }

    // =============================================================================
    // Memory Types
    // =============================================================================
    enum class RHIMemoryType : uint8
    {
        Default,   // GPU-only, fastest for rendering
        Upload,    // CPU-writable, for staging uploads
        Readback,  // CPU-readable, for reading back from GPU
    };

    // =============================================================================
    // Texture Dimension
    // =============================================================================
    enum class RHITextureDimension : uint8
    {
        Texture1D,
        Texture2D,
        Texture3D,
        TextureCube,
    };

    // =============================================================================
    // Sample Count
    // =============================================================================
    enum class RHISampleCount : uint8
    {
        Count1  = 1,
        Count2  = 2,
        Count4  = 4,
        Count8  = 8,
        Count16 = 16,
    };

    // =============================================================================
    // Resource States
    // =============================================================================
    enum class RHIResourceState : uint8
    {
        Undefined,
        Common,
        VertexBuffer,
        IndexBuffer,
        ConstantBuffer,
        ShaderResource,
        UnorderedAccess,
        RenderTarget,
        DepthWrite,
        DepthRead,
        CopyDest,
        CopySource,
        Present,
        IndirectArgument,
    };

    // =============================================================================
    // Shader Stages
    // =============================================================================
    enum class RHIShaderStage : uint32
    {
        None     = 0,
        Vertex   = 1 << 0,
        Hull     = 1 << 1,
        Domain   = 1 << 2,
        Geometry = 1 << 3,
        Pixel    = 1 << 4,
        Compute  = 1 << 5,

        AllGraphics = Vertex | Hull | Domain | Geometry | Pixel,
        All         = AllGraphics | Compute,
    };

    inline RHIShaderStage operator|(RHIShaderStage a, RHIShaderStage b)
    {
        return static_cast<RHIShaderStage>(static_cast<uint32>(a) | static_cast<uint32>(b));
    }

    inline RHIShaderStage operator&(RHIShaderStage a, RHIShaderStage b)
    {
        return static_cast<RHIShaderStage>(static_cast<uint32>(a) & static_cast<uint32>(b));
    }

    inline bool HasFlag(RHIShaderStage flags, RHIShaderStage flag)
    {
        return (static_cast<uint32>(flags) & static_cast<uint32>(flag)) != 0;
    }

    // =============================================================================
    // Primitive Topology
    // =============================================================================
    enum class RHIPrimitiveTopology : uint8
    {
        PointList,
        LineList,
        LineStrip,
        TriangleList,
        TriangleStrip,
    };

    // =============================================================================
    // Command Queue Type
    // =============================================================================
    enum class RHICommandQueueType : uint8
    {
        Graphics,
        Compute,
        Copy,
    };

    // =============================================================================
    // Binding Type
    // =============================================================================
    enum class RHIBindingType : uint8
    {
        UniformBuffer,
        StorageBuffer,
        DynamicUniformBuffer,
        DynamicStorageBuffer,
        SampledTexture,
        StorageTexture,
        Sampler,
        CombinedTextureSampler,
    };

    // =============================================================================
    // Texture Aspect
    // =============================================================================
    enum class RHITextureAspect : uint8
    {
        Color,
        Depth,
        Stencil,
        DepthStencil,
    };

    // =============================================================================
    // Comparison Function
    // =============================================================================
    enum class RHICompareOp : uint8
    {
        Never,
        Less,
        Equal,
        LessEqual,
        Greater,
        NotEqual,
        GreaterEqual,
        Always,
    };

    // =============================================================================
    // Blend Factors
    // =============================================================================
    enum class RHIBlendFactor : uint8
    {
        Zero,
        One,
        SrcColor,
        InvSrcColor,
        SrcAlpha,
        InvSrcAlpha,
        DstColor,
        InvDstColor,
        DstAlpha,
        InvDstAlpha,
        SrcAlphaSaturate,
        ConstantColor,
        InvConstantColor,
    };

    enum class RHIBlendOp : uint8
    {
        Add,
        Subtract,
        ReverseSubtract,
        Min,
        Max,
    };

    // =============================================================================
    // Cull Mode
    // =============================================================================
    enum class RHICullMode : uint8
    {
        None,
        Front,
        Back,
    };

    // =============================================================================
    // Fill Mode
    // =============================================================================
    enum class RHIFillMode : uint8
    {
        Solid,
        Wireframe,
    };

    // =============================================================================
    // Front Face
    // =============================================================================
    enum class RHIFrontFace : uint8
    {
        CounterClockwise,
        Clockwise,
    };

    // =============================================================================
    // Stencil Operation
    // =============================================================================
    enum class RHIStencilOp : uint8
    {
        Keep,
        Zero,
        Replace,
        IncrementClamp,
        DecrementClamp,
        Invert,
        IncrementWrap,
        DecrementWrap,
    };

    // =============================================================================
    // Filter Mode
    // =============================================================================
    enum class RHIFilterMode : uint8
    {
        Nearest,
        Linear,
    };

    // =============================================================================
    // Address Mode
    // =============================================================================
    enum class RHIAddressMode : uint8
    {
        Repeat,
        MirrorRepeat,
        ClampToEdge,
        ClampToBorder,
    };

    // =============================================================================
    // Load/Store Operations
    // =============================================================================
    enum class RHILoadOp : uint8
    {
        Load,
        Clear,
        DontCare,
    };

    enum class RHIStoreOp : uint8
    {
        Store,
        DontCare,
    };

} // namespace RVX
