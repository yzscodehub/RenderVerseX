/**
 * @file ResourceViewCache.cpp
 * @brief ResourceViewCache implementation
 */

#include "Render/Graph/ResourceViewCache.h"
#include "Core/Log.h"

namespace RVX
{

ResourceViewCache::~ResourceViewCache()
{
    Shutdown();
}

void ResourceViewCache::Initialize(IRHIDevice* device)
{
    if (m_device)
    {
        RVX_CORE_WARN("ResourceViewCache: Already initialized");
        return;
    }

    m_device = device;
    m_currentFrame = 0;
    m_stats = {};

    RVX_CORE_DEBUG("ResourceViewCache: Initialized");
}

void ResourceViewCache::Shutdown()
{
    if (!m_device)
        return;

    Clear();
    m_device = nullptr;

    RVX_CORE_DEBUG("ResourceViewCache: Shutdown");
}

RHITextureView* ResourceViewCache::GetTextureView(RHITexture* texture, const RHITextureViewDesc& desc)
{
    if (!m_device || !texture)
        return nullptr;

    uint64 hash = HashTextureViewKey(texture, desc);

    // Check cache
    auto it = m_textureViews.find(hash);
    if (it != m_textureViews.end())
    {
        it->second.lastUsedFrame = m_currentFrame;
        m_stats.cacheHits++;
        return it->second.view.Get();
    }

    // Create new view
    RHITextureViewRef view = m_device->CreateTextureView(texture, desc);
    if (!view)
    {
        RVX_CORE_ERROR("ResourceViewCache: Failed to create texture view");
        return nullptr;
    }

    CachedTextureView cached;
    cached.view = std::move(view);
    cached.texture = texture;
    cached.lastUsedFrame = m_currentFrame;

    RHITextureView* result = cached.view.Get();
    m_textureViews[hash] = std::move(cached);

    m_stats.cacheMisses++;
    m_stats.textureViewCount++;

    return result;
}

RHITextureView* ResourceViewCache::GetDefaultSRV(RHITexture* texture)
{
    if (!texture)
        return nullptr;

    RHITextureViewDesc desc;
    desc.format = texture->GetFormat();
    desc.dimension = texture->GetDimension();
    desc.subresourceRange = RHISubresourceRange::All();
    desc.debugName = "DefaultSRV";

    return GetTextureView(texture, desc);
}

RHITextureView* ResourceViewCache::GetDefaultRTV(RHITexture* texture)
{
    if (!texture)
        return nullptr;

    RHITextureViewDesc desc;
    desc.format = texture->GetFormat();
    desc.dimension = texture->GetDimension();
    desc.subresourceRange = RHISubresourceRange::All();
    desc.debugName = "DefaultRTV";

    return GetTextureView(texture, desc);
}

RHITextureView* ResourceViewCache::GetDefaultDSV(RHITexture* texture)
{
    if (!texture)
        return nullptr;

    RHITextureViewDesc desc;
    desc.format = texture->GetFormat();
    desc.dimension = texture->GetDimension();
    desc.subresourceRange = RHISubresourceRange::All();
    // Mark as depth aspect for depth formats
    if (IsDepthFormat(desc.format))
    {
        desc.subresourceRange.aspect = RHITextureAspect::Depth;
    }
    desc.debugName = "DefaultDSV";

    return GetTextureView(texture, desc);
}

RHITextureView* ResourceViewCache::GetDefaultUAV(RHITexture* texture)
{
    if (!texture)
        return nullptr;

    RHITextureViewDesc desc;
    desc.format = texture->GetFormat();
    desc.dimension = texture->GetDimension();
    // UAV typically only covers first mip
    desc.subresourceRange = RHISubresourceRange::Mip(0);
    desc.debugName = "DefaultUAV";

    return GetTextureView(texture, desc);
}

void ResourceViewCache::BeginFrame()
{
    m_currentFrame++;
    ResetFrameStats();
}

void ResourceViewCache::InvalidateTexture(RHITexture* texture)
{
    if (!texture)
        return;

    // Remove all views associated with this texture
    for (auto it = m_textureViews.begin(); it != m_textureViews.end(); )
    {
        if (it->second.texture == texture)
        {
            it = m_textureViews.erase(it);
            if (m_stats.textureViewCount > 0)
                m_stats.textureViewCount--;
        }
        else
        {
            ++it;
        }
    }
}

void ResourceViewCache::Clear()
{
    m_textureViews.clear();
    m_stats.textureViewCount = 0;
}

ResourceViewCache::Stats ResourceViewCache::GetStats() const
{
    return m_stats;
}

void ResourceViewCache::ResetFrameStats()
{
    m_stats.cacheHits = 0;
    m_stats.cacheMisses = 0;
}

uint64 ResourceViewCache::HashTextureViewKey(RHITexture* texture, const RHITextureViewDesc& desc)
{
    uint64 hash = reinterpret_cast<uint64>(texture);
    
    auto hashCombine = [&hash](uint64 value) {
        hash ^= value + 0x9e3779b97f4a7c15ULL + (hash << 6) + (hash >> 2);
    };

    hashCombine(static_cast<uint64>(desc.format));
    hashCombine(static_cast<uint64>(desc.dimension));
    hashCombine(desc.subresourceRange.baseMipLevel);
    hashCombine(desc.subresourceRange.mipLevelCount);
    hashCombine(desc.subresourceRange.baseArrayLayer);
    hashCombine(desc.subresourceRange.arrayLayerCount);
    hashCombine(static_cast<uint64>(desc.subresourceRange.aspect));

    return hash;
}

} // namespace RVX
