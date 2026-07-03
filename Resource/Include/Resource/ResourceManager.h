#pragma once

/**
 * @file ResourceManager.h
 * @brief Main facade for the resource management system
 */

#include "Core/Job/JobSystem.h"
#include "Resource/DependencyGraph.h"
#include "Resource/IResource.h"
#include "Resource/ResourceCache.h"
#include "Resource/ResourceHandle.h"
#include "Resource/ResourceRegistry.h"
#include "Resource/RuntimeResourcePolicy.h"
#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <type_traits>
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
        /// Used when ResourceManager initializes Core JobSystem for async loads.
        int asyncThreadCount = 2;

        /// Base path for resources
        std::string basePath = "";

        /// Runtime/source/cooked/package path policy
        ResourceRuntimePolicy runtimePolicy;

        /// Enable hot reload
        bool enableHotReload = false;
    };

    inline ResourceManagerConfig MakeResourceManagerConfigForAppMode(AppMode mode)
    {
        ResourceManagerConfig config;
        config.runtimePolicy = MakeResourceRuntimePolicyForAppMode(mode);
        config.enableHotReload = AllowsSourceAssetAccess(mode);
        return config;
    }

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
        bool m_initialized = false;
        ResourceManagerConfig m_config;

        std::unique_ptr<ResourceRegistry> m_registry;
        std::unique_ptr<ResourceCache> m_cache;
        std::unique_ptr<DependencyGraph> m_dependencyGraph;

        std::unordered_map<ResourceType, std::unique_ptr<IResourceLoader>> m_loaders;

        std::function<void(ResourceId, IResource*)> m_reloadCallback;

        mutable std::recursive_mutex m_loadMutex;

        bool m_jobSystemInitializedByManager = false;
        std::atomic<size_t> m_pendingAsyncJobCount{0};
        std::atomic<size_t> m_pendingAsyncCompletionCount{0};

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
        static_assert(std::is_base_of_v<IResource, T>, "T must derive from IResource");
        IResource* resource = LoadResource(path);
        return ResourceHandle<T>(static_cast<T*>(resource));
    }

    template<typename T>
    ResourceHandle<T> ResourceManager::Load(ResourceId id)
    {
        static_assert(std::is_base_of_v<IResource, T>, "T must derive from IResource");
        IResource* resource = LoadResource(id);
        return ResourceHandle<T>(static_cast<T*>(resource));
    }

    template<typename T>
    std::future<ResourceHandle<T>> ResourceManager::LoadAsync(const std::string& path)
    {
        static_assert(std::is_base_of_v<IResource, T>, "T must derive from IResource");
        m_pendingAsyncJobCount.fetch_add(1, std::memory_order_relaxed);
        JobSubmissionDesc desc;
        desc.category = "Resource.LoadAsync";
        desc.priority = JobPriority::Normal;

        return JobSystem::Get().SubmitWithResult([this, path]() {
            struct PendingLoadGuard
            {
                std::atomic<size_t>& counter;
                ~PendingLoadGuard()
                {
                    counter.fetch_sub(1, std::memory_order_relaxed);
                }
            } guard{m_pendingAsyncJobCount};

            return Load<T>(path);
        }, desc);
    }

    template<typename T>
    void ResourceManager::LoadAsync(const std::string& path, std::function<void(ResourceHandle<T>)> callback)
    {
        static_assert(std::is_base_of_v<IResource, T>, "T must derive from IResource");
        auto completedHandle = std::make_shared<ResourceHandle<T>>();
        m_pendingAsyncJobCount.fetch_add(1, std::memory_order_relaxed);
        m_pendingAsyncCompletionCount.fetch_add(1, std::memory_order_relaxed);

        JobSubmissionDesc desc;
        desc.category = "Resource.LoadAsync";
        desc.priority = JobPriority::Normal;
        desc.completionDispatch = JobCompletionDispatch::MainThread;
        desc.continuation = [this, completedHandle, callback = std::move(callback)]() mutable {
            struct PendingCompletionGuard
            {
                std::atomic<size_t>& counter;
                ~PendingCompletionGuard()
                {
                    counter.fetch_sub(1, std::memory_order_relaxed);
                }
            } guard{m_pendingAsyncCompletionCount};

            if (callback)
            {
                callback(*completedHandle);
            }
        };

        JobSystem::Get().Submit([this, path, completedHandle]() {
            struct PendingLoadGuard
            {
                std::atomic<size_t>& counter;
                ~PendingLoadGuard()
                {
                    counter.fetch_sub(1, std::memory_order_relaxed);
                }
            } guard{m_pendingAsyncJobCount};

            *completedHandle = Load<T>(path);
        }, desc);

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
    using Resource::MakeResourceManagerConfigForAppMode;
    using Resource::SaveResourceHotReloadDiagnosticJson;
}
