#pragma once

#include "RHI/RHIResources.h"
#include <algorithm>

namespace RVX
{
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
    struct RHITextureViewDesc
    {
        RHIFormat format = RHIFormat::Unknown;  // Unknown = use texture format
        RHITextureDimension dimension = RHITextureDimension::Texture2D;
        RHISubresourceRange subresourceRange;
        const char* debugName = nullptr;
    };

    // =============================================================================
    // Texture View Interface
    // =============================================================================
    class RHITextureView : public RHIResource
    {
    public:
        virtual ~RHITextureView() = default;

        virtual RHITexture* GetTexture() const = 0;
        virtual RHIFormat GetFormat() const = 0;
        virtual const RHISubresourceRange& GetSubresourceRange() const = 0;
    };

} // namespace RVX
