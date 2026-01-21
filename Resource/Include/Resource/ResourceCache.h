#pragma once

/**
 * @file ResourceCache.h
 * @brief In-memory cache for loaded resources
 */

#include "Resource/IResource.h"
#include "Resource/ResourceHandle.h"
#include <unordered_map>
#include <list>
#include <mutex>

namespace RVX::Resource
{
    /**
     * @brief Cache configuration
     */
    struct CacheConfig
    {
        /// Maximum memory limit (0 = unlimited)
        size_t maxMemoryBytes = 0;

        /// Whether to use LRU eviction
        bool useLRU = true;

        /// Minimum time to keep resources (seconds)
        float minRetentionTime = 5.0f;
    };

    /**
     * @brief In-memory cache for loaded resources
     * 
     * Provides:
     * - Fast lookup by ResourceId
     * - LRU eviction policy
     * - Memory limit enforcement
     */
    class ResourceCache
    {
    public:
        explicit ResourceCache(const CacheConfig& config = {});
        ~ResourceCache();

        // =====================================================================
        // Cache Operations
        // =====================================================================

        /// Store a resource in the cache
        void Store(IResource* resource);

        /// Retrieve a resource from the cache
        IResource* Get(ResourceId id);

        /// Check if resource is in cache
        bool Contains(ResourceId id) const;

        /// Remove a resource from the cache
        void Remove(ResourceId id);

        /// Clear all cached resources
        void Clear();

        // =====================================================================
        // Memory Management
        // =====================================================================

        /// Get current memory usage
        size_t GetMemoryUsage() const;

        /// Get current GPU memory usage
        size_t GetGPUMemoryUsage() const;

        /// Set memory limit
        void SetMemoryLimit(size_t bytes);

        /// Evict resources to meet memory limit
        void Evict(size_t targetBytes);

        /// Evict unused resources (ref count == 1, only cache holds reference)
        void EvictUnused();

        // =====================================================================
        // Statistics
        // =====================================================================

        struct Stats
        {
            size_t totalResources = 0;
            size_t memoryUsage = 0;
            size_t gpuMemoryUsage = 0;
            size_t hitCount = 0;
            size_t missCount = 0;
        };

        Stats GetStats() const;
        void ResetStats();

    private:
        CacheConfig m_config;
        mutable std::mutex m_mutex;

        // Resource storage
        std::unordered_map<ResourceId, IResource*> m_resources;

        // LRU tracking
        std::list<ResourceId> m_lruList;
        std::unordered_map<ResourceId, std::list<ResourceId>::iterator> m_lruMap;

        // Statistics
        mutable size_t m_hitCount = 0;
        mutable size_t m_missCount = 0;

        void TouchLRU(ResourceId id);
        void RemoveLRU(ResourceId id);
    };

} // namespace RVX::Resource
