#pragma once

/**
 * @file ResourceManager.h
 * @brief Main facade for the resource management system
 */

#include "Resource/DependencyGraph.h"
#include "Resource/IResource.h"
#include "Resource/ResourceCache.h"
#include "Resource/ResourceHandle.h"
#include "Resource/ResourceRegistry.h"
#include "Resource/RuntimeResourcePolicy.h"
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

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

        /// Runtime/source/cooked/package path policy
        ResourceRuntimePolicy runtimePolicy;

        /// Enable hot reload
        bool enableHotReload = false;
    };

    enum class ResourceHotReloadStatus : uint8
    {
        Disabled = 0,
        Active,
        UnsupportedRuntimePolicy,
        Uninitialized,
    };

    inline constexpr uint32 RVX_RESOURCE_HOT_RELOAD_DIAGNOSTIC_SCHEMA_VERSION = 1;
    inline constexpr const char* RVX_RESOURCE_HOT_RELOAD_DIAGNOSTIC_SCHEMA_ID =
        "RVX.Resource.HotReloadDiagnostic";

    const char* GetResourceHotReloadStatusName(ResourceHotReloadStatus status);

    struct ResourceHotReloadDiagnostic
    {
        bool requested = false;
        bool enabled = false;
        bool sourceAssetAccessRequired = true;
        bool watcherInitialized = false;
        ResourceHotReloadStatus status = ResourceHotReloadStatus::Disabled;
        std::string message;
        size_t watchedFileCount = 0;
        size_t registeredResourceCount = 0;
        size_t pendingReloadCount = 0;
        size_t totalReloadCount = 0;
        size_t successfulReloadCount = 0;
        size_t failedReloadCount = 0;
    };

    std::string ExportResourceHotReloadDiagnosticJson(const ResourceHotReloadDiagnostic& diagnostic);
    bool SaveResourceHotReloadDiagnosticJson(const char* filename,
                                             const ResourceHotReloadDiagnostic& diagnostic);

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

        /// Check whether hot reload is active under the current runtime policy.
        bool IsHotReloadEnabled() const;

        /// Get the current hot-reload policy/status diagnostic.
        ResourceHotReloadDiagnostic GetHotReloadDiagnostic() const;

        /// Export the current hot-reload diagnostic as a stable JSON artifact.
        std::string ExportHotReloadDiagnosticJson() const;

        /// Save the current hot-reload diagnostic as a stable JSON artifact.
        bool SaveHotReloadDiagnosticJson(const char* filename) const;

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

        /// Get the last synchronous or async load diagnostic emitted by the manager.
        ResourceLoadDiagnostic GetLastLoadDiagnostic() const;

        /// Export the last load diagnostic as a stable machine-readable JSON artifact.
        std::string ExportLastLoadDiagnosticJson() const;

        /// Save the last load diagnostic JSON artifact for tools/CI.
        bool SaveLastLoadDiagnosticJson(const char* filename) const;

        // =====================================================================
        // Utility
        // =====================================================================

        /// Get resource type from file extension
        static ResourceType GetTypeFromExtension(const std::string& extension);

        /// Resolve path relative to base path
        std::string ResolvePath(const std::string& path) const;

    private:
        struct PendingAsyncCallback
        {
            std::function<bool()> isReady;
            std::function<void()> dispatch;
        };

        bool m_initialized = false;
        ResourceManagerConfig m_config;

        std::unique_ptr<ResourceRegistry> m_registry;
        std::unique_ptr<ResourceCache> m_cache;
        std::unique_ptr<DependencyGraph> m_dependencyGraph;

        std::unordered_map<ResourceType, std::unique_ptr<IResourceLoader>> m_loaders;

        std::function<void(ResourceId, IResource*)> m_reloadCallback;

        mutable std::recursive_mutex m_loadMutex;

        std::vector<std::thread> m_asyncWorkers;
        std::deque<std::function<void()>> m_asyncJobs;
        mutable std::mutex m_asyncMutex;
        std::condition_variable m_asyncCondition;
        bool m_asyncStopping = false;

        std::vector<PendingAsyncCallback> m_pendingCallbacks;
        mutable std::mutex m_pendingCallbacksMutex;

        mutable std::mutex m_diagnosticMutex;
        ResourceLoadDiagnostic m_lastLoadDiagnostic;

        mutable std::mutex m_hotReloadMutex;
        ResourceHotReloadDiagnostic m_hotReloadDiagnostic;
        bool m_hotReloadInitializedByManager = false;
        std::unordered_map<ResourceId, uint32_t> m_hotReloadWatchIds;

        // Internal loading
        IResource* LoadInternal(const std::string& path,
                                const ResourcePathResolution& resolution,
                                ResourceType type);
        void LoadDependencies(IResource* resource);
        void SetLastLoadDiagnostic(const ResourceLoadDiagnostic& diagnostic);
        bool IsHotReloadSupportedByPolicy() const;
        void ConfigureHotReload(bool enable);
        void RegisterHotReloadResource(IResource* resource, const ResourcePathResolution& resolution);
        void SetHotReloadDiagnostic(ResourceHotReloadStatus status,
                                    bool requested,
                                    bool enabled,
                                    const std::string& message);

        // Register default loaders (ModelLoader, TextureLoader)
        void RegisterDefaultLoaders();

        void StartAsyncWorkers();
        void StopAsyncWorkers();
        void EnqueueAsyncJob(std::function<void()> job);
        void AsyncWorkerLoop();
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
        auto task = std::make_shared<std::packaged_task<ResourceHandle<T>()>>([this, path]() {
            return Load<T>(path);
        });

        auto future = task->get_future();
        EnqueueAsyncJob([task]() {
            (*task)();
        });

        return future;
    }

    template<typename T>
    void ResourceManager::LoadAsync(const std::string& path, std::function<void(ResourceHandle<T>)> callback)
    {
        auto task = std::make_shared<std::packaged_task<ResourceHandle<T>()>>([this, path]() {
            return Load<T>(path);
        });
        auto future = std::make_shared<std::future<ResourceHandle<T>>>(task->get_future());

        {
            std::lock_guard<std::mutex> lock(m_pendingCallbacksMutex);
            m_pendingCallbacks.push_back(PendingAsyncCallback{
                [future]() {
                    return future->wait_for(std::chrono::seconds(0)) == std::future_status::ready;
                },
                [future, callback = std::move(callback)]() mutable {
                    auto handle = future->get();
                    if (callback)
                    {
                        callback(handle);
                    }
                }
            });
        }

        EnqueueAsyncJob([task]() {
            (*task)();
        });
    }

} // namespace RVX::Resource

namespace RVX
{
    using Resource::IResourceLoader;
    using Resource::ExportResourceHotReloadDiagnosticJson;
    using Resource::RVX_RESOURCE_HOT_RELOAD_DIAGNOSTIC_SCHEMA_ID;
    using Resource::RVX_RESOURCE_HOT_RELOAD_DIAGNOSTIC_SCHEMA_VERSION;
    using Resource::ResourceHotReloadDiagnostic;
    using Resource::ResourceHotReloadStatus;
    using Resource::ResourceManager;
    using Resource::ResourceManagerConfig;
    using Resource::SaveResourceHotReloadDiagnosticJson;
}
