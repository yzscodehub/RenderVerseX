#pragma once

#include "Core/Assert.h"
#include "RHI/RHIResources.h"
#include <algorithm>
#include <cmath>
#include <utility>

namespace RVX
{
    enum class RHIOptimizedClearValueType : uint8
    {
        None = 0,
        Color,
        DepthStencil,
    };

    /**
     * @brief Optional texture creation hint for APIs with optimized clear metadata
     *
     * The value is an optimization contract, not the texture's initial contents.
     * Render passes that clear a hinted resource must use the exact same value.
     */
    struct RHIOptimizedClearValue
    {
        RHIOptimizedClearValueType type = RHIOptimizedClearValueType::None;
        RHIClearColor color{};
        RHIClearDepthStencil depthStencil{};

        static RHIOptimizedClearValue Color(RHIClearColor value)
        {
            RHIOptimizedClearValue result;
            result.type = RHIOptimizedClearValueType::Color;
            result.color = value;
            return result;
        }

        static RHIOptimizedClearValue DepthStencil(RHIClearDepthStencil value)
        {
            RHIOptimizedClearValue result;
            result.type = RHIOptimizedClearValueType::DepthStencil;
            result.depthStencil = value;
            return result;
        }

        bool IsPresent() const { return type != RHIOptimizedClearValueType::None; }
    };

    // =============================================================================
    // Texture Description
    // =============================================================================
    struct RHITextureDesc
    {
        uint32 width = 1;
        uint32 height = 1;
        uint32 depth = 1;
        uint32 mipLevels = 1;
        /// Logical array element count. For TextureCube this is cube count;
        /// use GetTexturePhysicalLayerCount() when native API code needs face layers.
        uint32 arraySize = 1;
        RHIFormat format = RHIFormat::RGBA8_UNORM;
        RHITextureUsage usage = RHITextureUsage::ShaderResource;
        RHITextureDimension dimension = RHITextureDimension::Texture2D;
        RHISampleCount sampleCount = RHISampleCount::Count1;
        RHIOptimizedClearValue optimizedClearValue;
        const char* debugName = nullptr;

        // Builder pattern helpers
        RHITextureDesc& SetWidth(uint32 w) { width = w; return *this; }
        RHITextureDesc& SetHeight(uint32 h) { height = h; return *this; }
        RHITextureDesc& SetDepth(uint32 d) { depth = d; return *this; }
        RHITextureDesc& SetMipLevels(uint32 m) { mipLevels = m; return *this; }
        RHITextureDesc& SetArraySize(uint32 a) { arraySize = a; return *this; }
        RHITextureDesc& SetFormat(RHIFormat f) { format = f; return *this; }
        RHITextureDesc& SetUsage(RHITextureUsage u) { usage = u; return *this; }
        RHITextureDesc& SetDimension(RHITextureDimension d) { dimension = d; return *this; }
        RHITextureDesc& SetSampleCount(RHISampleCount s) { sampleCount = s; return *this; }
        RHITextureDesc& SetOptimizedClearColor(RHIClearColor value)
        {
            optimizedClearValue = RHIOptimizedClearValue::Color(value);
            return *this;
        }
        RHITextureDesc& SetOptimizedClearDepthStencil(RHIClearDepthStencil value)
        {
            optimizedClearValue = RHIOptimizedClearValue::DepthStencil(value);
            return *this;
        }
        RHITextureDesc& ClearOptimizedClearValue()
        {
            optimizedClearValue = {};
            return *this;
        }
        RHITextureDesc& SetDebugName(const char* n) { debugName = n; return *this; }

        // Convenience constructors
        static RHITextureDesc Texture2D(uint32 width, uint32 height, RHIFormat format, RHITextureUsage usage = RHITextureUsage::ShaderResource)
        {
            RHITextureDesc desc;
            desc.width = width;
            desc.height = height;
            desc.format = format;
            desc.usage = usage;
            desc.dimension = RHITextureDimension::Texture2D;
            return desc;
        }

        static RHITextureDesc RenderTarget(uint32 width, uint32 height, RHIFormat format)
        {
            return Texture2D(width, height, format, RHITextureUsage::RenderTarget | RHITextureUsage::ShaderResource);
        }

        static RHITextureDesc DepthStencil(uint32 width, uint32 height, RHIFormat format = RHIFormat::D24_UNORM_S8_UINT)
        {
            return Texture2D(width, height, format, RHITextureUsage::DepthStencil | RHITextureUsage::ShaderResource);
        }
    };

    // =============================================================================
    // Texture Interface
    // =============================================================================
    class RHITexture : public RHIResource
    {
    public:
        virtual ~RHITexture() = default;

        // Getters
        virtual uint32 GetWidth() const = 0;
        virtual uint32 GetHeight() const = 0;
        virtual uint32 GetDepth() const = 0;
        virtual uint32 GetMipLevels() const = 0;
        virtual uint32 GetArraySize() const = 0;
        virtual RHIFormat GetFormat() const = 0;
        virtual RHITextureUsage GetUsage() const = 0;
        virtual RHITextureDimension GetDimension() const = 0;
        virtual RHISampleCount GetSampleCount() const = 0;

        // Calculate subresource index
        uint32 GetSubresourceIndex(uint32 mipLevel, uint32 arraySlice) const
        {
            return mipLevel + arraySlice * GetMipLevels();
        }
    };

    inline bool AreRHIClearColorsEqual(const RHIClearColor& lhs, const RHIClearColor& rhs)
    {
        return lhs.r == rhs.r && lhs.g == rhs.g && lhs.b == rhs.b && lhs.a == rhs.a;
    }

    inline bool AreRHIClearDepthStencilValuesEqual(
        const RHIClearDepthStencil& lhs,
        const RHIClearDepthStencil& rhs)
    {
        return lhs.depth == rhs.depth && lhs.stencil == rhs.stencil;
    }

    inline bool AreRHIOptimizedClearValuesEqual(
        const RHIOptimizedClearValue& lhs,
        const RHIOptimizedClearValue& rhs)
    {
        if (lhs.type != rhs.type)
        {
            return false;
        }
        switch (lhs.type)
        {
            case RHIOptimizedClearValueType::None:
                return true;
            case RHIOptimizedClearValueType::Color:
                return AreRHIClearColorsEqual(lhs.color, rhs.color);
            case RHIOptimizedClearValueType::DepthStencil:
                return AreRHIClearDepthStencilValuesEqual(lhs.depthStencil, rhs.depthStencil);
        }
        return false;
    }

    /** @brief Validate that an optimized clear hint is legal for the texture. */
    inline bool IsRHIOptimizedClearValueCompatible(const RHITextureDesc& desc)
    {
        switch (desc.optimizedClearValue.type)
        {
            case RHIOptimizedClearValueType::None:
                return true;
            case RHIOptimizedClearValueType::Color:
            {
                const RHIClearColor& color = desc.optimizedClearValue.color;
                return HasFlag(desc.usage, RHITextureUsage::RenderTarget) &&
                       !IsDepthFormat(desc.format) &&
                       std::isfinite(color.r) && std::isfinite(color.g) &&
                       std::isfinite(color.b) && std::isfinite(color.a);
            }
            case RHIOptimizedClearValueType::DepthStencil:
            {
                const float depth = desc.optimizedClearValue.depthStencil.depth;
                return HasFlag(desc.usage, RHITextureUsage::DepthStencil) &&
                       IsDepthFormat(desc.format) && std::isfinite(depth) &&
                       depth >= 0.0f && depth <= 1.0f;
            }
        }
        return false;
    }

    /** @brief Compare allocation-relevant texture identity; debug names are excluded. */
    inline bool AreRHITextureDescsEquivalent(
        const RHITextureDesc& lhs,
        const RHITextureDesc& rhs)
    {
        return lhs.width == rhs.width && lhs.height == rhs.height &&
               lhs.depth == rhs.depth && lhs.mipLevels == rhs.mipLevels &&
               lhs.arraySize == rhs.arraySize && lhs.format == rhs.format &&
               lhs.usage == rhs.usage && lhs.dimension == rhs.dimension &&
               lhs.sampleCount == rhs.sampleCount &&
               AreRHIOptimizedClearValuesEqual(lhs.optimizedClearValue,
                                                rhs.optimizedClearValue);
    }

    struct RHITextureSubresource
    {
        uint32 mipLevel = 0;
        uint32 physicalLayer = 0;
    };

    inline uint32 GetTexturePhysicalLayerCount(const RHITextureDesc& desc)
    {
        const uint32 logicalLayers = desc.dimension == RHITextureDimension::Texture3D ? 1u : std::max(1u, desc.arraySize);
        return desc.dimension == RHITextureDimension::TextureCube ? logicalLayers * 6u : logicalLayers;
    }

    inline uint32 GetTexturePhysicalLayerCount(const RHITexture& texture)
    {
        const uint32 logicalLayers = texture.GetDimension() == RHITextureDimension::Texture3D ? 1u : std::max(1u, texture.GetArraySize());
        return texture.GetDimension() == RHITextureDimension::TextureCube ? logicalLayers * 6u : logicalLayers;
    }

    inline uint32 GetTextureSubresourceCount(const RHITextureDesc& desc)
    {
        return std::max(1u, desc.mipLevels) * GetTexturePhysicalLayerCount(desc);
    }

    inline uint32 GetTextureSubresourceCount(const RHITexture& texture)
    {
        return std::max(1u, texture.GetMipLevels()) * GetTexturePhysicalLayerCount(texture);
    }

    inline uint32 EncodeTextureSubresource(uint32 mipLevel, uint32 physicalLayer, uint32 mipLevels)
    {
        return mipLevel + physicalLayer * std::max(1u, mipLevels);
    }

    inline RHITextureSubresource DecodeTextureSubresource(uint32 subresource, uint32 mipLevels)
    {
        const uint32 mipCount = std::max(1u, mipLevels);
        return RHITextureSubresource{subresource % mipCount, subresource / mipCount};
    }

    inline uint32 ResolveTextureMipLevelCount(const RHITextureDesc& desc, const RHISubresourceRange& range)
    {
        if (range.mipLevelCount == 0 || range.mipLevelCount == RVX_ALL_MIPS)
            return std::max(1u, desc.mipLevels) - range.baseMipLevel;

        return range.mipLevelCount;
    }

    inline uint32 ResolveTextureArrayLayerCount(const RHITextureDesc& desc, const RHISubresourceRange& range)
    {
        if (range.arrayLayerCount == 0 || range.arrayLayerCount == RVX_ALL_LAYERS)
            return GetTexturePhysicalLayerCount(desc) - range.baseArrayLayer;

        return range.arrayLayerCount;
    }

    inline uint32 ResolveTextureArrayLayerCount(const RHITexture& texture, const RHISubresourceRange& range)
    {
        if (range.arrayLayerCount == 0 || range.arrayLayerCount == RVX_ALL_LAYERS)
            return GetTexturePhysicalLayerCount(texture) - range.baseArrayLayer;

        return range.arrayLayerCount;
    }

    // =============================================================================
    // Texture View Description
    // =============================================================================
    enum class RHITextureViewType : uint8
    {
        ShaderResource = 0,
        RenderTarget,
        DepthStencil,
        UnorderedAccess,
    };

    struct RHITextureViewDesc
    {
        RHIFormat format = RHIFormat::Unknown;  // Unknown = use texture format
        RHITextureDimension dimension = RHITextureDimension::Texture2D;
        RHISubresourceRange subresourceRange;
        RHITextureViewType type = RHITextureViewType::ShaderResource;
        const char* debugName = nullptr;
    };

    inline const char* GetTextureViewTypeName(RHITextureViewType type)
    {
        switch (type)
        {
            case RHITextureViewType::ShaderResource: return "ShaderResource";
            case RHITextureViewType::RenderTarget: return "RenderTarget";
            case RHITextureViewType::DepthStencil: return "DepthStencil";
            case RHITextureViewType::UnorderedAccess: return "UnorderedAccess";
            default: return "Unknown";
        }
    }

    inline bool IsTextureViewTypeCompatible(RHITextureUsage usage, RHIFormat textureFormat, const RHITextureViewDesc& desc)
    {
        const RHIFormat viewFormat = desc.format == RHIFormat::Unknown ? textureFormat : desc.format;
        switch (desc.type)
        {
            case RHITextureViewType::ShaderResource:
                return HasFlag(usage, RHITextureUsage::ShaderResource);
            case RHITextureViewType::RenderTarget:
                return HasFlag(usage, RHITextureUsage::RenderTarget) && !IsDepthFormat(viewFormat);
            case RHITextureViewType::DepthStencil:
                return HasFlag(usage, RHITextureUsage::DepthStencil) && IsDepthFormat(viewFormat);
            case RHITextureViewType::UnorderedAccess:
                return HasFlag(usage, RHITextureUsage::UnorderedAccess);
            default:
                return false;
        }
    }

    // =============================================================================
    // Texture View Interface
    // =============================================================================
    class RHITextureView : public RHIResource
    {
    public:
        explicit RHITextureView(RHITextureRef texture)
            : m_sourceTexture(std::move(texture))
        {
            RVX_ASSERT_MSG(m_sourceTexture,
                           "RHI texture views require a live source texture");
        }
        virtual ~RHITextureView() = default;

        virtual RHITexture* GetTexture() const final
        {
            return m_sourceTexture.Get();
        }
        const RHITextureRef& GetTextureRef() const { return m_sourceTexture; }
        virtual RHIFormat GetFormat() const = 0;
        virtual const RHISubresourceRange& GetSubresourceRange() const = 0;

        /**
         * @brief Optional native shader-resource handle that UI backends can consume.
         *
         * Backends return 0 when the active UI renderer cannot sample the native
         * handle directly. The OpenGL backend returns a GL texture name for the
         * OpenGL ImGui renderer.
         */
        virtual uint64 GetNativeShaderResourceHandleForUI() const { return 0; }

    private:
        // Declared in the base so every backend obeys the same source lifetime
        // contract.  The derived native view is destroyed before this member,
        // which guarantees (for example) VkImageView-before-VkImage ordering.
        RHITextureRef m_sourceTexture;
    };

} // namespace RVX
