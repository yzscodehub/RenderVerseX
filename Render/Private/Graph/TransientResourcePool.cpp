/**
 * @file TransientResourcePool.cpp
 * @brief TransientResourcePool implementation
 */

#include "Render/Graph/TransientResourcePool.h"
#include "Core/Log.h"
#include <functional>

namespace RVX
{

TransientResourcePool::~TransientResourcePool()
{
    Shutdown();
}

void TransientResourcePool::Initialize(IRHIDevice* device)
{
    if (m_device)
    {
        RVX_CORE_WARN("TransientResourcePool: Already initialized");
        return;
    }

    m_device = device;
    m_currentFrame = 0;
    m_stats = {};

    RVX_CORE_DEBUG("TransientResourcePool: Initialized");
}

void TransientResourcePool::Shutdown()
{
    if (!m_device)
        return;

    // Clear all pooled resources
    m_texturePool.clear();
    m_bufferPool.clear();
    m_device = nullptr;
    m_stats = {};

    RVX_CORE_DEBUG("TransientResourcePool: Shutdown");
}

void TransientResourcePool::BeginFrame()
{
    m_currentFrame++;
    ResetFrameStats();
}

void TransientResourcePool::EndFrame()
{
    // Update statistics
    m_stats.texturePoolSize = static_cast<uint32>(m_texturePool.size());
    m_stats.bufferPoolSize = static_cast<uint32>(m_bufferPool.size());
    
    uint64 totalMemory = 0;
    for (const auto& [hash, pooled] : m_texturePool)
    {
        totalMemory += pooled.memorySize;
    }
    for (const auto& [hash, pooled] : m_bufferPool)
    {
        totalMemory += pooled.memorySize;
    }
    m_stats.totalPooledMemory = totalMemory;
}

RHITexture* TransientResourcePool::AcquireTexture(const RHITextureDesc& desc)
{
    if (!m_device)
        return nullptr;

    uint64 hash = HashTextureDesc(desc);

    // Look for an available texture with matching hash
    auto range = m_texturePool.equal_range(hash);
    for (auto it = range.first; it != range.second; ++it)
    {
        if (!it->second.inUse)
        {
            it->second.inUse = true;
            it->second.lastUsedFrame = m_currentFrame;
            m_stats.textureHits++;
            m_stats.texturesInUse++;
            return it->second.texture.Get();
        }
    }

    // No available texture found, create a new one
    RHITextureRef newTexture = m_device->CreateTexture(desc);
    if (!newTexture)
    {
        RVX_CORE_ERROR("TransientResourcePool: Failed to create texture");
        return nullptr;
    }

    PooledTexture pooled;
    pooled.texture = std::move(newTexture);
    pooled.descHash = hash;
    pooled.lastUsedFrame = m_currentFrame;
    pooled.memorySize = EstimateTextureMemory(desc);
    pooled.inUse = true;

    RHITexture* result = pooled.texture.Get();
    m_texturePool.emplace(hash, std::move(pooled));
    
    m_stats.textureMisses++;
    m_stats.texturesInUse++;

    return result;
}

RHIBuffer* TransientResourcePool::AcquireBuffer(const RHIBufferDesc& desc)
{
    if (!m_device)
        return nullptr;

    uint64 hash = HashBufferDesc(desc);

    // Look for an available buffer with matching hash
    auto range = m_bufferPool.equal_range(hash);
    for (auto it = range.first; it != range.second; ++it)
    {
        if (!it->second.inUse)
        {
            it->second.inUse = true;
            it->second.lastUsedFrame = m_currentFrame;
            m_stats.bufferHits++;
            m_stats.buffersInUse++;
            return it->second.buffer.Get();
        }
    }

    // No available buffer found, create a new one
    RHIBufferRef newBuffer = m_device->CreateBuffer(desc);
    if (!newBuffer)
    {
        RVX_CORE_ERROR("TransientResourcePool: Failed to create buffer");
        return nullptr;
    }

    PooledBuffer pooled;
    pooled.buffer = std::move(newBuffer);
    pooled.descHash = hash;
    pooled.lastUsedFrame = m_currentFrame;
    pooled.memorySize = EstimateBufferMemory(desc);
    pooled.inUse = true;

    RHIBuffer* result = pooled.buffer.Get();
    m_bufferPool.emplace(hash, std::move(pooled));
    
    m_stats.bufferMisses++;
    m_stats.buffersInUse++;

    return result;
}

void TransientResourcePool::ReleaseTexture(RHITexture* texture)
{
    if (!texture)
        return;

    for (auto& [hash, pooled] : m_texturePool)
    {
        if (pooled.texture.Get() == texture && pooled.inUse)
        {
            pooled.inUse = false;
            if (m_stats.texturesInUse > 0)
                m_stats.texturesInUse--;
            return;
        }
    }

    RVX_CORE_WARN("TransientResourcePool: ReleaseTexture called on unknown texture");
}

void TransientResourcePool::ReleaseBuffer(RHIBuffer* buffer)
{
    if (!buffer)
        return;

    for (auto& [hash, pooled] : m_bufferPool)
    {
        if (pooled.buffer.Get() == buffer && pooled.inUse)
        {
            pooled.inUse = false;
            if (m_stats.buffersInUse > 0)
                m_stats.buffersInUse--;
            return;
        }
    }

    RVX_CORE_WARN("TransientResourcePool: ReleaseBuffer called on unknown buffer");
}

void TransientResourcePool::EvictUnused(uint32 frameThreshold)
{
    uint32 evictedTextures = 0;
    uint32 evictedBuffers = 0;
    uint64 freedMemory = 0;

    // Evict unused textures
    for (auto it = m_texturePool.begin(); it != m_texturePool.end(); )
    {
        if (!it->second.inUse && 
            m_currentFrame - it->second.lastUsedFrame >= frameThreshold)
        {
            freedMemory += it->second.memorySize;
            it = m_texturePool.erase(it);
            evictedTextures++;
        }
        else
        {
            ++it;
        }
    }

    // Evict unused buffers
    for (auto it = m_bufferPool.begin(); it != m_bufferPool.end(); )
    {
        if (!it->second.inUse && 
            m_currentFrame - it->second.lastUsedFrame >= frameThreshold)
        {
            freedMemory += it->second.memorySize;
            it = m_bufferPool.erase(it);
            evictedBuffers++;
        }
        else
        {
            ++it;
        }
    }

    if (evictedTextures > 0 || evictedBuffers > 0)
    {
        RVX_CORE_DEBUG("TransientResourcePool: Evicted {} textures, {} buffers, freed {} KB",
                       evictedTextures, evictedBuffers, freedMemory / 1024);
    }
}

TransientResourcePool::Stats TransientResourcePool::GetStats() const
{
    return m_stats;
}

void TransientResourcePool::ResetFrameStats()
{
    m_stats.textureHits = 0;
    m_stats.textureMisses = 0;
    m_stats.bufferHits = 0;
    m_stats.bufferMisses = 0;
}

uint64 TransientResourcePool::HashTextureDesc(const RHITextureDesc& desc)
{
    // Simple hash combining key texture properties
    uint64 hash = 0;
    auto hashCombine = [&hash](uint64 value) {
        hash ^= value + 0x9e3779b97f4a7c15ULL + (hash << 6) + (hash >> 2);
    };

    hashCombine(desc.width);
    hashCombine(desc.height);
    hashCombine(desc.depth);
    hashCombine(desc.mipLevels);
    hashCombine(desc.arraySize);
    hashCombine(static_cast<uint64>(desc.format));
    hashCombine(static_cast<uint64>(desc.dimension));
    hashCombine(static_cast<uint64>(desc.usage));
    hashCombine(static_cast<uint64>(desc.sampleCount));

    return hash;
}

uint64 TransientResourcePool::HashBufferDesc(const RHIBufferDesc& desc)
{
    uint64 hash = 0;
    auto hashCombine = [&hash](uint64 value) {
        hash ^= value + 0x9e3779b97f4a7c15ULL + (hash << 6) + (hash >> 2);
    };

    hashCombine(desc.size);
    hashCombine(static_cast<uint64>(desc.usage));
    hashCombine(static_cast<uint64>(desc.memoryType));
    hashCombine(desc.stride);

    return hash;
}

uint64 TransientResourcePool::EstimateTextureMemory(const RHITextureDesc& desc)
{
    uint64 bpp = GetFormatBytesPerPixel(desc.format);
    if (bpp == 0) bpp = 4;  // Fallback

    uint64 totalSize = 0;
    uint32 width = desc.width;
    uint32 height = desc.height;
    uint32 depth = desc.depth;

    for (uint32 mip = 0; mip < desc.mipLevels; ++mip)
    {
        uint64 mipSize = static_cast<uint64>(width) * height * depth * bpp;
        totalSize += mipSize * desc.arraySize;

        width = std::max(1u, width / 2);
        height = std::max(1u, height / 2);
        depth = std::max(1u, depth / 2);
    }

    totalSize *= static_cast<uint32>(desc.sampleCount);

    return totalSize;
}

uint64 TransientResourcePool::EstimateBufferMemory(const RHIBufferDesc& desc)
{
    return desc.size;
}

} // namespace RVX
