#pragma once

/**
 * @file AssetCache.h
 * @brief In-memory cache for loaded assets
 */

#include "Asset/Asset.h"
#include "Asset/AssetHandle.h"
#include <unordered_map>
#include <list>
#include <mutex>

namespace RVX::Asset
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

        /// Minimum time to keep assets (seconds)
        float minRetentionTime = 5.0f;
    };

    /**
     * @brief In-memory cache for loaded assets
     * 
     * Provides:
     * - Fast lookup by AssetId
     * - LRU eviction policy
     * - Memory limit enforcement
     */
    class AssetCache
    {
    public:
        explicit AssetCache(const CacheConfig& config = {});
        ~AssetCache();

        // =====================================================================
        // Cache Operations
        // =====================================================================

        /// Store an asset in the cache
        void Store(Asset* asset);

        /// Retrieve an asset from the cache
        Asset* Get(AssetId id);

        /// Check if asset is in cache
        bool Contains(AssetId id) const;

        /// Remove an asset from the cache
        void Remove(AssetId id);

        /// Clear all cached assets
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

        /// Evict assets to meet memory limit
        void Evict(size_t targetBytes);

        /// Evict unused assets (ref count == 1, only cache holds reference)
        void EvictUnused();

        // =====================================================================
        // Statistics
        // =====================================================================

        struct Stats
        {
            size_t totalAssets = 0;
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

        // Asset storage
        std::unordered_map<AssetId, Asset*> m_assets;

        // LRU tracking
        std::list<AssetId> m_lruList;
        std::unordered_map<AssetId, std::list<AssetId>::iterator> m_lruMap;

        // Statistics
        mutable size_t m_hitCount = 0;
        mutable size_t m_missCount = 0;

        void TouchLRU(AssetId id);
        void RemoveLRU(AssetId id);
    };

} // namespace RVX::Asset
