#pragma once

#include "RHI/RHIResources.h"

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
