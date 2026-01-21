#pragma once

/**
 * @file ResourceManager.h
 * @brief Main facade for the resource management system
 */

#include "Resource/IResource.h"
#include "Resource/ResourceHandle.h"
#include "Resource/ResourceRegistry.h"
#include "Resource/ResourceCache.h"
#include "Resource/DependencyGraph.h"
#include <memory>
#include <future>
#include <functional>

namespace RVX::Resource
{
    // Forward declarations
    class IResourceLoader;

    /**
     * @brief Configuration for ResourceManager
     */
    struct ResourceManagerConfig
    {
        /// Cache configuration
        CacheConfig cacheConfig;

        /// Number of async loading threads
        int asyncThreadCount = 2;

        /// Base path for resources
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

        /// Load a resource by path
        template<typename T>
        ResourceHandle<T> Load(const std::string& path);

        /// Load a resource by ID
        template<typename T>
        ResourceHandle<T> Load(ResourceId id);

        /// Generic load (returns base IResource)
        IResource* LoadResource(const std::string& path);
        IResource* LoadResource(ResourceId id);

        // =====================================================================
        // Asynchronous Loading
        // =====================================================================

        /// Load a resource asynchronously
        template<typename T>
        std::future<ResourceHandle<T>> LoadAsync(const std::string& path);

        /// Load with callback
        template<typename T>
        void LoadAsync(const std::string& path, std::function<void(ResourceHandle<T>)> callback);

        // =====================================================================
        // Batch Loading
        // =====================================================================

        /// Load multiple resources
        void LoadBatch(const std::vector<std::string>& paths,
                      std::function<void(float progress)> onProgress = nullptr,
                      std::function<void()> onComplete = nullptr);

        // =====================================================================
        // Query
        // =====================================================================

        /// Check if resource is loaded by path
        bool IsLoaded(const std::string& path) const;

        /// Check if resource is loaded by ID
        bool IsLoaded(ResourceId id) const;

        // =====================================================================
        // Unloading
        // =====================================================================

        /// Unload a resource by path
        void Unload(const std::string& path);

        /// Unload a resource by ID
        void Unload(ResourceId id);

        /// Unload unused resources (reference count == 1)
        void UnloadUnused();

        /// Clear all resources
        void Clear();

        // =====================================================================
        // Hot Reload
        // =====================================================================

        /// Enable/disable hot reload
        void EnableHotReload(bool enable);

        /// Check for file changes and reload modified resources
        void CheckForChanges();

        /// Register callback for resource reloads
        void OnResourceReloaded(std::function<void(ResourceId, IResource*)> callback);

        // =====================================================================
        // Cache Control
        // =====================================================================

        /// Set memory limit for cache
        void SetCacheLimit(size_t bytes);

        /// Clear the cache
        void ClearCache();

        /// Get cache
        ResourceCache& GetCache() { return *m_cache; }
        const ResourceCache& GetCache() const { return *m_cache; }

        // =====================================================================
        // Registry
        // =====================================================================

        ResourceRegistry* GetRegistry() { return m_registry.get(); }
        const ResourceRegistry* GetRegistry() const { return m_registry.get(); }

        // =====================================================================
        // Loader Registration
        // =====================================================================

        /// Register a loader for a specific resource type
        void RegisterLoader(ResourceType type, std::unique_ptr<IResourceLoader> loader);

        /// Get loader for a resource type
        IResourceLoader* GetLoader(ResourceType type);

        // =====================================================================
        // Async Processing
        // =====================================================================

        /// Process completed async loads (call once per frame)
        void ProcessCompletedLoads();

        // =====================================================================
        // Statistics
        // =====================================================================

        struct Stats
        {
            size_t totalResources = 0;
            size_t loadedCount = 0;
            size_t pendingLoads = 0;
            size_t cpuMemory = 0;
            size_t gpuMemory = 0;
        };

        Stats GetStats() const;

        // =====================================================================
        // Utility
        // =====================================================================

        /// Get resource type from file extension
        static ResourceType GetTypeFromExtension(const std::string& extension);

        /// Resolve path relative to base path
        std::string ResolvePath(const std::string& path) const;

    private:
        bool m_initialized = false;
        ResourceManagerConfig m_config;

        std::unique_ptr<ResourceRegistry> m_registry;
        std::unique_ptr<ResourceCache> m_cache;
        std::unique_ptr<DependencyGraph> m_dependencyGraph;

        std::unordered_map<ResourceType, std::unique_ptr<IResourceLoader>> m_loaders;

        std::function<void(ResourceId, IResource*)> m_reloadCallback;

        // Internal loading
        IResource* LoadInternal(const std::string& path, ResourceType type);
        void LoadDependencies(IResource* resource);

        // Register default loaders (ModelLoader, TextureLoader)
        void RegisterDefaultLoaders();
    };

    /**
     * @brief Interface for resource loaders
     */
    class IResourceLoader
    {
    public:
        virtual ~IResourceLoader() = default;

        /// Get the resource type this loader handles
        virtual ResourceType GetResourceType() const = 0;

        /// Get supported file extensions
        virtual std::vector<std::string> GetSupportedExtensions() const = 0;

        /// Load a resource from file
        virtual IResource* Load(const std::string& path) = 0;

        /// Check if this loader can handle the file
        virtual bool CanLoad(const std::string& path) const;
    };

    // =========================================================================
    // Template Implementations
    // =========================================================================

    template<typename T>
    ResourceHandle<T> ResourceManager::Load(const std::string& path)
    {
        IResource* resource = LoadResource(path);
        return ResourceHandle<T>(static_cast<T*>(resource));
    }

    template<typename T>
    ResourceHandle<T> ResourceManager::Load(ResourceId id)
    {
        IResource* resource = LoadResource(id);
        return ResourceHandle<T>(static_cast<T*>(resource));
    }

    template<typename T>
    std::future<ResourceHandle<T>> ResourceManager::LoadAsync(const std::string& path)
    {
        return std::async(std::launch::async, [this, path]() {
            return Load<T>(path);
        });
    }

    template<typename T>
    void ResourceManager::LoadAsync(const std::string& path, std::function<void(ResourceHandle<T>)> callback)
    {
        std::thread([this, path, callback]() {
            auto handle = Load<T>(path);
            callback(handle);
        }).detach();
    }

} // namespace RVX::Resource

namespace RVX
{
    using Resource::ResourceManager;
    using Resource::ResourceManagerConfig;
    using Resource::IResourceLoader;
}
