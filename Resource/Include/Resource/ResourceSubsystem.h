#pragma once

/**
 * @file ResourceSubsystem.h
 * @brief Update-thread owner of CPU resources and immutable render uploads.
 */

#include "Core/Subsystem/EngineSubsystem.h"
#include "RenderContracts/IRenderResourceGateway.h"
#include "Resource/ResourceDiagnosticsView.h"
#include "Resource/ResourceHandle.h"
#include "Resource/ResourceManager.h"
#include "Resource/ResourcePublicationView.h"
#include "Resource/ResourceRetirementLedger.h"
#include "Resource/Types/ModelResource.h"

#include <functional>
#include <future>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace RVX::Resource
{
    enum class RenderResourceResolveCode : uint8
    {
        Resolved = 0,
        NotFound = 1,
        KindMismatch = 2,
        StaleGeneration = 3
    };

    struct RenderResourceResolveResult
    {
        RenderResourceResolveCode code = RenderResourceResolveCode::NotFound;
        RenderResourceHandle handle{};
        RenderResourceStatus status{};
    };

    /** @brief Update-side resource-to-render publication diagnostics. */
    struct ResourceRenderStats
    {
        uint64 readyEvents = 0;
        uint64 reloadEvents = 0;
        uint64 unloadEvents = 0;
        uint64 reservations = 0;
        uint64 existingReservations = 0;
        uint64 enqueueAccepted = 0;
        uint64 queueFullByCount = 0;
        uint64 queueFullByBytes = 0;
        uint64 retryAttempts = 0;
        uint64 releasesAccepted = 0;
        uint64 terminalRequestsReclaimed = 0;
        uint64 replacementsQueued = 0;
        uint64 replacementsCoalesced = 0;
        uint64 replacementFailures = 0;
        uint64 buildFailures = 0;
        uint64 gatewayRejections = 0;
        uint64 wrongThreadMutations = 0;
        size_t pendingResourceCount = 0;
        size_t pendingUploadCount = 0;
        size_t retainedRequestCount = 0;
    };

    /** @brief Immutable evidence retained after ResourceSubsystem shutdown. */
    struct ResourceShutdownDiagnostics
    {
        bool available = false;
        ResourceDiagnosticsSnapshot beforeManagerShutdown{};
        ResourceDiagnosticsSnapshot afterManagerShutdown{};
        bool lifecycleCallbackDetached = false;
        bool managerStopped = false;
        bool clean = false;
    };

    enum class ModelTextureStreamingRenderCancellationState : uint8
    {
        Cancelled = 0,
        AwaitingRenderRetirement
    };

    /** @brief Cancellation result spanning stream CPU ownership and gateway handoff. */
    struct ModelTextureStreamingCancellationReport
    {
        ModelTextureStreamingCancellationResult stream;
        uint64 discardedPendingUploads = 0;
        uint64 discardedPendingReplacements = 0;
        uint64 acceptedRenderReplacements = 0;
        ModelTextureStreamingRenderCancellationState renderState =
            ModelTextureStreamingRenderCancellationState::Cancelled;
    };

    /**
     * @brief Sole update-thread producer for the render-resource gateway.
     */
    class ResourceSubsystem : public EngineSubsystem,
                              public IResourcePublicationView,
                              public IResourceDiagnosticsView
    {
    public:
        // =====================================================================
        // Construction and EngineSubsystem
        // =====================================================================
        ResourceSubsystem() = default;
        ~ResourceSubsystem() override = default;

        const char* GetName() const override { return "ResourceSubsystem"; }
        /** @brief Configure the manager before Engine initializes the subsystem. */
        [[nodiscard]] bool Configure(const ResourceManagerConfig& config);
        void Initialize() override;
        void Initialize(const ResourceManagerConfig& config);
        void Deinitialize() override;
        bool ShouldTick() const override { return true; }
        void Tick(float deltaTime) override;

        /** @brief Inject Render's non-owning narrow gateway. */
        void SetRenderResourceGateway(IRenderResourceGateway* gateway);

        /** @brief Resolve an AssetId to its exact current generational handle. */
        RenderResourceResolveResult ResolveRenderResource(
            AssetId assetId,
            RenderResourceKind kind) const;

        /**
         * @brief Read one exact Resource publication without mutating its state.
         *
         * This must be called by the update-owner thread. A different caller
         * receives ResourcePublicationQueryCode::WrongThread without a log or
         * state change.
         */
        [[nodiscard]] ResourcePublicationQueryResult QueryResourcePublication(
            ResourceId resourceId,
            ResourceType expectedType) const noexcept override;

        /**
         * @brief Read the complete Resource diagnostics snapshot by value.
         *
         * This observes update-owned manager and gateway-handoff state, so a
         * non-owner caller is rejected before any mutable container is read.
         */
        [[nodiscard]] ResourceDiagnosticsQueryResult
            QueryResourceDiagnostics() const override;

        /** @brief Publish or atomically replace a runtime-created GPU resource. */
        [[nodiscard]] bool PublishRenderResource(
            ResourceHandle<IResource> resource,
            RenderResourceContentOperation operation =
                RenderResourceContentOperation::Create);

        /**
         * @brief Seal new upload production while preserving mappings needed
         * by already accepted render frames until the terminal drain.
         */
        void BeginRenderShutdown();

        /** @brief Release retained request payloads after observing terminal state. */
        void DrainTerminalRenderRequests();

        /**
         * @brief Admit an exact ResourceManager closure result for GPU retirement.
         *
         * A later ResourceManager closure implementation must call this before
         * it removes the corresponding tracked render mappings.  The outcome
         * carries only CPU closure ownership; this subsystem snapshots the
         * current AssetId-to-RenderResourceHandle generations before it asks
         * Render to release anything.
         */
        [[nodiscard]] ResourceClosureRetirementSubmitResult
            SubmitClosureRetirement(
                const ResourceClosureRetirementOutcome& outcome);

        /** @brief Poll one exact closure through the narrow render gateway. */
        [[nodiscard]] ResourceClosureRetirementPollResult
            PollClosureRetirement(ResourceRetirementToken token);

        /** @brief Return a copy of an exact closure retirement receipt. */
        [[nodiscard]] std::optional<ResourceClosureRetirementReceipt>
            QueryClosureRetirement(ResourceRetirementToken token) const;

        /** @brief Discard a terminal receipt; stale or duplicate tokens reject. */
        [[nodiscard]] ResourceClosureRetirementAcknowledgeResult
            AcknowledgeClosureRetirement(ResourceRetirementToken token);

        /**
         * @brief Transfer one Scene consumer's exact CPU lease into closure retirement.
         *
         * A non-final consumer completes as shared without touching Render.
         * The final consumer remains queued until ResourceManager has produced
         * its exact removed/shared closure outcome and Render has retired the
         * captured generations.
         */
        [[nodiscard]] ResourceSceneClosureReleaseBeginResult
            BeginSceneAssetClosureRelease(AssetResidencyLease&& lease) noexcept;

        [[nodiscard]] std::optional<ResourceSceneClosureReleaseReceipt>
            QuerySceneAssetClosureRelease(
                ResourceSceneClosureReleaseToken token) const;

        [[nodiscard]] ResourceSceneClosureReleaseAcknowledgeResult
            AcknowledgeSceneAssetClosureRelease(
                ResourceSceneClosureReleaseToken token);

        /** @brief Explicitly fail outstanding receipts after a Render device-loss signal. */
        void NotifyRenderDeviceLost();

        /**
         * @brief Cancel a model stream before Scene teardown touches its actors.
         *
         * CPU decode/publication work is removed immediately. Requests already
         * accepted by the render gateway remain completion-owned and are
         * reported as AwaitingRenderRetirement rather than being unsafely
         * cancelled from the update thread.
         */
        [[nodiscard]] ModelTextureStreamingCancellationReport
            CancelModelTextureStreaming(ResourceHandle<ModelResource> model);

        /** @brief Re-arm a cancelled cached stream for a newly active Scene consumer. */
        [[nodiscard]] bool EnsureModelTextureStreaming(
            ResourceHandle<ModelResource> model);

        /**
         * @brief Begin a previously registered model texture stream.
         *
         * Registration and decode are deliberately separate. The Scene asset
         * coordinator may call this only after its minimum-resident fallback
         * revision has been consumed and presented by Render. ResourceSubsystem
         * does not infer that consumer-visible boundary from GPU resource
         * readiness alone.
         */
        [[nodiscard]] bool BeginModelTextureStreaming(
            ResourceHandle<ModelResource> model);

        /** @brief True while Render owns a cancelled model's accepted replacement. */
        [[nodiscard]] bool IsModelTextureStreamingRetirementPending(
            ResourceId modelId) const;

        /** @brief Transfer a CPU residency pin to the update-side retirement observer. */
        void RetainAssetResidencyUntilModelTextureRetirement(
            ResourceId modelId,
            AssetResidencyLease lease);

        /**
         * @brief True when an update-owned Scene asset coordinator may safely
         * cancel Scene state and transfer completion-owned residency leases.
         *
         * This intentionally uses ResourceSubsystem's resource-lifecycle
         * state, so direct subsystem validation remains supported without
         * relying on EngineSubsystem registration state.
         */
        [[nodiscard]] bool CanTeardownSceneAssets() const noexcept;

        ResourceRenderStats GetRenderResourceStats() const;
        [[nodiscard]] ResourceDiagnosticsSnapshot GetDiagnosticsSnapshot() const;
        [[nodiscard]] const ResourceShutdownDiagnostics&
            GetLastShutdownDiagnostics() const noexcept
        {
            return m_lastShutdownDiagnostics;
        }

        // =====================================================================
        // Resource Loading
        // =====================================================================
        template<typename T>
        ResourceHandle<T> Load(const std::string& path)
        {
            if (!RequireUpdateThread("Load"))
                return {};
            return ResourceManager::Get().Load<T>(path);
        }

        template<typename T>
        ResourceHandle<T> Load(ResourceId id)
        {
            if (!RequireUpdateThread("Load"))
                return {};
            return ResourceManager::Get().Load<T>(id);
        }

        /** @brief Retain an already-published loaded resource without initiating I/O. */
        template<typename T>
        [[nodiscard]] ResourceHandle<T> TryAcquireLoaded(ResourceId id)
        {
            return ResourceManager::Get().TryAcquireLoaded<T>(id);
        }

        template<typename T>
        std::future<ResourceHandle<T>> LoadAsync(const std::string& path)
        {
            if (!RequireUpdateThread("LoadAsync"))
                return {};
            return ResourceManager::Get().LoadAsync<T>(path);
        }

        /** @brief Begin a coalesced load without blocking the update thread. */
        template<typename T>
        ResourceLoadHandle<T> RequestAsync(
            const std::string& path,
            ResourceLoadOptions options = {})
        {
            if (!m_initialized || !RequireUpdateThread("RequestAsync"))
                return {};
            return ResourceManager::Get().RequestAsync<T>(
                path,
                std::move(options));
        }

        /** @brief Begin a load with caller-supplied immutable loader state. */
        template<typename T>
        ResourceLoadHandle<T> RequestAsync(
            const std::string& path,
            ResourceLoadOptions options,
            ResourceLoadPreparationStateRef preparationState)
        {
            if (!m_initialized || !RequireUpdateThread("RequestAsync"))
                return {};
            return ResourceManager::Get().RequestAsync<T>(
                path,
                std::move(options),
                std::move(preparationState));
        }

        template<typename T>
        void LoadAsync(const std::string& path,
                       std::function<void(ResourceHandle<T>)> callback)
        {
            if (!RequireUpdateThread("LoadAsync"))
                return;
            ResourceManager::Get().LoadAsync<T>(path, std::move(callback));
        }

        bool IsLoaded(const std::string& path) const;
        bool IsLoaded(ResourceId id) const;
        void Unload(const std::string& path);
        void Unload(ResourceId id);
        void UnloadUnused();
        void RegisterLoader(ResourceType type,
                            std::unique_ptr<IResourceLoader> loader);
        void EnableHotReload(bool enable);
        void OnResourceReloaded(
            std::function<void(ResourceId, IResource*)> callback);
        void SetCacheLimit(size_t bytes);
        void ClearCache();
        ResourceManager::Stats GetStats() const;
        ResourceManager& GetManager();
        const ResourceManager& GetManager() const;

    private:
        // =====================================================================
        // Update-side State
        // =====================================================================
        struct PendingResource
        {
            AssetId assetId{};
            RenderResourceKind kind = RenderResourceKind::Invalid;
            ResourceHandle<IResource> resource;
            RenderUploadPriority priority = RenderUploadPriority::Normal;
            uint64 fifoOrder = 0;
            uint64 sourceRevision = 0;
            RenderResourceContentOperation operation =
                RenderResourceContentOperation::Create;
        };

        struct PendingUpload
        {
            AssetId assetId{};
            RenderResourceKind kind = RenderResourceKind::Invalid;
            RenderResourceHandle handle{};
            ResourceUploadRequestRef request{};
            RenderUploadPriority priority = RenderUploadPriority::Normal;
            uint64 fifoOrder = 0;
        };

        struct TrackedResource
        {
            RenderResourceKind kind = RenderResourceKind::Invalid;
            RenderResourceHandle handle{};
        };

        struct RetainedRequest
        {
            AssetId assetId{};
            RenderResourceHandle handle{};
            ResourceUploadRequestRef request{};
        };

        // =====================================================================
        // Internal Methods
        // =====================================================================
        bool IsOnUpdateThread() const;
        bool RequireUpdateThread(const char* operation);
        void HandleLifecycleEvent(const ResourceLifecycleEvent& event);
        void QueueReadyResource(
            const ResourceLifecycleEvent& event,
            RenderResourceContentOperation operation);
        [[nodiscard]] bool QueueRenderResourceTree(
            ResourceHandle<IResource> resource,
            RenderResourceContentOperation operation);
        void ProcessPendingResources();
        void ProcessPendingUploads();
        void ProcessModelTextureStreaming();
        void TrackModelTextureStreaming(
            ResourceHandle<ModelResource> model);
        void FailModelTextureStreamingForAsset(
            AssetId assetId,
            const std::string& reason);
        void RetireModelTextureStreamingRequest(
            const RenderResourceHandle& handle);
        bool TryBuildPendingResource(PendingResource& pending);
        RenderResourceHandle ResolveDependency(AssetId assetId,
                                               RenderResourceKind kind) const;
        void ReleaseAsset(AssetId assetId);
        void RemovePendingAsset(AssetId assetId);
        [[nodiscard]] ResourceClosureRetirementAdmission
            AdmitClosureRetirement(
                const ResourceClosureRetirementOutcome& outcome);
        void PollClosureRetirements();
        /**
         * @brief Discard only terminal Scene receipts during subsystem shutdown.
         *
         * Runtime callers keep terminal evidence until they explicitly
         * acknowledge it. Shutdown is the one ownership boundary where no
         * caller can observe the receipt again, so terminal ledger and Scene
         * tombstones must be drained before ResourceManager tears down its
         * residency control.
         */
        void DrainTerminalSceneClosureReleasesForShutdown();
        void RemoveTrackedRetirementCaptures(
            ResourceRetirementToken token);
        void UpdateSceneClosureReleaseFromAdmission(
            const ResourceClosureRetirementOutcome& outcome,
            const ResourceClosureRetirementAdmission& admission);
        static RenderResourceKind ToRenderResourceKind(ResourceType type);
        static RenderUploadPriority GetUploadPriority(ResourceType type);

        // =====================================================================
        // Members
        // =====================================================================
        ResourceManagerConfig m_config{};
        IRenderResourceGateway* m_gateway = nullptr;
        std::thread::id m_updateThreadId{};
        // Query admission is synchronized independently from the update-owned
        // resource maps so a tooling thread can be rejected safely while the
        // subsystem enters or leaves its owner-thread lifetime.
        mutable std::mutex m_publicationViewMutex;
        std::thread::id m_publicationViewOwner{};
        bool m_publicationViewAvailable = false;
        bool m_initialized = false;
        bool m_renderShuttingDown = false;
        uint64 m_nextRequestSequence = 1;
        uint64 m_nextFifoOrder = 1;
        uint64 m_nextSourceRevision = 1;

        std::vector<PendingResource> m_pendingResources{};
        std::vector<PendingUpload> m_pendingUploads{};
        std::unordered_map<AssetId, TrackedResource, AssetIdHash>
            m_trackedResources{};
        std::unordered_map<RenderResourceHandle,
                           RetainedRequest,
                           RenderResourceHandleHash>
            m_retainedRequests{};
        std::vector<ResourceUploadRequestRef> m_localTerminalRequests{};
        std::vector<ResourceHandle<ModelResource>> m_streamingModels{};
        std::unordered_map<ResourceId,
                           std::unordered_set<RenderResourceHandle,
                                              RenderResourceHandleHash>>
            m_pendingModelTextureRetirements{};
        std::unordered_map<ResourceId, std::vector<AssetResidencyLease>>
            m_retainedModelTextureLeases{};

        struct SceneClosureReleaseEntry
        {
            ResourceSceneClosureReleaseReceipt receipt{};
            // A failed manager consumption never drops the original lease.
            // This preserves CPU residency and a retryable value-owned proof.
            AssetResidencyLease retainedLease{};
        };
        uint64 m_nextSceneClosureReleaseToken = 1;
        std::vector<SceneClosureReleaseEntry> m_sceneClosureReleases{};
        std::vector<ResourceRetirementToken> m_activeClosureRetirements{};
        ResourceRetirementLedger m_retirementLedger{};
        ResourceRenderStats m_renderStats{};
        ResourceShutdownDiagnostics m_lastShutdownDiagnostics{};
    };
} // namespace RVX::Resource

namespace RVX
{
    using Resource::RenderResourceResolveCode;
    using Resource::RenderResourceResolveResult;
    using Resource::IResourcePublicationView;
    using Resource::IResourceDiagnosticsView;
    using Resource::ResourceDiagnosticsQueryCode;
    using Resource::ResourceDiagnosticsQueryResult;
    using Resource::ResourcePublicationQueryCode;
    using Resource::ResourcePublicationQueryResult;
    using Resource::ResourceRenderStats;
    using Resource::ResourceShutdownDiagnostics;
    using Resource::ModelTextureStreamingCancellationReport;
    using Resource::ModelTextureStreamingRenderCancellationState;
    using Resource::ResourceSubsystem;
} // namespace RVX
