#pragma once

/**
 * @file ResourceCache.h
 * @brief In-memory cache for loaded resources
 */

#include "Resource/IResource.h"
#include "Resource/ResourceHandle.h"
#include <functional>
#include <list>
#include <mutex>
#include <shared_mutex>
#include <typeinfo>
#include <unordered_map>
#include <utility>
#include <vector>

namespace RVX::Resource
{
    class ResourceManager;

    /**
     * @brief Retained immutable cache snapshot supplied to batch pre-commit observers.
     *
     * The cache builds this while it owns its resource mutex, after all
     * removal guards passed and before a batch can release any resource.
     */
    using ResourceCacheBatchSnapshot =
        std::vector<std::pair<ResourceId, ResourceHandle<IResource>>>;
    using ResourceCacheBatchPreCommit =
        std::function<bool(const ResourceCacheBatchSnapshot&)>;

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

        /**
         * @brief Atomically retain a dependency-first resource batch.
         *
         * Either every previously absent resource is inserted, or none of the
         * batch insertions remain. Memory-budget eviction runs only after the
         * batch has committed and therefore cannot corrupt publication rollback.
         */
        bool StoreBatch(const std::vector<IResource*>& resources);

        /**
         * @brief Atomically insert a prepared batch and make it Loaded before
         *        any cache reader can observe it.
         *
         * User callbacks are deliberately not invoked while the cache mutex is
         * held; ResourceManager dispatches them after the transaction commits.
         */
        bool StorePreparedBatch(const std::vector<IResource*>& resources);

        /// Retrieve a resource from the cache
        IResource* Get(ResourceId id);

        /**
         * @brief Retain an exactly typed already-loaded cache entry.
         *
         * This path never invokes a loader or mutates registry/lifecycle
         * state. The returned handle acquires its intrusive reference while
         * the cache mutex is held, so a concurrent removal cannot invalidate
         * the entry between lookup and retention.
         */
        template<typename T>
        [[nodiscard]] ResourceHandle<T> TryAcquireLoaded(ResourceId id)
        {
            static_assert(std::is_base_of_v<IResource, T>,
                          "T must derive from IResource");

            std::lock_guard<std::mutex> lock(m_mutex);
            const auto found = m_resources.find(id);
            if (found == m_resources.end())
            {
                ++m_missCount;
                return {};
            }

            ++m_hitCount;
            IResource* const resource = found->second;
            if (resource == nullptr || resource->GetId() != id ||
                resource->GetState() != ResourceState::Loaded ||
                typeid(*resource) != typeid(T))
            {
                return {};
            }

            TouchLRU(id);
            return ResourceHandle<T>(static_cast<T*>(resource));
        }

        /// Check if resource is in cache
        bool Contains(ResourceId id) const;

        /// Check that a cache entry exists and has committed Loaded visibility.
        bool ContainsLoaded(ResourceId id) const;

        /// Remove a resource from the cache unless an ownership guard blocks it.
        [[nodiscard]] bool Remove(ResourceId id);

        /**
         * @brief Remove an ordered ownership closure as one cache transaction.
         *
         * Holds the removal barrier for the complete preflight and mutation,
         * so an AssetResidencyLease cannot protect a later member after an
         * earlier member has already been released. Missing entries are a
         * permitted no-op: an eviction may have released their cache record
         * before this transaction acquired the barrier, while the caller
         * still needs to retire corresponding owner metadata.
         */
        /**
         * @param preCommit Optional no-throw logical observer commit. When
         *        supplied it replaces per-resource BeforeRemove callbacks for
         *        this batch and runs only after cache preflight, before the
         *        first Release(). Returning false (or throwing) aborts with
         *        no cache mutation.
         */
        [[nodiscard]] bool RemoveBatch(
            const std::vector<ResourceId>& resourceIds,
            const ResourceCacheBatchPreCommit& preCommit = {});

        /**
         * @brief Release every cache entry only if every entry can be released.
         *
         * Unlike Clear(), this is an all-or-nothing ownership transaction.
         * ResourceManager uses it before retiring the corresponding registry
         * and dependency-graph state, so a residency acquisition or external
         * ownership guard cannot leave those owner databases half-cleared.
         */
        [[nodiscard]] bool ClearAllOrNothing(
            const ResourceCacheBatchPreCommit& preCommit = {});

        /** @brief Clear only entries not protected by external CPU ownership. */
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

        /** @brief Observe every cache-owned resource before its retain is released. */
        void SetBeforeRemoveCallback(std::function<void(IResource*)> callback);

        /** @brief Reject removal for resources protected by external CPU ownership. */
        void SetCanRemoveCallback(std::function<bool(ResourceId)> callback);

    private:
        friend class ResourceManager;

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
        std::function<void(IResource*)> m_beforeRemoveCallback;
        std::function<bool(ResourceId)> m_canRemoveCallback;
        mutable std::shared_mutex m_removalBarrier;

        void TouchLRU(ResourceId id);
        void RemoveLRU(ResourceId id);
        void NotifyBeforeRemove(IResource* resource) noexcept;
        bool CanRemove(ResourceId id) noexcept;
        bool StoreBatchInternal(const std::vector<IResource*>& resources,
                                bool commitLoadedVisibility);
        void EvictToMemoryLimitLocked() noexcept;
        void ClearInternal(bool ignoreRemovalProtection);
        [[nodiscard]] std::unique_lock<std::shared_mutex>
            LockRemovalsForResidency();
        [[nodiscard]] bool ContainsLoadedUnderResidencyBarrier(
            ResourceId id) const;
        [[nodiscard]] ResourceCacheBatchSnapshot BuildBatchSnapshot(
            const std::vector<ResourceId>* orderedResourceIds) const;
    };

} // namespace RVX::Resource
