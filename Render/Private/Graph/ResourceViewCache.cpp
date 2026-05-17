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
    m_generation = 0;
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

    TextureViewKey key = MakeTextureViewKey(texture, desc);

    // Check cache
    auto it = m_textureViews.find(key);
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
    m_textureViews.emplace(key, std::move(cached));

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

    bool invalidated = false;

    // Remove all views associated with this texture
    for (auto it = m_textureViews.begin(); it != m_textureViews.end(); )
    {
        if (it->second.texture == texture)
        {
            it = m_textureViews.erase(it);
            invalidated = true;
            if (m_stats.textureViewCount > 0)
                m_stats.textureViewCount--;
        }
        else
        {
            ++it;
        }
    }

    if (invalidated)
    {
        ++m_generation;
    }
}

void ResourceViewCache::Clear()
{
    if (!m_textureViews.empty())
    {
        ++m_generation;
        m_textureViews.clear();
    }
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

ResourceViewCache::TextureViewKey ResourceViewCache::MakeTextureViewKey(RHITexture* texture, const RHITextureViewDesc& desc)
{
    TextureViewKey key;
    key.texture = texture;
    key.format = desc.format;
    key.dimension = desc.dimension;
    key.subresourceRange = desc.subresourceRange;
    return key;
}

size_t ResourceViewCache::TextureViewKeyHash::operator()(const TextureViewKey& key) const
{
    size_t hash = std::hash<RHITexture*>{}(key.texture);

    auto hashCombine = [&hash](size_t value) {
        hash ^= value + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
    };

    hashCombine(std::hash<uint32>{}(static_cast<uint32>(key.format)));
    hashCombine(std::hash<uint32>{}(static_cast<uint32>(key.dimension)));
    hashCombine(std::hash<uint32>{}(key.subresourceRange.baseMipLevel));
    hashCombine(std::hash<uint32>{}(key.subresourceRange.mipLevelCount));
    hashCombine(std::hash<uint32>{}(key.subresourceRange.baseArrayLayer));
    hashCombine(std::hash<uint32>{}(key.subresourceRange.arrayLayerCount));
    hashCombine(std::hash<uint32>{}(static_cast<uint32>(key.subresourceRange.aspect)));

    return hash;
}

} // namespace RVX
