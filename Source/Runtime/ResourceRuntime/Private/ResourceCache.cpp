#include "Resource/ResourceCache.h"

#include <algorithm>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace RVX::Resource
{

ResourceCache::ResourceCache(const CacheConfig& config)
    : m_config(config)
{
}

ResourceCache::~ResourceCache()
{
    ClearInternal(true);
}

void ResourceCache::Store(IResource* resource)
{
    if (!resource)
        return;

    (void)StoreBatch({resource});
}

bool ResourceCache::StoreBatch(const std::vector<IResource*>& resources)
{
    return StoreBatchInternal(resources, false);
}

bool ResourceCache::StorePreparedBatch(const std::vector<IResource*>& resources)
{
    return StoreBatchInternal(resources, true);
}

bool ResourceCache::StoreBatchInternal(const std::vector<IResource*>& resources,
                                       bool commitLoadedVisibility)
{
    if (resources.empty())
        return true;

    std::shared_lock<std::shared_mutex> removalLock(m_removalBarrier);
    std::lock_guard<std::mutex> lock(m_mutex);

    std::unordered_set<ResourceId> batchIds;
    try
    {
        batchIds.reserve(resources.size());
        for (IResource* resource : resources)
        {
            if (resource == nullptr || resource->GetId() == InvalidResourceId ||
                !batchIds.insert(resource->GetId()).second)
            {
                return false;
            }
            if (commitLoadedVisibility && m_resources.contains(resource->GetId()))
            {
                // ResourceManager canonicalizes existing dependencies before
                // forming this batch. Seeing an entry now means another writer
                // raced the transaction; fail closed instead of publishing two
                // objects with one identity.
                return false;
            }
        }

        m_resources.reserve(m_resources.size() + resources.size());
        m_lruMap.reserve(m_lruMap.size() + resources.size());
    }
    catch (...)
    {
        return false;
    }

    struct InsertedResource
    {
        ResourceId id = InvalidResourceId;
        IResource* resource = nullptr;
    };
    std::vector<InsertedResource> inserted;
    try
    {
        inserted.reserve(resources.size());
        for (IResource* resource : resources)
        {
            const ResourceId id = resource->GetId();
            if (m_resources.contains(id))
            {
                TouchLRU(id);
                continue;
            }

            resource->AddRef();
            bool resourceInserted = false;
            bool lruListInserted = false;
            try
            {
                const auto [resourceIt, didInsert] = m_resources.emplace(id, resource);
                (void)resourceIt;
                if (!didInsert)
                {
                    resource->Release();
                    TouchLRU(id);
                    continue;
                }
                resourceInserted = true;
                m_lruList.push_front(id);
                lruListInserted = true;
                const auto [lruIt, didInsertLRU] =
                    m_lruMap.emplace(id, m_lruList.begin());
                (void)lruIt;
                if (!didInsertLRU)
                {
                    throw std::runtime_error("Resource cache LRU identity collision.");
                }
            }
            catch (...)
            {
                if (lruListInserted)
                {
                    m_lruList.pop_front();
                }
                if (resourceInserted)
                {
                    m_resources.erase(id);
                }
                resource->Release();
                throw;
            }
            inserted.push_back({id, resource});
        }
    }
    catch (...)
    {
        for (auto it = inserted.rbegin(); it != inserted.rend(); ++it)
        {
            RemoveLRU(it->id);
            m_resources.erase(it->id);
            it->resource->Release();
        }
        return false;
    }

    // Cache readers take the same mutex. Committing the state here closes the
    // window in which a prepared object was addressable but still Unloaded.
    if (commitLoadedVisibility)
    {
        for (const InsertedResource& entry : inserted)
        {
            entry.resource->CommitLoadedState();
        }
    }

    EvictToMemoryLimitLocked();
    return true;
}

void ResourceCache::EvictToMemoryLimitLocked() noexcept
{
    if (m_config.maxMemoryBytes == 0)
        return;

    try
    {
        size_t currentUsage = 0;
        for (const auto& [rid, res] : m_resources)
        {
            currentUsage += res->GetTotalMemoryUsage();
        }

        if (currentUsage > m_config.maxMemoryBytes)
        {
            // Only cache-exclusive objects are legal eviction candidates.
            // Prepared batches remain pinned by their transaction/operation,
            // so they cannot disappear before Ready is published.
            std::vector<ResourceId> victims;
            victims.reserve(m_lruList.size());
            for (auto it = m_lruList.rbegin();
                 it != m_lruList.rend() && currentUsage > m_config.maxMemoryBytes;
                 ++it)
            {
                const ResourceId victimId = *it;
                auto victimIt = m_resources.find(victimId);
                if (victimIt != m_resources.end() &&
                    victimIt->second->GetRefCount() == 1 &&
                    CanRemove(victimId))
                {
                    currentUsage -= victimIt->second->GetTotalMemoryUsage();
                    victims.push_back(victimId);
                }
            }

            for (ResourceId victimId : victims)
            {
                auto victimIt = m_resources.find(victimId);
                if (victimIt == m_resources.end() ||
                    victimIt->second->GetRefCount() != 1 ||
                    !CanRemove(victimId))
                {
                    continue;
                }
                NotifyBeforeRemove(victimIt->second);
                if (victimIt->second->Release())
                {
                    delete victimIt->second;
                }
                m_resources.erase(victimIt);
                RemoveLRU(victimId);
            }
        }
    }
    catch (...)
    {
        // Cache retention policy is advisory. A resource batch that has already
        // committed remains valid even if a custom resource cannot report its
        // memory footprint during this pass.
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

bool ResourceCache::ContainsLoaded(ResourceId id) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto it = m_resources.find(id);
    return it != m_resources.end() && it->second != nullptr && it->second->IsLoaded();
}

bool ResourceCache::Remove(ResourceId id)
{
    std::shared_lock<std::shared_mutex> removalLock(m_removalBarrier);
    std::lock_guard<std::mutex> lock(m_mutex);

    auto it = m_resources.find(id);
    if (it == m_resources.end())
    {
        return false;
    }
    if (!CanRemove(id))
    {
        return false;
    }

    NotifyBeforeRemove(it->second);
    if (it->second->Release())
    {
        delete it->second;
    }
    m_resources.erase(it);
    RemoveLRU(id);
    return true;
}

bool ResourceCache::RemoveBatch(const std::vector<ResourceId>& resourceIds,
                                const ResourceCacheBatchPreCommit& preCommit)
{
    if (resourceIds.empty())
    {
        return true;
    }

    // This is deliberately a unique lock, unlike Remove(). A residency
    // acquire holds this barrier exclusively while it snapshots its closure;
    // holding it across the whole batch makes closure release and acquisition
    // serializable rather than merely individually safe.
    std::unique_lock<std::shared_mutex> removalLock(m_removalBarrier);
    std::lock_guard<std::mutex> lock(m_mutex);

    // Every fallible/guarded check completes before the first Release(). Do
    // not allocate here: a duplicate check is intentionally O(n^2), because
    // closure size is normally small and allocation failure after a partial
    // resource release would violate this API's transaction contract.
    for (size_t index = 0; index < resourceIds.size(); ++index)
    {
        const ResourceId id = resourceIds[index];
        if (id == InvalidResourceId)
        {
            return false;
        }
        for (size_t previous = 0; previous < index; ++previous)
        {
            if (resourceIds[previous] == id)
            {
                return false;
            }
        }

        const auto found = m_resources.find(id);
        if (found != m_resources.end() && !CanRemove(id))
        {
            return false;
        }
    }

    // Manager-side lifecycle events have to be fully value-owned and staged
    // before a cache reference can be released. The callback is intentionally
    // a pre-commit gate: after it succeeds, all following batch mutation is
    // no-throw. Direct ResourceCache callers retain the traditional observer
    // behaviour below.
    if (preCommit)
    {
        try
        {
            const ResourceCacheBatchSnapshot snapshot =
                BuildBatchSnapshot(&resourceIds);
            if (!preCommit(snapshot))
            {
                return false;
            }
        }
        catch (...)
        {
            return false;
        }
    }

    for (ResourceId id : resourceIds)
    {
        const auto found = m_resources.find(id);
        if (found == m_resources.end())
        {
            continue;
        }

        if (!preCommit)
        {
            NotifyBeforeRemove(found->second);
        }
        if (found->second->Release())
        {
            delete found->second;
        }
        m_resources.erase(found);
        RemoveLRU(id);
    }
    return true;
}

bool ResourceCache::ClearAllOrNothing(
    const ResourceCacheBatchPreCommit& preCommit)
{
    // Keep the exclusive barrier from preflight through the final release.
    // A residency lease acquires this same barrier before it snapshots its
    // closure, making Clear and lease admission serializable.
    std::unique_lock<std::shared_mutex> removalLock(m_removalBarrier);
    std::lock_guard<std::mutex> lock(m_mutex);

    // Complete all fallible ownership checks before releasing a single cache
    // reference. This deliberately does not allocate after preflight.
    for (const auto& [resourceId, resource] : m_resources)
    {
        (void)resource;
        if (!CanRemove(resourceId))
        {
            return false;
        }
    }

    if (preCommit)
    {
        try
        {
            const ResourceCacheBatchSnapshot snapshot = BuildBatchSnapshot(nullptr);
            if (!preCommit(snapshot))
            {
                return false;
            }
        }
        catch (...)
        {
            return false;
        }
    }

    for (auto resource = m_resources.begin(); resource != m_resources.end();)
    {
        if (!preCommit)
        {
            NotifyBeforeRemove(resource->second);
        }
        if (resource->second->Release())
        {
            delete resource->second;
        }
        resource = m_resources.erase(resource);
    }
    m_lruList.clear();
    m_lruMap.clear();
    return true;
}

void ResourceCache::Clear()
{
    std::shared_lock<std::shared_mutex> removalLock(m_removalBarrier);
    ClearInternal(false);
}

void ResourceCache::ClearInternal(bool ignoreRemovalProtection)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    auto resource = m_resources.begin();
    while (resource != m_resources.end())
    {
        if (!ignoreRemovalProtection && !CanRemove(resource->first))
        {
            ++resource;
            continue;
        }

        NotifyBeforeRemove(resource->second);
        if (resource->second->Release())
        {
            delete resource->second;
        }
        resource = m_resources.erase(resource);
    }

    m_lruList.clear();
    m_lruMap.clear();
    for (const auto& [id, retained] : m_resources)
    {
        (void)retained;
        m_lruList.push_front(id);
        m_lruMap.emplace(id, m_lruList.begin());
    }
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
    std::shared_lock<std::shared_mutex> removalLock(m_removalBarrier);
    std::lock_guard<std::mutex> lock(m_mutex);
    m_config.maxMemoryBytes = bytes;
    EvictToMemoryLimitLocked();
}

void ResourceCache::Evict(size_t targetBytes)
{
    std::shared_lock<std::shared_mutex> removalLock(m_removalBarrier);
    std::lock_guard<std::mutex> lock(m_mutex);

    size_t currentUsage = 0;
    for (const auto& [id, resource] : m_resources)
    {
        (void)id;
        currentUsage += resource->GetTotalMemoryUsage();
    }

    std::vector<ResourceId> candidates;
    candidates.reserve(m_lruList.size());
    for (auto it = m_lruList.rbegin(); it != m_lruList.rend(); ++it)
    {
        candidates.push_back(*it);
    }

    for (ResourceId victimId : candidates)
    {
        if (currentUsage <= targetBytes)
        {
            break;
        }
        auto victimIt = m_resources.find(victimId);
        if (victimIt != m_resources.end() && CanRemove(victimId))
        {
            currentUsage -= victimIt->second->GetTotalMemoryUsage();
            NotifyBeforeRemove(victimIt->second);
            if (victimIt->second->Release())
            {
                delete victimIt->second;
            }
            m_resources.erase(victimIt);
            RemoveLRU(victimId);
        }
    }
}

void ResourceCache::EvictUnused()
{
    std::shared_lock<std::shared_mutex> removalLock(m_removalBarrier);
    std::lock_guard<std::mutex> lock(m_mutex);

    std::vector<ResourceId> toRemove;

    for (const auto& [id, resource] : m_resources)
    {
        // If ref count is 1, only the cache holds a reference
        if (resource->GetRefCount() == 1 && CanRemove(id))
        {
            toRemove.push_back(id);
        }
    }

    for (ResourceId id : toRemove)
    {
        auto it = m_resources.find(id);
        if (it != m_resources.end())
        {
            NotifyBeforeRemove(it->second);
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

void ResourceCache::SetBeforeRemoveCallback(
    std::function<void(IResource*)> callback)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_beforeRemoveCallback = std::move(callback);
}

void ResourceCache::SetCanRemoveCallback(
    std::function<bool(ResourceId)> callback)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_canRemoveCallback = std::move(callback);
}

void ResourceCache::NotifyBeforeRemove(IResource* resource) noexcept
{
    if (m_beforeRemoveCallback)
    {
        try
        {
            m_beforeRemoveCallback(resource);
        }
        catch (...)
        {
            // Observer failures must never interrupt cache ownership release.
        }
    }
}

bool ResourceCache::CanRemove(ResourceId id) noexcept
{
    if (!m_canRemoveCallback)
    {
        return true;
    }

    try
    {
        return m_canRemoveCallback(id);
    }
    catch (...)
    {
        // The cache must fail closed: a broken ownership observer cannot make
        // a live resource disappear under a caller that requested protection.
        return false;
    }
}

std::unique_lock<std::shared_mutex>
ResourceCache::LockRemovalsForResidency()
{
    return std::unique_lock<std::shared_mutex>(m_removalBarrier);
}

bool ResourceCache::ContainsLoadedUnderResidencyBarrier(ResourceId id) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto it = m_resources.find(id);
    return it != m_resources.end() && it->second != nullptr &&
           it->second->IsLoaded();
}

ResourceCacheBatchSnapshot ResourceCache::BuildBatchSnapshot(
    const std::vector<ResourceId>* orderedResourceIds) const
{
    ResourceCacheBatchSnapshot resources;
    if (orderedResourceIds != nullptr)
    {
        resources.reserve(orderedResourceIds->size());
        for (ResourceId resourceId : *orderedResourceIds)
        {
            const auto found = m_resources.find(resourceId);
            if (found != m_resources.end() && found->second != nullptr)
            {
                resources.emplace_back(resourceId,
                                       ResourceHandle<IResource>(found->second));
            }
        }
        return resources;
    }

    resources.reserve(m_resources.size());
    for (const auto& [resourceId, resource] : m_resources)
    {
        if (resource != nullptr)
        {
            resources.emplace_back(resourceId, ResourceHandle<IResource>(resource));
        }
    }

    std::sort(resources.begin(),
              resources.end(),
              [](const auto& left, const auto& right)
              {
                  return left.first < right.first;
              });
    return resources;
}

void ResourceCache::TouchLRU(ResourceId id)
{
    auto it = m_lruMap.find(id);
    if (it != m_lruMap.end())
    {
        // Relinking an existing list node cannot allocate or throw. Erasing
        // before push_front could leave the LRU structures inconsistent if
        // the new-node allocation failed while a cache acquisition holds the
        // resource mutex.
        m_lruList.splice(m_lruList.begin(), m_lruList, it->second);
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
