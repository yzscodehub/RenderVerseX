#pragma once

/**
 * @file ResourceManager.h
 * @brief Main facade for the resource management system
 */

#include "Core/Job/JobSystem.h"
#include "Core/Diagnostics/Trace.h"
#include "Resource/AssetResidencyLease.h"
#include "Resource/DependencyGraph.h"
#include "Resource/IResource.h"
#include "Resource/PreparedResourceBundle.h"
#include "Resource/ResourceCache.h"
#include "Resource/ResourceDiagnostics.h"
#include "Resource/ResourceHandle.h"
#include "Resource/ResourceLoadOperation.h"
#include "Resource/ResourceRetirementLedger.h"
#include "Resource/ResourceRegistry.h"
#include "Resource/RuntimeResourcePolicy.h"
#include "Resource/Types/MaterialInstanceResource.h"
#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace RVX::Resource
{
    // Forward declarations
    class IResourceLoader;
    class ModelResource;
    class ModelTextureStreamingService;
    class TextureResource;

    struct ModelTextureStreamingStats
    {
        uint64 decodedByteBudget = 256ull * 1024ull * 1024ull;
        uint64 reservedDecodedBytes = 0;
        uint64 peakReservedDecodedBytes = 0;
        uint64 completedDecodedBytes = 0;
        uint32 maxConcurrentDecodes = 0;
        uint32 activeDecodes = 0;
        uint32 peakActiveDecodes = 0;
        uint32 queuedDecodes = 0;
        uint32 pendingPublications = 0;
        uint64 completedDecodes = 0;
        uint64 failedDecodes = 0;
        uint64 cancelledDecodes = 0;
    };

    /** @brief CPU-side result of cancelling one model's deferred texture stream. */
    struct ModelTextureStreamingCancellationResult
    {
        bool modelFound = false;
        uint64 cancelledQueuedDecodes = 0;
        uint64 cancellationRequestedForActiveDecodes = 0;
        uint64 cancelledPendingPublications = 0;
        uint64 releasedReservedBytes = 0;
    };

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

        /// Maximum simultaneously expanded RGBA/mip bytes for model textures.
        uint64 modelTextureDecodedByteBudget = 256ull * 1024ull * 1024ull;

        /// Clamped to Core JobSystem worker count and an engine maximum of four.
        uint32 modelTextureMaxConcurrentDecodes = 4;

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
        // A closure admitted by ResourceSubsystem has already captured this
        // exact RenderResourceHandle before the cache releases CPU ownership.
        // The legacy per-resource callback must not request a second release.
        ResourceRetirementToken closureRetirementToken{};
    };

    using ResourceLifecycleEventCallback =
        std::function<void(const ResourceLifecycleEvent&)>;

    enum class ResourceClosureRetirementAdmissionCode : uint8
    {
        CommitCpuClosure = 0,
        RetryLater,
        FailedRetained,
    };

    /** @brief Result of the pre-commit Render retirement admission. */
    struct ResourceClosureRetirementAdmission
    {
        ResourceClosureRetirementAdmissionCode code =
            ResourceClosureRetirementAdmissionCode::RetryLater;
        ResourceClosureRetirementReceipt receipt{};
    };

    using ResourceClosureRetirementCallback = std::function<
        ResourceClosureRetirementAdmission(const ResourceClosureRetirementOutcome&)>;

    /** @brief Manager-side consumption result for an exact Scene residency pin. */
    enum class AssetResidencyLeaseConsumeCode : uint8
    {
        QueuedForClosure = 0,
        ReleasedShared,
        RetainedFailure,
        InvalidLease,
        NotFound,
    };

    struct AssetResidencyLeaseConsumeResult
    {
        AssetResidencyLeaseConsumeCode code =
            AssetResidencyLeaseConsumeCode::InvalidLease;
        AssetId rootAssetId{};
        uint64 leaseGeneration = 0;
        uint64 closureGeneration = 0;

        [[nodiscard]] bool Consumed() const noexcept
        {
            return code == AssetResidencyLeaseConsumeCode::QueuedForClosure ||
                   code == AssetResidencyLeaseConsumeCode::ReleasedShared;
        }
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
        bool IsInitialized() const;

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

        /** @brief Retain an exactly typed cached resource without initiating I/O. */
        template<typename T>
        [[nodiscard]] ResourceHandle<T> TryAcquireLoaded(ResourceId id);

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

        /**
         * @brief Begin a prepared load with an immutable caller-supplied loader state.
         *
         * Stateful loaders validate the supplied state and its canonical import
         * hash during admission. The state is then captured by the worker
         * context, so later loader configuration changes cannot alter the
         * admitted AssetKey meaning.
         */
        template<typename T>
        ResourceLoadHandle<T> RequestAsync(
            const std::string& path,
            ResourceLoadOptions options,
            ResourceLoadPreparationStateRef preparationState);

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
        // Runtime Material Instances
        // =====================================================================

        /** @brief Create and publish a runtime instance of a loaded immutable material. */
        [[nodiscard]] MaterialInstanceCreateResult CreateMaterialInstance(
            const ResourceHandle<MaterialResource>& parent,
            const std::string& runtimeKey);

        /**
         * @brief Atomically validate and apply legal PBR instance changes.
         *
         * The owner/update thread is the sole writer. Texture-dependency
         * changes reject while the existing closure has an AssetResidencyLease,
         * pending an explicit lease closure-refresh contract.
         */
        [[nodiscard]] MaterialInstanceMutationReceipt UpdateMaterialInstance(
            const MaterialInstanceHandle& instance,
            const MaterialInstancePatch& patch);

        // =====================================================================
        // Unloading
        // =====================================================================

        /// Unload a resource by path.
        [[nodiscard]] AssetResidencyReleaseResult Unload(const std::string& path);

        /** @brief Unload one exact AssetKey variant without affecting siblings. */
        [[nodiscard]] AssetResidencyReleaseResult Unload(const AssetKey& assetKey);

        /// Unload a resource by ID. Dependency-closure pins report BlockedByLease.
        [[nodiscard]] AssetResidencyReleaseResult Unload(ResourceId id);

        /** @brief Evict one exact cached variant while retaining registry metadata. */
        [[nodiscard]] AssetResidencyReleaseResult Evict(const AssetKey& assetKey);

        /** @brief Acquire a move-only pin for the exact loaded AssetKey closure. */
        [[nodiscard]] AssetResidencyLease AcquireAssetResidencyLease(
            const AssetKey& assetKey);

        /**
         * @brief True only when this exact lease is the sole active consumer.
         *
         * Invalid, stale, or concurrently acquiring lease epochs return false
         * so asset-wide background work is never cancelled on ambiguous
         * ownership evidence.
         */
        [[nodiscard]] bool IsSoleAssetResidencyConsumer(
            const AssetResidencyLease& lease) const noexcept;

        /** @brief Resolve the exact published variant for an already-loaded resource. */
        [[nodiscard]] std::optional<AssetKey> FindPublishedAssetKey(
            ResourceId resourceId) const;

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

        /** @brief Start deferred model texture decode after fallback residency. */
        bool BeginModelTextureStreaming(
            ResourceHandle<ModelResource> model);

        /** @brief GPU publications whose decoded-byte leases remain active. */
        [[nodiscard]] std::vector<ResourceHandle<TextureResource>>
            GetPendingModelTexturePublications() const;

        /** @brief Retire one decoded-byte lease after render completion/failure. */
        bool CompleteModelTexturePublication(ResourceId textureId,
                                             bool succeeded,
                                             std::string error = {});

        /** @brief Cancel queued and publication-waiting work for one model. */
        [[nodiscard]] ModelTextureStreamingCancellationResult
            CancelModelTextureStreaming(ResourceId modelId);

        [[nodiscard]] ModelTextureStreamingStats
            GetModelTextureStreamingStats() const;

        /** @brief Register the callback drained only by ProcessCompletedLoads. */
        void SetLifecycleEventCallback(ResourceLifecycleEventCallback callback);

        /** @brief Register the sole pre-commit closure-retirement admission callback. */
        void SetClosureRetirementCallback(
            ResourceClosureRetirementCallback callback);

        /**
         * @brief Consume one Scene-owned residency lease into the exact closure path.
         *
         * The result is value-only. A failed consumption leaves @p lease valid
         * and therefore preserves CPU residency for retry evidence.
         */
        [[nodiscard]] AssetResidencyLeaseConsumeResult
            ConsumeSceneAssetResidencyLease(AssetResidencyLease& lease);

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

        /** @brief Return a copyable diagnostic snapshot without publishing work. */
        [[nodiscard]] ResourceDiagnosticsSnapshot GetDiagnosticsSnapshot() const;

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
        bool m_shuttingDown = false;
        ResourceManagerConfig m_config;

        std::unique_ptr<ResourceRegistry> m_registry;
        std::unique_ptr<ResourceCache> m_cache;
        std::unique_ptr<DependencyGraph> m_dependencyGraph;

        std::unordered_map<ResourceType, std::shared_ptr<IResourceLoader>> m_loaders;

        std::function<void(ResourceId, IResource*)> m_reloadCallback;

        mutable std::mutex m_lifecycleMutex;
        std::vector<ResourceLifecycleEvent> m_lifecycleEvents;
        ResourceLifecycleEventCallback m_lifecycleEventCallback;
        ResourceClosureRetirementCallback m_closureRetirementCallback;

        /**
         * Protects manager lifecycle/cache lifetime plus loader registration
         * and in-flight operation maps. Never held during IO/parse/decode.
         */
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
        std::atomic<uint64> m_cancelledLoadCount{0};
        std::atomic<uint64> m_closureUnloadRequestCount{0};
        std::atomic<uint64> m_closureUnloadedResourceCount{0};
        std::atomic<uint64> m_closureRetainedResourceCount{0};
        std::atomic<uint64> m_closureUnloadRejectedCount{0};
        std::unique_ptr<ModelTextureStreamingService>
            m_modelTextureStreaming;

        std::shared_ptr<AssetResidencyLeaseControl> m_assetResidencyControl;
        mutable std::mutex m_pendingLeaseUnloadMutex;
        std::vector<std::pair<AssetKey, uint64>> m_pendingLeaseUnloads;

        enum class RootOwnershipState : uint8
        {
            Active = 0,
            ReleasePending
        };

        struct PublishedRootRecord
        {
            AssetKey assetKey;
            RootOwnershipState ownership = RootOwnershipState::Active;
            uint64 pendingClosureGeneration = 0;
        };

        // Exact identity persists while an explicitly released root is
        // physically retained by another graph consumer. The ownership state
        // lives in the same map entry, so Active <-> ReleasePending changes
        // never allocate and cannot fail between logical release and retry.
        std::unordered_map<ResourceId, PublishedRootRecord>
            m_publishedAssetKeys;
        uint64 m_nextClosureGeneration = 1;

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
            ResourceType requestedType = ResourceType::Unknown,
            ResourceLoadPreparationStateRef preparationState = {});
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
        struct ClosureUnloadPlan
        {
            // Dependents precede their dependencies. This permits every
            // graph removal to leave a valid remaining graph.
            std::vector<ResourceId> removalOrder;
            std::vector<ResourceId> sharedRetainedResourceIds;
        };

        [[nodiscard]] std::optional<ClosureUnloadPlan> BuildClosureUnloadPlan(
            ResourceId rootResourceId) const;
        [[nodiscard]] AssetResidencyReleaseResult UnloadClosureNow(
            ResourceId rootResourceId);
        [[nodiscard]] bool IsPublishedRootActive(
            ResourceId resourceId,
            const AssetKey& assetKey) const;
        bool ReactivatePublishedRoot(
            ResourceId resourceId,
            const AssetKey& assetKey);
        [[nodiscard]] AssetResidencyReleaseResult UnloadResourceNow(ResourceId id);
        [[nodiscard]] AssetResidencyReleaseResult EvictResourceNow(ResourceId id);
        [[nodiscard]] uint64 AllocateClosureGeneration() noexcept;
        [[nodiscard]] std::optional<ResourceClosureRetirementOutcome>
            MakeClosureRetirementOutcome(
                ResourceId rootResourceId,
                const ClosureUnloadPlan& plan) const;
        void ProcessPendingLeaseUnloads();
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

        /**
         * @brief Validate immutable state supplied by a request caller.
         *
         * Stateless and legacy loaders reject non-null explicit state by
         * default. Stateful loaders must verify both the concrete state type
         * and requested hash before returning the canonical worker snapshot.
         */
        virtual bool ValidatePreparationState(
            uint64 requestedImportOptionsHash,
            ResourceLoadPreparationStateRef suppliedState,
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
    ResourceHandle<T> ResourceManager::TryAcquireLoaded(ResourceId id)
    {
        std::lock_guard<std::recursive_mutex> lock(m_loadMutex);
        if (!m_initialized || m_shuttingDown || !m_cache)
        {
            return {};
        }

        return m_cache->TryAcquireLoaded<T>(id);
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

    template<typename T>
    ResourceLoadHandle<T> ResourceManager::RequestAsync(
        const std::string& path,
        ResourceLoadOptions options,
        ResourceLoadPreparationStateRef preparationState)
    {
        static_assert(std::is_base_of_v<IResource, T>, "T must derive from IResource");
        ResourceLoadOperationOwner operation = RequestPreparedLoad(
            path,
            std::move(options),
            GetResourceTypeHint<T>(),
            std::move(preparationState));
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
