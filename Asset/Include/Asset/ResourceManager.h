#pragma once

/**
 * @file ResourceManager.h
 * @brief Main facade for the asset/resource system
 */

#include "Asset/Asset.h"
#include "Asset/AssetHandle.h"
#include "Asset/AssetRegistry.h"
#include "Asset/AssetCache.h"
#include "Asset/DependencyGraph.h"
#include <memory>
#include <future>
#include <functional>

namespace RVX::Asset
{
    // Forward declarations
    class IAssetLoader;

    /**
     * @brief Configuration for ResourceManager
     */
    struct ResourceManagerConfig
    {
        /// Cache configuration
        CacheConfig cacheConfig;

        /// Number of async loading threads
        int asyncThreadCount = 2;

        /// Base path for assets
        std::string basePath = "";

        /// Enable hot reload
        bool enableHotReload = false;
    };

    /**
     * @brief Central resource management facade
     * 
     * Provides:
     * - Synchronous and asynchronous loading
     * - Automatic dependency resolution
     * - Caching and memory management
     * - Hot reload support
     */
    class ResourceManager
    {
    public:
        ResourceManager();
        ~ResourceManager();

        // Singleton access
        static ResourceManager& Get();

        // =====================================================================
        // Initialization
        // =====================================================================

        void Initialize(const ResourceManagerConfig& config = {});
        void Shutdown();
        bool IsInitialized() const { return m_initialized; }

        // =====================================================================
        // Synchronous Loading
        // =====================================================================

        /// Load an asset by path
        template<typename T>
        AssetHandle<T> Load(const std::string& path);

        /// Load an asset by ID
        template<typename T>
        AssetHandle<T> Load(AssetId id);

        /// Generic load (returns base Asset)
        Asset* LoadAsset(const std::string& path);
        Asset* LoadAsset(AssetId id);

        // =====================================================================
        // Asynchronous Loading
        // =====================================================================

        /// Load an asset asynchronously
        template<typename T>
        std::future<AssetHandle<T>> LoadAsync(const std::string& path);

        /// Load with callback
        template<typename T>
        void LoadAsync(const std::string& path, std::function<void(AssetHandle<T>)> callback);

        // =====================================================================
        // Batch Loading
        // =====================================================================

        /// Load multiple assets
        void LoadBatch(const std::vector<std::string>& paths,
                      std::function<void(float progress)> onProgress = nullptr,
                      std::function<void()> onComplete = nullptr);

        // =====================================================================
        // Unloading
        // =====================================================================

        /// Unload an asset by ID
        void Unload(AssetId id);

        /// Unload unused assets (reference count == 1)
        void UnloadUnused();

        // =====================================================================
        // Hot Reload
        // =====================================================================

        /// Enable/disable hot reload
        void EnableHotReload(bool enable);

        /// Check for file changes and reload modified assets
        void CheckForChanges();

        /// Register callback for asset reloads
        void OnAssetReloaded(std::function<void(AssetId, Asset*)> callback);

        // =====================================================================
        // Cache Control
        // =====================================================================

        /// Set memory limit for cache
        void SetCacheLimit(size_t bytes);

        /// Clear the cache
        void ClearCache();

        /// Get cache
        AssetCache* GetCache() { return m_cache.get(); }

        // =====================================================================
        // Registry
        // =====================================================================

        AssetRegistry* GetRegistry() { return m_registry.get(); }
        const AssetRegistry* GetRegistry() const { return m_registry.get(); }

        // =====================================================================
        // Loader Registration
        // =====================================================================

        /// Register a loader for a specific asset type
        void RegisterLoader(AssetType type, std::unique_ptr<IAssetLoader> loader);

        /// Get loader for an asset type
        IAssetLoader* GetLoader(AssetType type);

        // =====================================================================
        // Statistics
        // =====================================================================

        struct Stats
        {
            size_t totalAssets = 0;
            size_t loadedAssets = 0;
            size_t pendingLoads = 0;
            size_t memoryUsage = 0;
            size_t gpuMemoryUsage = 0;
        };

        Stats GetStats() const;

        // =====================================================================
        // Utility
        // =====================================================================

        /// Get asset type from file extension
        static AssetType GetTypeFromExtension(const std::string& extension);

        /// Resolve path relative to base path
        std::string ResolvePath(const std::string& path) const;

    private:
        bool m_initialized = false;
        ResourceManagerConfig m_config;

        std::unique_ptr<AssetRegistry> m_registry;
        std::unique_ptr<AssetCache> m_cache;
        std::unique_ptr<DependencyGraph> m_dependencyGraph;

        std::unordered_map<AssetType, std::unique_ptr<IAssetLoader>> m_loaders;

        std::function<void(AssetId, Asset*)> m_reloadCallback;

        // Internal loading
        Asset* LoadInternal(const std::string& path, AssetType type);
        void LoadDependencies(Asset* asset);
    };

    /**
     * @brief Interface for asset loaders
     */
    class IAssetLoader
    {
    public:
        virtual ~IAssetLoader() = default;

        /// Get the asset type this loader handles
        virtual AssetType GetAssetType() const = 0;

        /// Get supported file extensions
        virtual std::vector<std::string> GetSupportedExtensions() const = 0;

        /// Load an asset from file
        virtual Asset* Load(const std::string& path) = 0;

        /// Check if this loader can handle the file
        virtual bool CanLoad(const std::string& path) const;
    };

    // =========================================================================
    // Template Implementations
    // =========================================================================

    template<typename T>
    AssetHandle<T> ResourceManager::Load(const std::string& path)
    {
        Asset* asset = LoadAsset(path);
        return AssetHandle<T>(static_cast<T*>(asset));
    }

    template<typename T>
    AssetHandle<T> ResourceManager::Load(AssetId id)
    {
        Asset* asset = LoadAsset(id);
        return AssetHandle<T>(static_cast<T*>(asset));
    }

    template<typename T>
    std::future<AssetHandle<T>> ResourceManager::LoadAsync(const std::string& path)
    {
        return std::async(std::launch::async, [this, path]() {
            return Load<T>(path);
        });
    }

    template<typename T>
    void ResourceManager::LoadAsync(const std::string& path, std::function<void(AssetHandle<T>)> callback)
    {
        // TODO: Use proper async loader thread pool
        std::thread([this, path, callback]() {
            auto handle = Load<T>(path);
            callback(handle);
        }).detach();
    }

} // namespace RVX::Asset
