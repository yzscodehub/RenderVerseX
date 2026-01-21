#include "Resource/ResourceCache.h"

namespace RVX::Resource
{

ResourceCache::ResourceCache(const CacheConfig& config)
    : m_config(config)
{
}

ResourceCache::~ResourceCache()
{
    Clear();
}

void ResourceCache::Store(IResource* resource)
{
    if (!resource) return;

    std::lock_guard<std::mutex> lock(m_mutex);

    ResourceId id = resource->GetId();

    // Check if already in cache
    auto it = m_resources.find(id);
    if (it != m_resources.end())
    {
        // Already cached, just update LRU
        TouchLRU(id);
        return;
    }

    // Add to cache
    resource->AddRef();
    m_resources[id] = resource;

    // Add to LRU list
    m_lruList.push_front(id);
    m_lruMap[id] = m_lruList.begin();

    // Check memory limit
    if (m_config.maxMemoryBytes > 0)
    {
        size_t currentUsage = 0;
        for (const auto& [rid, res] : m_resources)
        {
            currentUsage += res->GetTotalMemoryUsage();
        }

        if (currentUsage > m_config.maxMemoryBytes)
        {
            // Evict LRU items until under limit
            while (currentUsage > m_config.maxMemoryBytes && !m_lruList.empty())
            {
                ResourceId victimId = m_lruList.back();
                auto victimIt = m_resources.find(victimId);
                if (victimIt != m_resources.end())
                {
                    currentUsage -= victimIt->second->GetTotalMemoryUsage();
                    if (victimIt->second->Release())
                    {
                        delete victimIt->second;
                    }
                    m_resources.erase(victimIt);
                }
                RemoveLRU(victimId);
            }
        }
    }
}

IResource* ResourceCache::Get(ResourceId id)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    auto it = m_resources.find(id);
    if (it != m_resources.end())
    {
        m_hitCount++;
        TouchLRU(id);
        return it->second;
    }

    m_missCount++;
    return nullptr;
}

bool ResourceCache::Contains(ResourceId id) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_resources.find(id) != m_resources.end();
}

void ResourceCache::Remove(ResourceId id)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    auto it = m_resources.find(id);
    if (it != m_resources.end())
    {
        if (it->second->Release())
        {
            delete it->second;
        }
        m_resources.erase(it);
        RemoveLRU(id);
    }
}

void ResourceCache::Clear()
{
    std::lock_guard<std::mutex> lock(m_mutex);

    for (auto& [id, resource] : m_resources)
    {
        if (resource->Release())
        {
            delete resource;
        }
    }

    m_resources.clear();
    m_lruList.clear();
    m_lruMap.clear();
}

size_t ResourceCache::GetMemoryUsage() const
{
    std::lock_guard<std::mutex> lock(m_mutex);

    size_t total = 0;
    for (const auto& [id, resource] : m_resources)
    {
        total += resource->GetMemoryUsage();
    }
    return total;
}

size_t ResourceCache::GetGPUMemoryUsage() const
{
    std::lock_guard<std::mutex> lock(m_mutex);

    size_t total = 0;
    for (const auto& [id, resource] : m_resources)
    {
        total += resource->GetGPUMemoryUsage();
    }
    return total;
}

void ResourceCache::SetMemoryLimit(size_t bytes)
{
    m_config.maxMemoryBytes = bytes;
}

void ResourceCache::Evict(size_t targetBytes)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    size_t currentUsage = GetMemoryUsage() + GetGPUMemoryUsage();

    while (currentUsage > targetBytes && !m_lruList.empty())
    {
        ResourceId victimId = m_lruList.back();
        auto victimIt = m_resources.find(victimId);
        if (victimIt != m_resources.end())
        {
            currentUsage -= victimIt->second->GetTotalMemoryUsage();
            if (victimIt->second->Release())
            {
                delete victimIt->second;
            }
            m_resources.erase(victimIt);
        }
        RemoveLRU(victimId);
    }
}

void ResourceCache::EvictUnused()
{
    std::lock_guard<std::mutex> lock(m_mutex);

    std::vector<ResourceId> toRemove;

    for (const auto& [id, resource] : m_resources)
    {
        // If ref count is 1, only the cache holds a reference
        if (resource->GetRefCount() == 1)
        {
            toRemove.push_back(id);
        }
    }

    for (ResourceId id : toRemove)
    {
        auto it = m_resources.find(id);
        if (it != m_resources.end())
        {
            if (it->second->Release())
            {
                delete it->second;
            }
            m_resources.erase(it);
            RemoveLRU(id);
        }
    }
}

ResourceCache::Stats ResourceCache::GetStats() const
{
    std::lock_guard<std::mutex> lock(m_mutex);

    Stats stats;
    stats.totalResources = m_resources.size();
    
    for (const auto& [id, resource] : m_resources)
    {
        stats.memoryUsage += resource->GetMemoryUsage();
        stats.gpuMemoryUsage += resource->GetGPUMemoryUsage();
    }

    stats.hitCount = m_hitCount;
    stats.missCount = m_missCount;

    return stats;
}

void ResourceCache::ResetStats()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_hitCount = 0;
    m_missCount = 0;
}

void ResourceCache::TouchLRU(ResourceId id)
{
    auto it = m_lruMap.find(id);
    if (it != m_lruMap.end())
    {
        m_lruList.erase(it->second);
        m_lruList.push_front(id);
        it->second = m_lruList.begin();
    }
}

void ResourceCache::RemoveLRU(ResourceId id)
{
    auto it = m_lruMap.find(id);
    if (it != m_lruMap.end())
    {
        m_lruList.erase(it->second);
        m_lruMap.erase(it);
    }
}

} // namespace RVX::Resource
