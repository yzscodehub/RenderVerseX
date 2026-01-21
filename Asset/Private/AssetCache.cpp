#include "Asset/AssetCache.h"

namespace RVX::Asset
{

AssetCache::AssetCache(const CacheConfig& config)
    : m_config(config)
{
}

AssetCache::~AssetCache()
{
    Clear();
}

void AssetCache::Store(Asset* asset)
{
    if (!asset) return;

    std::lock_guard<std::mutex> lock(m_mutex);
    
    AssetId id = asset->GetId();
    
    // Remove existing if present
    auto it = m_assets.find(id);
    if (it != m_assets.end())
    {
        RemoveLRU(id);
    }

    // Add to cache
    asset->AddRef(); // Cache holds a reference
    m_assets[id] = asset;
    
    // Add to LRU
    if (m_config.useLRU)
    {
        m_lruList.push_front(id);
        m_lruMap[id] = m_lruList.begin();
    }
}

Asset* AssetCache::Get(AssetId id)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    
    auto it = m_assets.find(id);
    if (it != m_assets.end())
    {
        m_hitCount++;
        TouchLRU(id);
        return it->second;
    }

    m_missCount++;
    return nullptr;
}

bool AssetCache::Contains(AssetId id) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_assets.find(id) != m_assets.end();
}

void AssetCache::Remove(AssetId id)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    
    auto it = m_assets.find(id);
    if (it != m_assets.end())
    {
        RemoveLRU(id);
        if (it->second->Release())
        {
            delete it->second;
        }
        m_assets.erase(it);
    }
}

void AssetCache::Clear()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    
    for (auto& [id, asset] : m_assets)
    {
        if (asset->Release())
        {
            delete asset;
        }
    }
    
    m_assets.clear();
    m_lruList.clear();
    m_lruMap.clear();
}

size_t AssetCache::GetMemoryUsage() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    
    size_t total = 0;
    for (const auto& [id, asset] : m_assets)
    {
        total += asset->GetMemoryUsage();
    }
    return total;
}

size_t AssetCache::GetGPUMemoryUsage() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    
    size_t total = 0;
    for (const auto& [id, asset] : m_assets)
    {
        total += asset->GetGPUMemoryUsage();
    }
    return total;
}

void AssetCache::SetMemoryLimit(size_t bytes)
{
    m_config.maxMemoryBytes = bytes;
    
    // Evict if over limit
    if (bytes > 0 && GetMemoryUsage() > bytes)
    {
        Evict(bytes);
    }
}

void AssetCache::Evict(size_t targetBytes)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    
    while (GetMemoryUsage() > targetBytes && !m_lruList.empty())
    {
        AssetId oldestId = m_lruList.back();
        
        auto it = m_assets.find(oldestId);
        if (it != m_assets.end())
        {
            // Only evict if refcount is 1 (only cache holds reference)
            if (it->second->GetRefCount() == 1)
            {
                RemoveLRU(oldestId);
                if (it->second->Release())
                {
                    delete it->second;
                }
                m_assets.erase(it);
            }
            else
            {
                // Move to front (can't evict, still in use)
                TouchLRU(oldestId);
            }
        }
    }
}

void AssetCache::EvictUnused()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    
    std::vector<AssetId> toRemove;
    
    for (const auto& [id, asset] : m_assets)
    {
        if (asset->GetRefCount() == 1)
        {
            toRemove.push_back(id);
        }
    }
    
    for (AssetId id : toRemove)
    {
        auto it = m_assets.find(id);
        if (it != m_assets.end())
        {
            RemoveLRU(id);
            if (it->second->Release())
            {
                delete it->second;
            }
            m_assets.erase(it);
        }
    }
}

AssetCache::Stats AssetCache::GetStats() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    
    Stats stats;
    stats.totalAssets = m_assets.size();
    stats.hitCount = m_hitCount;
    stats.missCount = m_missCount;
    
    for (const auto& [id, asset] : m_assets)
    {
        stats.memoryUsage += asset->GetMemoryUsage();
        stats.gpuMemoryUsage += asset->GetGPUMemoryUsage();
    }
    
    return stats;
}

void AssetCache::ResetStats()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_hitCount = 0;
    m_missCount = 0;
}

void AssetCache::TouchLRU(AssetId id)
{
    if (!m_config.useLRU) return;
    
    auto it = m_lruMap.find(id);
    if (it != m_lruMap.end())
    {
        m_lruList.erase(it->second);
        m_lruList.push_front(id);
        it->second = m_lruList.begin();
    }
}

void AssetCache::RemoveLRU(AssetId id)
{
    auto it = m_lruMap.find(id);
    if (it != m_lruMap.end())
    {
        m_lruList.erase(it->second);
        m_lruMap.erase(it);
    }
}

} // namespace RVX::Asset
