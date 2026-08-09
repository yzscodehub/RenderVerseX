#pragma once

/**
 * @file ResourceManager.h
 * @brief Main facade for the resource management system
 */

#include "Core/Job/JobSystem.h"
#include "Core/Diagnostics/Trace.h"
#include "Resource/DependencyGraph.h"
#include "Resource/IResource.h"
#include "Resource/PreparedResourceBundle.h"
#include "Resource/ResourceCache.h"
#include "Resource/ResourceHandle.h"
#include "Resource/ResourceLoadOperation.h"
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

    /** @brief Immutable loader-specific state captured at request admission. */
    class ResourceLoadPreparationState
    {
    public:
        virtual ~ResourceLoadPreparationState() = default;
    };

    using ResourceLoadPreparationStateRef =
        std::shared_ptr<const ResourceLoadPreparationState>;

    template<typename T>
    constexpr ResourceType GetResourceTypeHint()
    {
        if constexpr (requires { T::StaticResourceType; })
        {
            return static_cast<ResourceType>(T::StaticResourceType);
        }
        else
        {
            return ResourceType::Unknown;
        }
    }

    /**
     * @brief Immutable worker input for a resource preparation operation.
     *
     * This deliberately contains no ResourceManager, ResourceCache, Scene or
     * render-gateway reference.  A loader may read, parse and decode through
     * it, then returns a PreparedResourceBundle for owner-thread publication.
     */
    struct ResourceLoadPreparationContext
    {
        AssetKey assetKey;

        /// Stable cache identity for the complete AssetKey.  This is distinct
        /// from resolvedPath, which exists only for worker-side IO.
        std::string resourceIdentityPath;
        std::string requestedPath;
        std::string resolvedPath;
        ResourceId rootResourceId = InvalidResourceId;
        Diagnostics::TraceContext traceContext;
        ResourceLoadPreparationStateRef loaderState;
        std::function<bool()> isCancellationRequested;

        [[nodiscard]] bool IsCancellationRequested() const
        {
            return isCancellationRequested && isCancellationRequested();
        }
    };

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

        /** @brief Optional correlated diagnostics context; disabled by default. */
        Diagnostics::TraceContext startupTraceContext{};
    };

    inline ResourceManagerConfig MakeResourceManagerConfigForAppMode(AppMode mode)
    {
        ResourceManagerConfig config;
        config.runtimePolicy = MakeResourceRuntimePolicyForAppMode(mode);
        config.enableHotReload = AllowsHotReload(mode);
        return config;
    }

    /** @brief Update-side lifecycle values produced by resource loading/cache work. */
    enum class ResourceLifecycleEventType : uint8
    {
        Ready = 0,
        Reloaded = 1,
        BeforeUnload = 2
    };

    struct ResourceLifecycleEvent
    {
        ResourceLifecycleEventType type = ResourceLifecycleEventType::Ready;
        ResourceId resourceId = InvalidResourceId;
        ResourceHandle<IResource> resource;
    };

    using ResourceLifecycleEventCallback =
        std::function<void(const ResourceLifecycleEvent&)>;

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

        /**
         * @brief Return the immutable startup correlation context configured
         * for this manager.
         *
         * Loaders use this value only to record observed work.  They must not
         * mutate the session or manufacture timing outside their own execution
         * boundary.
         */
        [[nodiscard]] Diagnostics::TraceContext GetStartupTraceContext() const
        {
            return m_config.startupTraceContext;
        }

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
        IResource* LoadResource(const std::string& path, ResourceType requestedType);
        IResource* LoadResource(ResourceId id);

        // =====================================================================
        // Asynchronous Loading
        // =====================================================================

        /**
         * @brief Compatibility future for a prepared asynchronous resource load.
         *
         * The future becomes ready only after the owner/update thread pumps
         * ProcessCompletedLoads(). Callers on that thread must not wait on it;
         * doing so would prevent the publish that resolves the future.
         */
        template<typename T>
        std::future<ResourceHandle<T>> LoadAsync(const std::string& path);

        /**
         * @brief Begin a coalesced prepared load.
         *
         * The returned subscription becomes Ready only after the update/owner
         * thread calls ProcessCompletedLoads().  Workers never publish cache,
         * registry, Scene or render state.
         */
        template<typename T>
        ResourceLoadHandle<T> RequestAsync(const std::string& path,
                                           ResourceLoadOptions options = {});

        /** @brief Owner-thread-pumped callback compatibility overload. */
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

        /** @brief Query an exact import/profile variant returned by a load handle. */
        bool IsLoaded(const AssetKey& assetKey) const;

        /// Check if resource is loaded by ID
        bool IsLoaded(ResourceId id) const;

        // =====================================================================
        // Unloading
        // =====================================================================

        /// Unload a resource by path
        void Unload(const std::string& path);

        /** @brief Unload one exact AssetKey variant without affecting siblings. */
        void Unload(const AssetKey& assetKey);

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

        /** @brief Register the callback drained only by ProcessCompletedLoads. */
        void SetLifecycleEventCallback(ResourceLifecycleEventCallback callback);

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

        std::unordered_map<ResourceType, std::shared_ptr<IResourceLoader>> m_loaders;

        std::function<void(ResourceId, IResource*)> m_reloadCallback;

        mutable std::mutex m_lifecycleMutex;
        std::vector<ResourceLifecycleEvent> m_lifecycleEvents;
        ResourceLifecycleEventCallback m_lifecycleEventCallback;

        /** Protects loader registration and in-flight operation maps only. Never held during IO/parse/decode. */
        mutable std::recursive_mutex m_loadMutex;

        struct PreparedLoadCompletion
        {
            AssetKey assetKey;
            ResourceLoadOperationOwner operation;
            ResourcePathResolution resolution;
            std::string requestedPath;
            PreparedResourceBundle bundle;
            ResourceLoadError error;
            bool cancelled = false;
        };

        struct LegacyAsyncWaiter
        {
            ResourceLoadHandle<IResource> subscription;
            std::function<void(ResourceHandle<IResource>)> completion;
            bool readyWithoutSubscription = false;
        };

        mutable std::mutex m_preparedCompletionMutex;
        std::vector<PreparedLoadCompletion> m_preparedCompletions;
        mutable std::mutex m_preparedJobMutex;
        std::vector<JobHandle> m_preparedJobs;
        std::unordered_map<AssetKey, ResourceLoadOperationOwner, AssetKeyHash> m_inFlightLoads;
        mutable std::mutex m_legacyAsyncWaiterMutex;
        std::vector<LegacyAsyncWaiter> m_legacyAsyncWaiters;
        std::atomic<bool> m_acceptAsyncRequests{false};
        uint64 m_ownerThreadToken = 0;

        bool m_jobSystemInitializedByManager = false;
        std::atomic<size_t> m_pendingAsyncJobCount{0};
        std::atomic<size_t> m_pendingAsyncCompletionCount{0};

        mutable std::mutex m_diagnosticMutex;
        ResourceLoadDiagnostic m_lastLoadDiagnostic;

        mutable std::mutex m_hotReloadMutex;
        ResourceHotReloadDiagnostic m_hotReloadDiagnostic;
        bool m_hotReloadInitializedByManager = false;
        uint32_t m_hotReloadCallbackId = 0;
        std::unordered_map<ResourceId, uint32_t> m_hotReloadWatchIds;

        // Internal loading
        IResource* LoadInternal(const std::string& path,
                                const ResourcePathResolution& resolution,
                                ResourceType type,
                                ResourceId managerId,
                                std::shared_ptr<IResourceLoader> loader = {});
        IResource* LoadPreparedSynchronously(
            const ResourcePathResolution& resolution,
            const std::string& requestedPath,
            const AssetKey& assetKey,
            std::shared_ptr<IResourceLoader> loader,
            ResourceLoadPreparationStateRef loaderState);
        ResourceLoadOperationOwner RequestPreparedLoad(
            const std::string& path,
            ResourceLoadOptions options,
            ResourceType requestedType = ResourceType::Unknown);
        void ExecutePreparedLoad(ResourceLoadOperationOwner operation,
                                 std::shared_ptr<IResourceLoader> loader,
                                 ResourceLoadPreparationContext context,
                                 ResourcePathResolution resolution);
        void DrainPreparedLoadCompletions(bool shutdownCancellation = false);
        void DrainLegacyAsyncWaiters();
        bool PublishPreparedBundle(const ResourcePathResolution& resolution,
                                   const std::string& requestedPath,
                                   const AssetKey& assetKey,
                                   PreparedResourceBundle& bundle,
                                   ResourceLoadError& outError);
        bool ValidatePreparedBundle(const PreparedResourceBundle& bundle,
                                    ResourceLoadError& outError) const;
        void RemoveInFlightLoad(const AssetKey& assetKey,
                                ResourceLoadRequestId requestId);
        bool IsOwnerThread() const;
        void LoadDependencies(IResource* resource);
        void SetLastLoadDiagnostic(const ResourceLoadDiagnostic& diagnostic);
        bool IsHotReloadSupportedByPolicy() const;
        void ConfigureHotReload(bool enable);
        void RegisterHotReloadResource(IResource* resource, const ResourcePathResolution& resolution);
        void QueueLifecycleEvent(ResourceLifecycleEventType type,
                                 IResource* resource);
        void DrainLifecycleEvents();
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

        /**
         * @brief Prepare resources without owner-state side effects.
         *
         * Built-in production loaders override this method.  Third-party
         * legacy loaders can keep using Load() synchronously, but are rejected
         * from the asynchronous prepared path until they supply this contract.
         */
        virtual bool Prepare(const ResourceLoadPreparationContext& context,
                             PreparedResourceBundle& outBundle,
                             ResourceLoadError& outError);

        /** @brief Whether synchronous compatibility loads may use Prepare + owner publish. */
        virtual bool SupportsPreparedLoading() const { return false; }

        /**
         * @brief Capture immutable loader configuration for one AssetKey.
         *
         * Stateless loaders keep the default implementation. Stateful loaders
         * must return a snapshot and its canonical import hash so later worker
         * execution cannot drift from the admitted cache identity.
         */
        virtual bool CapturePreparationState(
            uint64 requestedImportOptionsHash,
            ResourceLoadPreparationStateRef& outState,
            uint64& outCanonicalImportOptionsHash,
            ResourceLoadError& outError) const;

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
        IResource* resource = LoadResource(path, GetResourceTypeHint<T>());
        return ResourceHandle<T>(dynamic_cast<T*>(resource));
    }

    template<typename T>
    ResourceHandle<T> ResourceManager::Load(ResourceId id)
    {
        static_assert(std::is_base_of_v<IResource, T>, "T must derive from IResource");
        IResource* resource = LoadResource(id);
        return ResourceHandle<T>(dynamic_cast<T*>(resource));
    }

    template<typename T>
    std::future<ResourceHandle<T>> ResourceManager::LoadAsync(const std::string& path)
    {
        static_assert(std::is_base_of_v<IResource, T>, "T must derive from IResource");
        auto promise = std::make_shared<std::promise<ResourceHandle<T>>>();
        std::future<ResourceHandle<T>> result = promise->get_future();
        ResourceLoadOperationOwner operation =
            RequestPreparedLoad(path, {}, GetResourceTypeHint<T>());
        ResourceLoadHandle<IResource> subscription =
            operation ? operation.Subscribe<IResource>() : ResourceLoadHandle<IResource>{};
        if (!subscription)
        {
            LegacyAsyncWaiter waiter;
            waiter.readyWithoutSubscription = true;
            waiter.completion = [promise](ResourceHandle<IResource>)
            {
                promise->set_value({});
            };
            std::lock_guard<std::mutex> lock(m_legacyAsyncWaiterMutex);
            m_legacyAsyncWaiters.push_back(std::move(waiter));
            return result;
        }

        LegacyAsyncWaiter waiter;
        waiter.subscription = std::move(subscription);
        waiter.completion = [promise](ResourceHandle<IResource> resource)
        {
            T* typed = dynamic_cast<T*>(resource.Get());
            promise->set_value(typed ? ResourceHandle<T>(typed) : ResourceHandle<T>{});
        };
        {
            std::lock_guard<std::mutex> lock(m_legacyAsyncWaiterMutex);
            m_legacyAsyncWaiters.push_back(std::move(waiter));
        }
        return result;
    }

    template<typename T>
    void ResourceManager::LoadAsync(const std::string& path, std::function<void(ResourceHandle<T>)> callback)
    {
        static_assert(std::is_base_of_v<IResource, T>, "T must derive from IResource");
        ResourceLoadOperationOwner operation =
            RequestPreparedLoad(path, {}, GetResourceTypeHint<T>());
        ResourceLoadHandle<IResource> subscription =
            operation ? operation.Subscribe<IResource>() : ResourceLoadHandle<IResource>{};
        if (!subscription)
        {
            LegacyAsyncWaiter waiter;
            waiter.readyWithoutSubscription = true;
            waiter.completion = [callback = std::move(callback)](ResourceHandle<IResource>) mutable
            {
                if (callback)
                {
                    callback({});
                }
            };
            std::lock_guard<std::mutex> lock(m_legacyAsyncWaiterMutex);
            m_legacyAsyncWaiters.push_back(std::move(waiter));
            return;
        }

        LegacyAsyncWaiter waiter;
        waiter.subscription = std::move(subscription);
        waiter.completion = [callback = std::move(callback)](ResourceHandle<IResource> resource) mutable
        {
            if (!callback)
                return;
            T* typed = dynamic_cast<T*>(resource.Get());
            callback(typed ? ResourceHandle<T>(typed) : ResourceHandle<T>{});
        };
        {
            std::lock_guard<std::mutex> lock(m_legacyAsyncWaiterMutex);
            m_legacyAsyncWaiters.push_back(std::move(waiter));
        }

    }

    template<typename T>
    ResourceLoadHandle<T> ResourceManager::RequestAsync(const std::string& path,
                                                         ResourceLoadOptions options)
    {
        static_assert(std::is_base_of_v<IResource, T>, "T must derive from IResource");
        ResourceLoadOperationOwner operation = RequestPreparedLoad(
            path,
            std::move(options),
            GetResourceTypeHint<T>());
        return operation ? operation.Subscribe<T>() : ResourceLoadHandle<T>{};
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
    using Resource::ResourceLifecycleEvent;
    using Resource::ResourceLifecycleEventCallback;
    using Resource::ResourceLifecycleEventType;
    using Resource::ResourceManager;
    using Resource::ResourceManagerConfig;
    using Resource::MakeResourceManagerConfigForAppMode;
    using Resource::SaveResourceHotReloadDiagnosticJson;
}
