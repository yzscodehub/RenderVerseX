#include "Resource/ResourceSubsystem.h"

#include "Core/Assert.h"
#include "Core/Log.h"
#include "Resource/RenderUploadRequestBuilder.h"
#include "Resource/Types/ModelResource.h"

#include <algorithm>
#include <limits>
#include <type_traits>
#include <unordered_set>
#include <utility>

namespace RVX::Resource
{
namespace
{
    static_assert(
        std::is_nothrow_move_constructible_v<
            ResourceSceneClosureReleaseReceipt>);
    static_assert(
        std::is_nothrow_move_assignable_v<
            ResourceSceneClosureReleaseReceipt>);
    bool IsTerminalStatus(const RenderResourceStatus& status)
    {
        if (status.code == RenderResourceStatusCode::StaleGeneration ||
            status.code == RenderResourceStatusCode::InvalidHandle)
        {
            return true;
        }
        return (status.state == RenderResourcePublicState::GPUReady &&
                status.replacementState ==
                    RenderResourceReplacementState::None) ||
               status.state == RenderResourcePublicState::Failed ||
               status.state == RenderResourcePublicState::Released;
    }

    bool HigherPriority(RenderUploadPriority left,
                        RenderUploadPriority right)
    {
        return static_cast<uint8>(left) > static_cast<uint8>(right);
    }

    bool HasOutstandingShutdownWork(
        const ResourceDiagnosticsSnapshot& snapshot)
    {
        return snapshot.activeOperations != 0 ||
               snapshot.activeSubscribers != 0 ||
               snapshot.pendingAsyncJobs != 0 ||
               snapshot.pendingAsyncCompletions != 0 ||
               snapshot.cacheEntryCount != 0 ||
               snapshot.decodeQueuedCount != 0 ||
               snapshot.decodeActiveCount != 0 ||
               snapshot.decodeReservedBytes != 0 ||
               snapshot.pendingPublicationCount != 0 ||
               snapshot.pendingUploadCount != 0 ||
               snapshot.pendingReplacementCount != 0 ||
               snapshot.pendingRollbackCount != 0 ||
               snapshot.pendingRetirementCount != 0 ||
               snapshot.activeLeaseCount != 0 ||
               snapshot.protectedResourceCount != 0 ||
               snapshot.queuedLeaseUnloadCount != 0;
    }
} // namespace

    bool ResourceSubsystem::Configure(const ResourceManagerConfig& config)
    {
        if (m_initialized)
        {
            RVX_VERIFY(false,
                       "ResourceSubsystem configuration is immutable after initialization");
            return false;
        }
        m_config = config;
        return true;
    }

    void ResourceSubsystem::Initialize()
    {
        if (m_initialized)
            return;

        m_updateThreadId = std::this_thread::get_id();
        m_renderShuttingDown = false;
        m_lastShutdownDiagnostics = {};
        ResourceManager& manager = ResourceManager::Get();
        manager.Initialize(m_config);
        manager.SetLifecycleEventCallback(
            [this](const ResourceLifecycleEvent& event)
            {
                HandleLifecycleEvent(event);
            });
        manager.SetClosureRetirementCallback(
            [this](const ResourceClosureRetirementOutcome& outcome)
            {
                return AdmitClosureRetirement(outcome);
            });
        m_initialized = true;
        {
            std::lock_guard lock(m_publicationViewMutex);
            m_publicationViewOwner = m_updateThreadId;
            m_publicationViewAvailable = true;
        }
    }

    void ResourceSubsystem::Initialize(const ResourceManagerConfig& config)
    {
        if (!Configure(config))
            return;
        Initialize();
    }

    void ResourceSubsystem::Deinitialize()
    {
        if (!m_initialized)
            return;
        RVX_ASSERT_MSG(
            IsOnUpdateThread(),
            "ResourceSubsystem::Deinitialize must run on the registered "
            "update thread; returning would leave ResourceManager with a "
            "callback to a subsystem the collection may destroy");
        {
            std::lock_guard lock(m_publicationViewMutex);
            m_publicationViewAvailable = false;
            m_publicationViewOwner = {};
        }

        BeginRenderShutdown();
        DrainTerminalRenderRequests();
        DrainTerminalSceneClosureReleasesForShutdown();
        ResourceShutdownDiagnostics shutdown;
        shutdown.available = true;
        shutdown.beforeManagerShutdown = GetDiagnosticsSnapshot();

        // Engine shutdown stops Render before reverse-deinitializing Resource.
        // A normal Render stop and a device-lost teardown both publish a
        // terminal status for every accepted request, which
        // DrainTerminalRenderRequests consumes above. IRenderResourceGateway
        // deliberately has no API that can prove a non-terminal request is no
        // longer in GPU use. Continuing in that state would detach the manager
        // callback and let the subsystem collection destroy this object while
        // ResourceManager still owns a callback to it. Fail closed instead of
        // creating a zombie ResourceSubsystem or releasing a
        // completion-owned lease.
        RVX_ASSERT_MSG(
            m_retainedRequests.empty() &&
                m_pendingModelTextureRetirements.empty() &&
                m_retainedModelTextureLeases.empty() &&
                m_sceneClosureReleases.empty() &&
                m_activeClosureRetirements.empty() &&
                m_retirementLedger.GetOutstandingGpuRetirementCount() == 0,
            "ResourceSubsystem cannot deinitialize before Render publishes "
            "terminal retirement for every accepted resource request");

        ResourceManager& manager = ResourceManager::Get();
        // ResourceManager may deliver final cancellation/unload events while
        // it drains jobs and cache entries. Detach first so those callbacks
        // cannot observe partially cleared ResourceSubsystem state.
        manager.SetLifecycleEventCallback({});
        manager.SetClosureRetirementCallback({});
        shutdown.lifecycleCallbackDetached = true;
        manager.Shutdown();

        m_pendingResources.clear();
        m_pendingUploads.clear();
        m_localTerminalRequests.clear();
        m_retainedRequests.clear();
        m_pendingModelTextureRetirements.clear();
        m_retainedModelTextureLeases.clear();
        m_sceneClosureReleases.clear();
        m_activeClosureRetirements.clear();
        m_retirementLedger = {};
        m_trackedResources.clear();
        m_streamingModels.clear();
        m_gateway = nullptr;
        m_initialized = false;
        m_updateThreadId = {};
        shutdown.afterManagerShutdown = GetDiagnosticsSnapshot();
        shutdown.managerStopped = !manager.IsInitialized();
        shutdown.clean = shutdown.lifecycleCallbackDetached &&
                         shutdown.managerStopped &&
                         !HasOutstandingShutdownWork(
                             shutdown.afterManagerShutdown);
        m_lastShutdownDiagnostics = std::move(shutdown);
    }

    void ResourceSubsystem::Tick(float deltaTime)
    {
        (void)deltaTime;
        if (!m_initialized || !RequireUpdateThread("Tick"))
            return;

        ResourceManager& manager = ResourceManager::Get();
        manager.ProcessCompletedLoads();
        if (m_config.enableHotReload)
        {
            manager.CheckForChanges();
            manager.ProcessCompletedLoads();
        }

        DrainTerminalRenderRequests();
        PollClosureRetirements();
        if (!m_renderShuttingDown)
        {
            ProcessPendingResources();
            ProcessPendingUploads();
            ProcessModelTextureStreaming();
        }
    }

    void ResourceSubsystem::SetRenderResourceGateway(
        IRenderResourceGateway* gateway)
    {
        if (m_initialized && !RequireUpdateThread("SetRenderResourceGateway"))
            return;
        if (m_renderShuttingDown && gateway != nullptr)
        {
            ++m_renderStats.gatewayRejections;
            RVX_VERIFY(false,
                       "ResourceSubsystem rejected a gateway mutation after render shutdown began");
            return;
        }
        m_gateway = gateway;
    }

    RenderResourceResolveResult ResourceSubsystem::ResolveRenderResource(
        AssetId assetId,
        RenderResourceKind kind) const
    {
        RenderResourceResolveResult result;
        const auto tracked = m_trackedResources.find(assetId);
        if (tracked == m_trackedResources.end())
            return result;
        if (tracked->second.kind != kind)
        {
            result.code = RenderResourceResolveCode::KindMismatch;
            return result;
        }

        result.handle = tracked->second.handle;
        if (m_gateway == nullptr)
            return result;
        result.status = m_gateway->QueryResourceStatus(result.handle);
        if (result.status.code == RenderResourceStatusCode::StaleGeneration ||
            result.status.code == RenderResourceStatusCode::InvalidHandle)
        {
            result.code = RenderResourceResolveCode::StaleGeneration;
            return result;
        }
        result.code = RenderResourceResolveCode::Resolved;
        return result;
    }

    ResourcePublicationQueryResult
    ResourceSubsystem::QueryResourcePublication(
        ResourceId resourceId,
        ResourceType expectedType) const noexcept
    {
        ResourcePublicationQueryResult result;
        result.resourceId = resourceId;
        if (resourceId == InvalidResourceId)
        {
            result.code = ResourcePublicationQueryCode::InvalidResourceId;
            return result;
        }

        result.kind = ToRenderResourceKind(expectedType);
        if (result.kind == RenderResourceKind::Invalid)
        {
            result.code = ResourcePublicationQueryCode::UnsupportedResourceType;
            return result;
        }
        {
            std::lock_guard lock(m_publicationViewMutex);
            if (!m_publicationViewAvailable)
            {
                result.code = ResourcePublicationQueryCode::NotInitialized;
                return result;
            }
            if (m_publicationViewOwner != std::this_thread::get_id())
            {
                result.code = ResourcePublicationQueryCode::WrongThread;
                return result;
            }
        }

        const RenderResourceResolveResult resolved = ResolveRenderResource(
            AssetId{resourceId}, result.kind);
        result.handle = resolved.handle;
        result.status = resolved.status;
        switch (resolved.code)
        {
            case RenderResourceResolveCode::Resolved:
                result.code = ResourcePublicationQueryCode::Resolved;
                break;
            case RenderResourceResolveCode::NotFound:
                result.code = resolved.handle.IsValid()
                                  ? ResourcePublicationQueryCode::GatewayUnavailable
                                  : ResourcePublicationQueryCode::NotFound;
                break;
            case RenderResourceResolveCode::KindMismatch:
                result.code = ResourcePublicationQueryCode::KindMismatch;
                break;
            case RenderResourceResolveCode::StaleGeneration:
                result.code = ResourcePublicationQueryCode::StaleGeneration;
                break;
            default:
                result.code = ResourcePublicationQueryCode::NotFound;
                break;
        }
        return result;
    }

    ResourceDiagnosticsQueryResult
    ResourceSubsystem::QueryResourceDiagnostics() const
    {
        ResourceDiagnosticsQueryResult result;
        {
            std::lock_guard lock(m_publicationViewMutex);
            if (!m_publicationViewAvailable)
            {
                result.code = ResourceDiagnosticsQueryCode::NotInitialized;
                result.snapshot =
                    DiagnosticValue<ResourceDiagnosticsSnapshot>::Unavailable(
                        "ResourceSubsystem diagnostics are unavailable outside its initialized lifetime.");
                return result;
            }
            if (m_publicationViewOwner != std::this_thread::get_id())
            {
                result.code = ResourceDiagnosticsQueryCode::WrongThread;
                result.snapshot =
                    DiagnosticValue<ResourceDiagnosticsSnapshot>::Unavailable(
                        "ResourceSubsystem diagnostics may only be queried from its update-owner thread.");
                return result;
            }
        }

        result.code = ResourceDiagnosticsQueryCode::Available;
        result.snapshot =
            DiagnosticValue<ResourceDiagnosticsSnapshot>::Available(
                GetDiagnosticsSnapshot());
        return result;
    }

    bool ResourceSubsystem::PublishRenderResource(
        ResourceHandle<IResource> resource,
        RenderResourceContentOperation operation)
    {
        if (!m_initialized || m_renderShuttingDown ||
            !RequireUpdateThread("PublishRenderResource") ||
            (operation != RenderResourceContentOperation::Create &&
             operation != RenderResourceContentOperation::Replace))
        {
            return false;
        }
        return QueueRenderResourceTree(std::move(resource), operation);
    }

    void ResourceSubsystem::BeginRenderShutdown()
    {
        if (!m_initialized || m_renderShuttingDown)
            return;
        if (!RequireUpdateThread("BeginRenderShutdown"))
            return;

        m_renderShuttingDown = true;
        for (const ResourceHandle<ModelResource>& model : m_streamingModels)
        {
            if (model)
                model->CancelTextureStreaming();
        }
        for (const ResourceHandle<ModelResource>& model : m_streamingModels)
        {
            if (model)
            {
                static_cast<void>(
                    ResourceManager::Get().CancelModelTextureStreaming(
                        model.GetId()));
            }
        }
        m_streamingModels.clear();
        m_pendingResources.clear();
        for (PendingUpload& pending : m_pendingUploads)
        {
            if (pending.request)
                m_localTerminalRequests.push_back(std::move(pending.request));
        }
        m_pendingUploads.clear();
    }

    void ResourceSubsystem::DrainTerminalRenderRequests()
    {
        if (m_initialized && !RequireUpdateThread(
                                 "DrainTerminalRenderRequests"))
        {
            return;
        }

        PollClosureRetirements();

        const size_t localCount = m_localTerminalRequests.size();
        m_localTerminalRequests.clear();
        m_renderStats.terminalRequestsReclaimed += localCount;

        if (m_gateway == nullptr)
            return;

        auto request = m_retainedRequests.begin();
        while (request != m_retainedRequests.end())
        {
            const RenderResourceStatus status =
                m_gateway->QueryResourceStatus(request->second.handle);
            if (!IsTerminalStatus(status))
            {
                ++request;
                continue;
            }

            if (status.state == RenderResourcePublicState::Released ||
                status.code != RenderResourceStatusCode::Current)
            {
                const auto tracked =
                    m_trackedResources.find(request->second.assetId);
                if (tracked != m_trackedResources.end() &&
                    tracked->second.handle == request->second.handle)
                {
                    m_trackedResources.erase(tracked);
                }
            }
            RetireModelTextureStreamingRequest(request->second.handle);
            request = m_retainedRequests.erase(request);
            ++m_renderStats.terminalRequestsReclaimed;
        }

        // BeginRenderShutdown deliberately preserves current mappings while
        // the dedicated renderer drains accepted frames. Once Render has
        // stopped, its registry owns no live GPU resources and these lookup
        // mappings can be discarded without enqueueing releases ahead of the
        // frames that still reference them.
        if (m_renderShuttingDown)
        {
            m_trackedResources.clear();
        }
    }

    ResourceClosureRetirementSubmitResult
    ResourceSubsystem::SubmitClosureRetirement(
        const ResourceClosureRetirementOutcome& outcome)
    {
        ResourceClosureRetirementSubmitResult result;
        if (!m_initialized)
        {
            result.code = ResourceClosureRetirementSubmitCode::NotInitialized;
            return result;
        }
        if (!RequireUpdateThread("SubmitClosureRetirement"))
        {
            result.code = ResourceClosureRetirementSubmitCode::WrongThread;
            return result;
        }
        if (m_renderShuttingDown)
        {
            result.code =
                ResourceClosureRetirementSubmitCode::RenderShuttingDown;
            return result;
        }
        std::vector<ResourceRetirementHandleCapture> captures;
        try
        {
            captures.reserve(outcome.removedResourceIds.size());
            // This reservation happens before ResourceRetirementLedger may
            // call RequestRelease. Once Begin returns a token, recording it
            // below is allocation-free and cannot lose its exact captures.
            m_activeClosureRetirements.reserve(
                m_activeClosureRetirements.size() + 1);
        }
        catch (...)
        {
            result.code = ResourceClosureRetirementSubmitCode::AllocationFailure;
            return result;
        }
        for (ResourceId resourceId : outcome.removedResourceIds)
        {
            const auto tracked = m_trackedResources.find(AssetId{resourceId});
            if (tracked != m_trackedResources.end())
            {
                // Copy the complete Render generation before RequestRelease;
                // the ledger never resolves from ResourceId after this point.
                captures.push_back(
                    ResourceRetirementHandleCapture{resourceId,
                                                    tracked->second.handle});
            }
        }

        // CPU-only closures have no exact Render generation to retire. They
        // still receive a terminal ledger receipt so closure generations and
        // shutdown accounting remain deterministic, but a Render gateway is
        // only a prerequisite when at least one capture exists.
        if (!captures.empty() && m_gateway == nullptr)
        {
            result.code =
                ResourceClosureRetirementSubmitCode::GatewayUnavailable;
            return result;
        }

        result = m_retirementLedger.Begin(outcome, captures, m_gateway);
        if (result.receipt.token.IsValid())
        {
            const bool alreadyTracked = std::any_of(
                m_activeClosureRetirements.begin(),
                m_activeClosureRetirements.end(),
                [&result](const ResourceRetirementToken& existing)
                {
                    return existing == result.receipt.token;
                });
            if (!alreadyTracked)
            {
                // reserve above makes this move/copy allocation-free.
                m_activeClosureRetirements.push_back(result.receipt.token);
            }
        }
        return result;
    }

    ResourceClosureRetirementPollResult
    ResourceSubsystem::PollClosureRetirement(ResourceRetirementToken token)
    {
        if (!m_initialized || !RequireUpdateThread("PollClosureRetirement"))
        {
            return {};
        }
        const std::optional<ResourceClosureRetirementReceipt> current =
            m_retirementLedger.Query(token);
        if (!current)
        {
            return {};
        }
        if (current->IsTerminal())
        {
            return {ResourceClosureRetirementPollCode::Updated, *current};
        }
        if (m_gateway == nullptr)
        {
            return {};
        }
        return m_retirementLedger.Poll(token, *m_gateway);
    }

    std::optional<ResourceClosureRetirementReceipt>
    ResourceSubsystem::QueryClosureRetirement(ResourceRetirementToken token) const
    {
        if (!m_initialized || !IsOnUpdateThread())
        {
            return std::nullopt;
        }
        return m_retirementLedger.Query(token);
    }

    ResourceClosureRetirementAcknowledgeResult
    ResourceSubsystem::AcknowledgeClosureRetirement(
        ResourceRetirementToken token)
    {
        if (!m_initialized || !RequireUpdateThread(
                "AcknowledgeClosureRetirement"))
        {
            return {};
        }
        ResourceClosureRetirementAcknowledgeResult result =
            m_retirementLedger.Acknowledge(token);
        if (result.code == ResourceClosureRetirementAcknowledgeCode::Acknowledged)
        {
            std::erase(m_activeClosureRetirements, token);
        }
        return result;
    }

    void ResourceSubsystem::NotifyRenderDeviceLost()
    {
        if (!m_initialized || !RequireUpdateThread("NotifyRenderDeviceLost"))
        {
            return;
        }
        m_retirementLedger.NotifyDeviceLost();
        PollClosureRetirements();
    }

    ResourceSceneClosureReleaseBeginResult
    ResourceSubsystem::BeginSceneAssetClosureRelease(
        AssetResidencyLease&& lease) noexcept
    {
        ResourceSceneClosureReleaseBeginResult result;
        if (!m_initialized)
        {
            result.code = ResourceSceneClosureReleaseBeginCode::NotInitialized;
            return result;
        }
        if (!RequireUpdateThread("BeginSceneAssetClosureRelease"))
        {
            result.code = ResourceSceneClosureReleaseBeginCode::WrongThread;
            return result;
        }
        if (!lease.IsValid())
        {
            return result;
        }
        if (m_nextSceneClosureReleaseToken == 0)
        {
            result.code = ResourceSceneClosureReleaseBeginCode::AllocationFailure;
            return result;
        }

        // The entry and the response each retain an AssetKey value. Copy both
        // before moving the caller's lease: an allocation failure must leave
        // that lease wholly owned by the caller and publish no receipt.
        SceneClosureReleaseEntry entry;
        ResourceSceneClosureReleaseReceipt response;
        try
        {
            m_sceneClosureReleases.reserve(m_sceneClosureReleases.size() + 1);

            const uint64 tokenValue = m_nextSceneClosureReleaseToken;
            const uint64 leaseGeneration = lease.GetGeneration();
            const AssetKey assetKey = lease.GetAssetKey();

            entry.receipt.token = ResourceSceneClosureReleaseToken{
                tokenValue,
                leaseGeneration};
            entry.receipt.assetKey = assetKey;
            entry.receipt.leaseGeneration = leaseGeneration;
            entry.receipt.state = ResourceSceneClosureReleaseState::Queued;

            // This intentionally copies the prepared receipt rather than
            // querying the persisted entry later. Query/result construction
            // must not allocate after ResourceManager consumes the lease.
            response = entry.receipt;
        }
        catch (...)
        {
            result.code = ResourceSceneClosureReleaseBeginCode::AllocationFailure;
            return result;
        }

        // AssetResidencyLease is move-only and its move is noexcept. From
        // here onward ResourceSubsystem, rather than the caller, owns the
        // valid lease until Manager consumes it. The reserved vector capacity
        // makes this append allocation-free; retain a defensive move-back
        // path for library/debug implementations that still report failure.
        static_assert(
            std::is_nothrow_move_constructible_v<SceneClosureReleaseEntry>);
        entry.retainedLease = std::move(lease);
        try
        {
            m_sceneClosureReleases.push_back(std::move(entry));
        }
        catch (...)
        {
            lease = std::move(entry.retainedLease);
            result.code = ResourceSceneClosureReleaseBeginCode::AllocationFailure;
            return result;
        }

        // Advance only after the receipt has acquired persistent ownership.
        // Value zero is reserved for invalid tokens.
        m_nextSceneClosureReleaseToken =
            m_nextSceneClosureReleaseToken ==
                    std::numeric_limits<uint64>::max()
                ? 1
                : m_nextSceneClosureReleaseToken + 1;
        SceneClosureReleaseEntry& persisted = m_sceneClosureReleases.back();

        AssetResidencyLeaseConsumeResult consumed;
        try
        {
            // Manager's transaction is designed to leave its root/control
            // state and this retained lease unchanged on failure. Keep this
            // catch nevertheless: this public service boundary is noexcept
            // and must expose unexpected Manager failure as retained evidence
            // rather than terminate an Engine shutdown path.
            consumed = ResourceManager::Get().ConsumeSceneAssetResidencyLease(
                persisted.retainedLease);
        }
        catch (...)
        {
            persisted.receipt.state =
                ResourceSceneClosureReleaseState::FailedRetained;
            response.state = persisted.receipt.state;
            result.code = ResourceSceneClosureReleaseBeginCode::RetainedFailure;
            result.receipt = std::move(response);
            return result;
        }

        persisted.receipt.rootAssetId = consumed.rootAssetId;
        persisted.receipt.closureGeneration = consumed.closureGeneration;
        response.rootAssetId = consumed.rootAssetId;
        response.closureGeneration = consumed.closureGeneration;
        switch (consumed.code)
        {
            case AssetResidencyLeaseConsumeCode::QueuedForClosure:
                result.code = ResourceSceneClosureReleaseBeginCode::Accepted;
                break;
            case AssetResidencyLeaseConsumeCode::ReleasedShared:
                persisted.receipt.state =
                    ResourceSceneClosureReleaseState::CompletedShared;
                response.state = persisted.receipt.state;
                result.code = ResourceSceneClosureReleaseBeginCode::Accepted;
                break;
            case AssetResidencyLeaseConsumeCode::InvalidLease:
            case AssetResidencyLeaseConsumeCode::NotFound:
            case AssetResidencyLeaseConsumeCode::RetainedFailure:
                persisted.receipt.state =
                    ResourceSceneClosureReleaseState::FailedRetained;
                response.state = persisted.receipt.state;
                result.code = ResourceSceneClosureReleaseBeginCode::RetainedFailure;
                break;
        }
        result.receipt = std::move(response);
        return result;
    }

    std::optional<ResourceSceneClosureReleaseReceipt>
    ResourceSubsystem::QuerySceneAssetClosureRelease(
        ResourceSceneClosureReleaseToken token) const
    {
        if (!m_initialized || !IsOnUpdateThread() || !token.IsValid())
        {
            return std::nullopt;
        }
        const auto found = std::find_if(
            m_sceneClosureReleases.begin(),
            m_sceneClosureReleases.end(),
            [token](const SceneClosureReleaseEntry& entry)
            {
                return entry.receipt.token == token;
            });
        return found == m_sceneClosureReleases.end()
                   ? std::nullopt
                   : std::optional<ResourceSceneClosureReleaseReceipt>(
                         found->receipt);
    }

    ResourceSceneClosureReleaseAcknowledgeResult
    ResourceSubsystem::AcknowledgeSceneAssetClosureRelease(
        ResourceSceneClosureReleaseToken token)
    {
        ResourceSceneClosureReleaseAcknowledgeResult result;
        if (!m_initialized || !RequireUpdateThread(
                "AcknowledgeSceneAssetClosureRelease"))
        {
            return result;
        }
        const auto found = std::find_if(
            m_sceneClosureReleases.begin(),
            m_sceneClosureReleases.end(),
            [token](const SceneClosureReleaseEntry& entry)
            {
                return entry.receipt.token == token;
            });
        if (found == m_sceneClosureReleases.end())
        {
            return result;
        }

        result.receipt = found->receipt;
        if (!result.receipt.IsTerminal())
        {
            result.code = ResourceSceneClosureReleaseAcknowledgeCode::NotTerminal;
            return result;
        }
        if (result.receipt.renderRetirementToken.IsValid())
        {
            const ResourceClosureRetirementAcknowledgeResult retired =
                AcknowledgeClosureRetirement(
                    result.receipt.renderRetirementToken);
            if (retired.code == ResourceClosureRetirementAcknowledgeCode::NotTerminal)
            {
                result.code = ResourceSceneClosureReleaseAcknowledgeCode::NotTerminal;
                return result;
            }
            if (retired.code != ResourceClosureRetirementAcknowledgeCode::Acknowledged)
            {
                return result;
            }
        }
        result.code = ResourceSceneClosureReleaseAcknowledgeCode::Acknowledged;
        m_sceneClosureReleases.erase(found);
        return result;
    }

    ResourceClosureRetirementAdmission
    ResourceSubsystem::AdmitClosureRetirement(
        const ResourceClosureRetirementOutcome& outcome)
    {
        ResourceClosureRetirementAdmission admission;
        const ResourceClosureRetirementSubmitResult submitted =
            SubmitClosureRetirement(outcome);
        admission.receipt = submitted.receipt;
        switch (submitted.code)
        {
            case ResourceClosureRetirementSubmitCode::Accepted:
                admission.code =
                    ResourceClosureRetirementAdmissionCode::CommitCpuClosure;
                break;
            case ResourceClosureRetirementSubmitCode::GatewayUnavailable:
            case ResourceClosureRetirementSubmitCode::AllocationFailure:
                admission.code =
                    ResourceClosureRetirementAdmissionCode::RetryLater;
                break;
            case ResourceClosureRetirementSubmitCode::GatewayRejected:
            case ResourceClosureRetirementSubmitCode::StaleBeforeAdmission:
            case ResourceClosureRetirementSubmitCode::DeviceLost:
            case ResourceClosureRetirementSubmitCode::DuplicateClosureGeneration:
            case ResourceClosureRetirementSubmitCode::InvalidOutcome:
            case ResourceClosureRetirementSubmitCode::TokenExhausted:
            case ResourceClosureRetirementSubmitCode::NotInitialized:
            case ResourceClosureRetirementSubmitCode::WrongThread:
            case ResourceClosureRetirementSubmitCode::RenderShuttingDown:
                admission.code =
                    ResourceClosureRetirementAdmissionCode::FailedRetained;
                break;
        }
        UpdateSceneClosureReleaseFromAdmission(outcome, admission);
        return admission;
    }

    void ResourceSubsystem::RemoveTrackedRetirementCaptures(
        ResourceRetirementToken token)
    {
        const std::vector<ResourceRetirementHandleCapture>* captures =
            m_retirementLedger.GetCaptures(token);
        if (captures == nullptr)
        {
            return;
        }
        for (const ResourceRetirementHandleCapture& capture : *captures)
        {
            const auto tracked = m_trackedResources.find(
                AssetId{capture.resourceId});
            if (tracked != m_trackedResources.end() &&
                tracked->second.handle == capture.handle)
            {
                m_trackedResources.erase(tracked);
            }
            RetireModelTextureStreamingRequest(capture.handle);
        }
    }

    void ResourceSubsystem::PollClosureRetirements()
    {
        for (auto token = m_activeClosureRetirements.begin();
             token != m_activeClosureRetirements.end();)
        {
            ResourceClosureRetirementPollResult polled;
            const std::optional<ResourceClosureRetirementReceipt> current =
                m_retirementLedger.Query(*token);
            if (!current)
            {
                token = m_activeClosureRetirements.erase(token);
                continue;
            }
            if (current->IsTerminal())
            {
                polled.code = ResourceClosureRetirementPollCode::Updated;
                polled.receipt = *current;
            }
            else
            {
                if (m_gateway == nullptr)
                {
                    ++token;
                    continue;
                }
                polled = m_retirementLedger.Poll(*token, *m_gateway);
            }
            if (polled.code == ResourceClosureRetirementPollCode::StaleToken)
            {
                token = m_activeClosureRetirements.erase(token);
                continue;
            }
            if (!polled.receipt.IsTerminal())
            {
                ++token;
                continue;
            }

            RemoveTrackedRetirementCaptures(*token);
            bool hasSceneConsumer = false;
            for (SceneClosureReleaseEntry& scene : m_sceneClosureReleases)
            {
                if (scene.receipt.renderRetirementToken != *token)
                {
                    continue;
                }
                hasSceneConsumer = true;
                switch (polled.receipt.state)
                {
                    case ResourceClosureRetirementState::Completed:
                        scene.receipt.state =
                            ResourceSceneClosureReleaseState::Completed;
                        break;
                    case ResourceClosureRetirementState::DeviceLost:
                        scene.receipt.state =
                            ResourceSceneClosureReleaseState::DeviceLost;
                        break;
                    case ResourceClosureRetirementState::Failed:
                        scene.receipt.state =
                            ResourceSceneClosureReleaseState::FailedRetained;
                        break;
                    default:
                        break;
                }
            }
            if (hasSceneConsumer)
            {
                ++token;
                continue;
            }

            // A legacy manager unload has no external receipt owner. Once
            // its exact captures have reached a terminal Render state, retire
            // the ledger entry immediately so shutdown cannot accumulate
            // completed tombstones.
            static_cast<void>(m_retirementLedger.Acknowledge(*token));
            token = m_activeClosureRetirements.erase(token);
        }
    }

    void ResourceSubsystem::DrainTerminalSceneClosureReleasesForShutdown()
    {
        // Runtime evidence remains queryable until the owning Scene service
        // explicitly acknowledges it. Deinitialization is different: Render
        // has already completed its terminal drain and ResourceManager is
        // about to destroy the residency control. Remove only receipts whose
        // Render retirement proof is terminal as well; a failed-retained
        // receipt with an in-flight partial release must still block shutdown.
        for (auto scene = m_sceneClosureReleases.begin();
             scene != m_sceneClosureReleases.end();)
        {
            if (!scene->receipt.IsTerminal())
            {
                ++scene;
                continue;
            }

            const ResourceRetirementToken renderToken =
                scene->receipt.renderRetirementToken;
            if (renderToken.IsValid())
            {
                const ResourceClosureRetirementAcknowledgeResult retired =
                    m_retirementLedger.Acknowledge(renderToken);
                if (retired.code ==
                    ResourceClosureRetirementAcknowledgeCode::NotTerminal)
                {
                    ++scene;
                    continue;
                }
                if (retired.code !=
                    ResourceClosureRetirementAcknowledgeCode::Acknowledged)
                {
                    // A stale ledger token is a lifecycle contract failure,
                    // not a reason to erase the Scene receipt silently.
                    ++scene;
                    continue;
                }
                std::erase(m_activeClosureRetirements, renderToken);
            }

            scene = m_sceneClosureReleases.erase(scene);
        }
    }

    void ResourceSubsystem::UpdateSceneClosureReleaseFromAdmission(
        const ResourceClosureRetirementOutcome& outcome,
        const ResourceClosureRetirementAdmission& admission)
    {
        auto scene = std::find_if(
            m_sceneClosureReleases.begin(),
            m_sceneClosureReleases.end(),
            [&outcome](const SceneClosureReleaseEntry& entry)
            {
                return entry.receipt.rootAssetId == outcome.rootAssetId &&
                       entry.receipt.closureGeneration ==
                           outcome.closureGeneration &&
                       entry.receipt.state ==
                           ResourceSceneClosureReleaseState::Queued;
            });
        if (scene == m_sceneClosureReleases.end())
        {
            return;
        }

        scene->receipt.sharedRetainedResourceCount =
            outcome.sharedRetainedResourceIds.size();
        scene->receipt.renderRetirementToken = admission.receipt.token;
        switch (admission.code)
        {
            case ResourceClosureRetirementAdmissionCode::CommitCpuClosure:
                scene->receipt.state = admission.receipt.IsTerminal()
                                           ? (admission.receipt.state ==
                                                      ResourceClosureRetirementState::Completed
                                                  ? ResourceSceneClosureReleaseState::Completed
                                                  : ResourceSceneClosureReleaseState::DeviceLost)
                                           : ResourceSceneClosureReleaseState::AwaitingGpuLastUse;
                break;
            case ResourceClosureRetirementAdmissionCode::RetryLater:
                break;
            case ResourceClosureRetirementAdmissionCode::FailedRetained:
                scene->receipt.state =
                    admission.receipt.state ==
                            ResourceClosureRetirementState::DeviceLost
                        ? ResourceSceneClosureReleaseState::DeviceLost
                        : ResourceSceneClosureReleaseState::FailedRetained;
                break;
        }
    }

    ResourceRenderStats ResourceSubsystem::GetRenderResourceStats() const
    {
        ResourceRenderStats stats = m_renderStats;
        stats.pendingResourceCount = m_pendingResources.size();
        stats.pendingUploadCount = m_pendingUploads.size();
        stats.retainedRequestCount = m_retainedRequests.size();
        return stats;
    }

    ResourceDiagnosticsSnapshot ResourceSubsystem::GetDiagnosticsSnapshot() const
    {
        ResourceDiagnosticsSnapshot snapshot =
            ResourceManager::Get().GetDiagnosticsSnapshot();
        snapshot.pendingUploadCount = m_pendingUploads.size();
        snapshot.pendingRetirementCount = m_retainedRequests.size();
        snapshot.pendingRollbackCount = m_localTerminalRequests.size();

        for (const PendingResource& pending : m_pendingResources)
        {
            if (pending.operation == RenderResourceContentOperation::Replace)
            {
                ++snapshot.pendingReplacementCount;
            }
        }
        for (const PendingUpload& pending : m_pendingUploads)
        {
            if (pending.request && pending.request->GetOperation() ==
                                       RenderResourceContentOperation::Replace)
            {
                ++snapshot.pendingReplacementCount;
            }
        }
        for (const auto& [handle, retained] : m_retainedRequests)
        {
            (void)handle;
            if (retained.request && retained.request->GetOperation() ==
                                        RenderResourceContentOperation::Replace)
            {
                ++snapshot.pendingReplacementCount;
            }
        }
        return snapshot;
    }

    ModelTextureStreamingCancellationReport
    ResourceSubsystem::CancelModelTextureStreaming(
        ResourceHandle<ModelResource> model)
    {
        ModelTextureStreamingCancellationReport report;
        if (!m_initialized || !model || !RequireUpdateThread(
                "CancelModelTextureStreaming"))
        {
            return report;
        }

        const ResourceId modelId = model.GetId();
        const std::vector<ResourceHandle<TextureResource>> textures =
            model->GetStreamingTextures();
        std::unordered_set<ResourceId> textureIds;
        textureIds.reserve(textures.size());
        for (const ResourceHandle<TextureResource>& texture : textures)
        {
            if (texture)
            {
                textureIds.insert(texture.GetId());
            }
        }

        report.stream = ResourceManager::Get().CancelModelTextureStreaming(
            modelId);
        model->CancelTextureStreaming();
        std::erase_if(
            m_streamingModels,
            [modelId](const ResourceHandle<ModelResource>& value)
            {
                return !value || value.GetId() == modelId;
            });

        const auto targetsTexture = [&textureIds](AssetId assetId)
        {
            return textureIds.contains(assetId.value);
        };
        auto pendingResource = m_pendingResources.begin();
        while (pendingResource != m_pendingResources.end())
        {
            if (!targetsTexture(pendingResource->assetId))
            {
                ++pendingResource;
                continue;
            }
            ++report.discardedPendingUploads;
            if (pendingResource->operation ==
                RenderResourceContentOperation::Replace)
            {
                ++report.discardedPendingReplacements;
            }
            pendingResource = m_pendingResources.erase(pendingResource);
        }

        auto pendingUpload = m_pendingUploads.begin();
        while (pendingUpload != m_pendingUploads.end())
        {
            if (!targetsTexture(pendingUpload->assetId))
            {
                ++pendingUpload;
                continue;
            }
            ++report.discardedPendingUploads;
            if (pendingUpload->request &&
                pendingUpload->request->GetOperation() ==
                    RenderResourceContentOperation::Replace)
            {
                ++report.discardedPendingReplacements;
            }
            if (pendingUpload->request)
            {
                m_localTerminalRequests.push_back(
                    std::move(pendingUpload->request));
            }
            pendingUpload = m_pendingUploads.erase(pendingUpload);
        }

        for (const auto& [handle, retained] : m_retainedRequests)
        {
            if (targetsTexture(retained.assetId) && retained.request &&
                retained.request->GetOperation() ==
                    RenderResourceContentOperation::Replace)
            {
                m_pendingModelTextureRetirements[modelId].insert(handle);
                ++report.acceptedRenderReplacements;
            }
        }
        if (report.acceptedRenderReplacements != 0)
        {
            report.renderState =
                ModelTextureStreamingRenderCancellationState::AwaitingRenderRetirement;
        }
        return report;
    }

    bool ResourceSubsystem::EnsureModelTextureStreaming(
        ResourceHandle<ModelResource> model)
    {
        if (!m_initialized || !model || !RequireUpdateThread(
                "EnsureModelTextureStreaming"))
        {
            return false;
        }

        const ModelTextureStreamingSnapshot before =
            model->GetTextureStreamingSnapshot();
        if (before.stage == ModelTextureStreamingStage::Cancelled)
        {
            static_cast<void>(model->RestartTextureStreamingAfterCancellation());
        }

        const ModelTextureStreamingSnapshot after =
            model->GetTextureStreamingSnapshot();
        if (after.stage == ModelTextureStreamingStage::Cancelled ||
            after.stage == ModelTextureStreamingStage::Failed)
        {
            return false;
        }
        TrackModelTextureStreaming(std::move(model));
        return true;
    }

    bool ResourceSubsystem::BeginModelTextureStreaming(
        ResourceHandle<ModelResource> model)
    {
        if (!m_initialized || m_renderShuttingDown || m_gateway == nullptr ||
            !model || !RequireUpdateThread("BeginModelTextureStreaming"))
        {
            return false;
        }

        const ModelTextureStreamingSnapshot snapshot =
            model->GetTextureStreamingSnapshot();
        switch (snapshot.stage)
        {
            case ModelTextureStreamingStage::None:
            case ModelTextureStreamingStage::FullyResident:
            case ModelTextureStreamingStage::Decoding:
            case ModelTextureStreamingStage::Uploading:
                // The operation is idempotent for an already started stream
                // and for models which have no deferred texture sources.
                return true;
            case ModelTextureStreamingStage::Failed:
            case ModelTextureStreamingStage::Cancelled:
                return false;
            case ModelTextureStreamingStage::AwaitingMinimumResident:
                break;
        }

        // Keep the model observable for replacement completion/failure and
        // cancellation before handing decode work to ResourceManager. This is
        // intentionally an explicit consumer action; Tick never calls it.
        TrackModelTextureStreaming(model);
        return ResourceManager::Get().BeginModelTextureStreaming(
            std::move(model));
    }

    bool ResourceSubsystem::IsModelTextureStreamingRetirementPending(
        ResourceId modelId) const
    {
        const auto found = m_pendingModelTextureRetirements.find(modelId);
        return found != m_pendingModelTextureRetirements.end() &&
               !found->second.empty();
    }

    void ResourceSubsystem::RetainAssetResidencyUntilModelTextureRetirement(
        ResourceId modelId,
        AssetResidencyLease lease)
    {
        if (!lease.IsValid())
        {
            return;
        }
        const auto pending = m_pendingModelTextureRetirements.find(modelId);
        if (pending == m_pendingModelTextureRetirements.end() ||
            pending->second.empty())
        {
            return;
        }
        m_retainedModelTextureLeases[modelId].push_back(std::move(lease));
    }

    bool ResourceSubsystem::CanTeardownSceneAssets() const noexcept
    {
        return m_initialized && IsOnUpdateThread() &&
               ResourceManager::Get().IsInitialized();
    }

    bool ResourceSubsystem::IsLoaded(const std::string& path) const
    {
        return ResourceManager::Get().IsLoaded(path);
    }

    bool ResourceSubsystem::IsLoaded(ResourceId id) const
    {
        return ResourceManager::Get().IsLoaded(id);
    }

    void ResourceSubsystem::Unload(const std::string& path)
    {
        if (RequireUpdateThread("Unload"))
            static_cast<void>(ResourceManager::Get().Unload(path));
    }

    void ResourceSubsystem::Unload(ResourceId id)
    {
        if (RequireUpdateThread("Unload"))
            static_cast<void>(ResourceManager::Get().Unload(id));
    }

    void ResourceSubsystem::UnloadUnused()
    {
        if (RequireUpdateThread("UnloadUnused"))
            ResourceManager::Get().UnloadUnused();
    }

    void ResourceSubsystem::RegisterLoader(
        ResourceType type,
        std::unique_ptr<IResourceLoader> loader)
    {
        if (RequireUpdateThread("RegisterLoader"))
            ResourceManager::Get().RegisterLoader(type, std::move(loader));
    }

    void ResourceSubsystem::EnableHotReload(bool enable)
    {
        if (!RequireUpdateThread("EnableHotReload"))
            return;
        m_config.enableHotReload = enable;
        ResourceManager::Get().EnableHotReload(enable);
    }

    void ResourceSubsystem::OnResourceReloaded(
        std::function<void(ResourceId, IResource*)> callback)
    {
        if (RequireUpdateThread("OnResourceReloaded"))
        {
            ResourceManager::Get().OnResourceReloaded(std::move(callback));
        }
    }

    void ResourceSubsystem::SetCacheLimit(size_t bytes)
    {
        if (RequireUpdateThread("SetCacheLimit"))
            ResourceManager::Get().SetCacheLimit(bytes);
    }

    void ResourceSubsystem::ClearCache()
    {
        if (RequireUpdateThread("ClearCache"))
            ResourceManager::Get().ClearCache();
    }

    ResourceManager::Stats ResourceSubsystem::GetStats() const
    {
        return ResourceManager::Get().GetStats();
    }

    ResourceManager& ResourceSubsystem::GetManager()
    {
        return ResourceManager::Get();
    }

    const ResourceManager& ResourceSubsystem::GetManager() const
    {
        return ResourceManager::Get();
    }

    bool ResourceSubsystem::IsOnUpdateThread() const
    {
        return m_updateThreadId == std::thread::id{} ||
               m_updateThreadId == std::this_thread::get_id();
    }

    bool ResourceSubsystem::RequireUpdateThread(const char* operation)
    {
        if (IsOnUpdateThread())
            return true;
        ++m_renderStats.wrongThreadMutations;
        RVX_VERIFY(false,
                   "ResourceSubsystem operation '{}' must run on its registered update thread",
                   operation == nullptr ? "unknown" : operation);
        return false;
    }

    void ResourceSubsystem::HandleLifecycleEvent(
        const ResourceLifecycleEvent& event)
    {
        if (!RequireUpdateThread("HandleLifecycleEvent"))
            return;

        switch (event.type)
        {
            case ResourceLifecycleEventType::Ready:
                ++m_renderStats.readyEvents;
                QueueReadyResource(
                    event,
                    RenderResourceContentOperation::Create);
                break;
            case ResourceLifecycleEventType::Reloaded:
                ++m_renderStats.reloadEvents;
                QueueReadyResource(
                    event,
                    RenderResourceContentOperation::Replace);
                break;
            case ResourceLifecycleEventType::BeforeUnload:
                ++m_renderStats.unloadEvents;
                if (event.resource &&
                    event.resource->GetType() == ResourceType::Model)
                {
                    auto* model = static_cast<ModelResource*>(
                        event.resource.Get());
                    model->CancelTextureStreaming();
                    static_cast<void>(
                        ResourceManager::Get().CancelModelTextureStreaming(
                            event.resourceId));
                    std::erase_if(
                        m_streamingModels,
                        [id = event.resourceId](
                            const ResourceHandle<ModelResource>& value)
                        {
                            return !value || value.GetId() == id;
                        });
                }
                if (event.closureRetirementToken.IsValid())
                {
                    // The pre-commit closure callback captured this exact
                    // render generation and admitted RequestRelease before
                    // ResourceCache erased the CPU resource. Preserve that
                    // single ownership path; a legacy ReleaseAsset here would
                    // either double-release or resolve a reused slot.
                    RemovePendingAsset(AssetId{event.resourceId});
                    break;
                }
                ReleaseAsset(AssetId{event.resourceId});
                break;
        }
    }

    void ResourceSubsystem::QueueReadyResource(
        const ResourceLifecycleEvent& event,
        RenderResourceContentOperation operation)
    {
        if (m_renderShuttingDown || !event.resource ||
            event.resourceId == InvalidResourceId)
        {
            return;
        }

        static_cast<void>(QueueRenderResourceTree(event.resource, operation));
    }

    bool ResourceSubsystem::QueueRenderResourceTree(
        ResourceHandle<IResource> rootResource,
        RenderResourceContentOperation operation)
    {
        if (!rootResource ||
            rootResource.GetId() == InvalidResourceId)
        {
            return false;
        }

        const ResourceId rootResourceId = rootResource.GetId();

        const uint64 sourceRevision = m_nextSourceRevision++;
        std::vector<ResourceHandle<IResource>> resourcesToQueue{
            std::move(rootResource)};
        std::unordered_set<ResourceId> visited;
        size_t nextResource = 0;
        bool renderResourceFound = false;
        while (nextResource < resourcesToQueue.size())
        {
            ResourceHandle<IResource> resource =
                resourcesToQueue[nextResource++];
            if (!resource || resource.GetId() == InvalidResourceId ||
                !visited.insert(resource.GetId()).second)
            {
                continue;
            }

            for (ResourceId dependencyId : resource->GetAllDependencies())
            {
                if (dependencyId == InvalidResourceId ||
                    visited.contains(dependencyId))
                {
                    continue;
                }
                if (IResource* dependency =
                        ResourceManager::Get().GetCache().Get(dependencyId))
                {
                    resourcesToQueue.emplace_back(dependency);
                }
            }

            const RenderResourceKind kind =
                ToRenderResourceKind(resource->GetType());
            if (kind == RenderResourceKind::Invalid)
                continue;
            renderResourceFound = true;

            // A Reloaded lifecycle event belongs only to its root resource.
            // Dependencies are ensured/create-if-missing; replacing them here
            // would reinterpret a material/model revision as texture/mesh
            // content changes and duplicate their own lifecycle events.
            const RenderResourceContentOperation resourceOperation =
                resource.GetId() == rootResourceId
                    ? operation
                    : RenderResourceContentOperation::Create;

            const AssetId assetId{resource.GetId()};
            const auto tracked = m_trackedResources.find(assetId);
            if (tracked != m_trackedResources.end())
            {
                if (tracked->second.kind != kind)
                {
                    ++m_renderStats.gatewayRejections;
                    continue;
                }
                if (resourceOperation !=
                    RenderResourceContentOperation::Replace)
                    continue;
            }
            const auto pending = std::find_if(
                m_pendingResources.begin(),
                m_pendingResources.end(),
                [assetId](const PendingResource& value)
                {
                    return value.assetId == assetId;
                });
            if (pending != m_pendingResources.end())
            {
                if (resourceOperation ==
                    RenderResourceContentOperation::Replace)
                {
                    pending->resource = resource;
                    pending->sourceRevision = sourceRevision;
                    if (pending->operation !=
                        RenderResourceContentOperation::Create)
                    {
                        pending->operation =
                            RenderResourceContentOperation::Replace;
                    }
                    if (HigherPriority(GetUploadPriority(resource->GetType()),
                                       pending->priority))
                    {
                        pending->priority =
                            GetUploadPriority(resource->GetType());
                    }
                    ++m_renderStats.replacementsCoalesced;
                }
                continue;
            }

            const RenderResourceContentOperation effectiveOperation =
                tracked == m_trackedResources.end()
                    ? RenderResourceContentOperation::Create
                    : resourceOperation;
            m_pendingResources.push_back(PendingResource{
                assetId,
                kind,
                resource,
                GetUploadPriority(resource->GetType()),
                m_nextFifoOrder++,
                sourceRevision,
                effectiveOperation});
            if (effectiveOperation ==
                RenderResourceContentOperation::Replace)
            {
                ++m_renderStats.replacementsQueued;
            }
        }
        return renderResourceFound;
    }

    void ResourceSubsystem::TrackModelTextureStreaming(
        ResourceHandle<ModelResource> model)
    {
        if (!model ||
            !model->GetTextureStreamingSnapshot().HasStreamingTextures())
        {
            return;
        }
        const auto existing = std::find_if(
            m_streamingModels.begin(),
            m_streamingModels.end(),
            [id = model.GetId()](const ResourceHandle<ModelResource>& value)
            {
                return value && value.GetId() == id;
            });
        if (existing == m_streamingModels.end())
            m_streamingModels.push_back(std::move(model));
    }

    void ResourceSubsystem::FailModelTextureStreamingForAsset(
        AssetId assetId,
        const std::string& reason)
    {
        for (const ResourceHandle<ModelResource>& model : m_streamingModels)
        {
            if (!model)
                continue;
            const std::vector<ResourceHandle<TextureResource>> textures =
                model->GetStreamingTextures();
            const bool referencesAsset = std::any_of(
                textures.begin(),
                textures.end(),
                [assetId](const ResourceHandle<TextureResource>& texture)
                {
                    return texture && texture.GetId() == assetId.value;
                });
            if (referencesAsset)
            {
                if (!ResourceManager::Get()
                         .CompleteModelTexturePublication(assetId.value,
                                                          false,
                                                          reason))
                {
                    model->MarkTextureStreamingFailed(reason);
                }
            }
        }
    }

    void ResourceSubsystem::RetireModelTextureStreamingRequest(
        const RenderResourceHandle& handle)
    {
        auto pending = m_pendingModelTextureRetirements.begin();
        while (pending != m_pendingModelTextureRetirements.end())
        {
            pending->second.erase(handle);
            if (!pending->second.empty())
            {
                ++pending;
                continue;
            }

            m_retainedModelTextureLeases.erase(pending->first);
            pending = m_pendingModelTextureRetirements.erase(pending);
        }
    }

    void ResourceSubsystem::ProcessModelTextureStreaming()
    {
        if (m_gateway == nullptr)
            return;

        auto isReady = [this](AssetId assetId,
                              RenderResourceKind kind,
                              bool requireNoReplacement,
                              bool& failed)
        {
            const RenderResourceResolveResult resolved =
                ResolveRenderResource(assetId, kind);
            if (resolved.code == RenderResourceResolveCode::NotFound)
                return false;
            if (resolved.code != RenderResourceResolveCode::Resolved ||
                resolved.status.code != RenderResourceStatusCode::Current ||
                resolved.status.state == RenderResourcePublicState::Failed ||
                resolved.status.state == RenderResourcePublicState::Released ||
                (resolved.status.failure != RenderResourceFailureCode::None &&
                 resolved.status.replacementState ==
                     RenderResourceReplacementState::None))
            {
                failed = true;
                return false;
            }
            return resolved.status.state == RenderResourcePublicState::GPUReady &&
                   (!requireNoReplacement ||
                    resolved.status.replacementState ==
                        RenderResourceReplacementState::None);
        };
        auto hasLocalPublication = [this](AssetId assetId)
        {
            const bool pendingResource = std::any_of(
                m_pendingResources.begin(),
                m_pendingResources.end(),
                [assetId](const PendingResource& value)
                {
                    return value.assetId == assetId;
                });
            const bool pendingUpload = std::any_of(
                m_pendingUploads.begin(),
                m_pendingUploads.end(),
                [assetId](const PendingUpload& value)
                {
                    return value.assetId == assetId;
                });
            const bool retained = std::any_of(
                m_retainedRequests.begin(),
                m_retainedRequests.end(),
                [assetId](const auto& value)
                {
                    return value.second.assetId == assetId;
                });
            return pendingResource || pendingUpload || retained;
        };

        ResourceManager& manager = ResourceManager::Get();
        const std::vector<ResourceHandle<TextureResource>> publications =
            manager.GetPendingModelTexturePublications();
        for (const ResourceHandle<TextureResource>& texture : publications)
        {
            if (!texture)
                continue;
            const AssetId textureAsset{texture.GetId()};
            if (hasLocalPublication(textureAsset))
                continue;

            const RenderResourceResolveResult resolved =
                ResolveRenderResource(textureAsset,
                                      RenderResourceKind::Texture);
            if (resolved.code == RenderResourceResolveCode::NotFound)
            {
                static_cast<void>(manager.CompleteModelTexturePublication(
                    texture.GetId(),
                    false,
                    "A streamed texture publication lost its render resource"));
                continue;
            }

            bool failed = false;
            const bool ready = isReady(textureAsset,
                                       RenderResourceKind::Texture,
                                       true,
                                       failed);
            if (failed)
            {
                static_cast<void>(manager.CompleteModelTexturePublication(
                    texture.GetId(),
                    false,
                    "A streamed texture replacement failed on Render"));
            }
            else if (ready)
            {
                static_cast<void>(manager.CompleteModelTexturePublication(
                    texture.GetId(), true));
            }
        }

        // GPU readiness establishes only that fallback geometry/materials can
        // be drawn. It does not establish that a consumer has ever seen that
        // fallback. SceneAssetLoadCoordinator supplies the separate
        // applied-and-presented proof before it calls BeginModelTextureStreaming.
        std::erase_if(
            m_streamingModels,
            [](const ResourceHandle<ModelResource>& model)
            {
                if (!model)
                    return true;
                const ModelTextureStreamingStage stage =
                    model->GetTextureStreamingSnapshot().stage;
                return stage == ModelTextureStreamingStage::Failed ||
                       stage == ModelTextureStreamingStage::Cancelled ||
                       stage == ModelTextureStreamingStage::FullyResident ||
                       stage == ModelTextureStreamingStage::None;
            });
    }

    void ResourceSubsystem::ProcessPendingResources()
    {
        std::stable_sort(
            m_pendingResources.begin(),
            m_pendingResources.end(),
            [](const PendingResource& left, const PendingResource& right)
            {
                if (left.priority != right.priority)
                    return HigherPriority(left.priority, right.priority);
                return left.fifoOrder < right.fifoOrder;
            });

        size_t index = 0;
        while (index < m_pendingResources.size())
        {
            if (TryBuildPendingResource(m_pendingResources[index]))
            {
                m_pendingResources.erase(m_pendingResources.begin() + index);
            }
            else
            {
                ++index;
            }
        }
    }

    void ResourceSubsystem::ProcessPendingUploads()
    {
        if (m_gateway == nullptr)
            return;

        std::stable_sort(
            m_pendingUploads.begin(),
            m_pendingUploads.end(),
            [](const PendingUpload& left, const PendingUpload& right)
            {
                if (left.priority != right.priority)
                    return HigherPriority(left.priority, right.priority);
                return left.fifoOrder < right.fifoOrder;
            });

        size_t index = 0;
        while (index < m_pendingUploads.size())
        {
            PendingUpload& pending = m_pendingUploads[index];
            const RenderUploadEnqueueResult result =
                m_gateway->TryEnqueueUpload(pending.request);
            if (result.code == RenderUploadEnqueueCode::Accepted)
            {
                // This instant is emitted only after the narrow render gateway
                // has accepted ownership of the immutable request.  It is not
                // inferred from later status polling.
                const ResourceUploadRequestRef acceptedRequest = pending.request;
                Diagnostics::RecordTraceInstant(
                    m_config.startupTraceContext,
                    "UploadQueued",
                    {{"assetId", pending.assetId.value},
                     {"requestSequence", acceptedRequest->GetSequence()},
                     {"sourceRevision", acceptedRequest->GetSourceRevision()},
                     {"bytes", acceptedRequest->GetDeclaredPayloadBytes()},
                     {"kind", static_cast<uint64>(acceptedRequest->GetKind())}});
                m_retainedRequests.emplace(
                    pending.handle,
                    RetainedRequest{pending.assetId,
                                    pending.handle,
                                    std::move(pending.request)});
                m_pendingUploads.erase(m_pendingUploads.begin() + index);
                ++m_renderStats.enqueueAccepted;
                continue;
            }
            if (result.code == RenderUploadEnqueueCode::QueueFullByCount ||
                result.code == RenderUploadEnqueueCode::QueueFullByBytes)
            {
                if (result.code == RenderUploadEnqueueCode::QueueFullByCount)
                    ++m_renderStats.queueFullByCount;
                else
                    ++m_renderStats.queueFullByBytes;
                ++m_renderStats.retryAttempts;
                break;
            }

            ++m_renderStats.gatewayRejections;
            const RenderResourceStatus status =
                m_gateway->QueryResourceStatus(pending.handle);
            const bool replacement = pending.request != nullptr &&
                pending.request->GetOperation() ==
                    RenderResourceContentOperation::Replace;
            if (replacement)
            {
                ++m_renderStats.replacementFailures;
                FailModelTextureStreamingForAsset(
                    pending.assetId,
                    "A streamed texture replacement was rejected by the render gateway");
                if (status.code != RenderResourceStatusCode::Current ||
                    status.state == RenderResourcePublicState::Failed ||
                    status.state == RenderResourcePublicState::Released)
                {
                    const auto tracked =
                        m_trackedResources.find(pending.assetId);
                    if (tracked != m_trackedResources.end() &&
                        tracked->second.handle == pending.handle)
                    {
                        m_trackedResources.erase(tracked);
                    }
                }
                m_localTerminalRequests.push_back(
                    std::move(pending.request));
                m_pendingUploads.erase(m_pendingUploads.begin() + index);
                continue;
            }
            bool waitForTerminal = IsTerminalStatus(status) ||
                                   status.state ==
                                       RenderResourcePublicState::Evicting;
            if (!waitForTerminal)
            {
                const RenderReleaseResult release =
                    m_gateway->RequestRelease(pending.handle);
                waitForTerminal =
                    release.code == RenderReleaseCode::Accepted ||
                    release.code == RenderReleaseCode::AlreadyPending;
                if (release.code == RenderReleaseCode::Accepted)
                    ++m_renderStats.releasesAccepted;
            }

            if (waitForTerminal)
            {
                m_retainedRequests.emplace(
                    pending.handle,
                    RetainedRequest{pending.assetId,
                                    pending.handle,
                                    std::move(pending.request)});
            }
            else
            {
                m_localTerminalRequests.push_back(std::move(pending.request));
            }
            const auto tracked = m_trackedResources.find(pending.assetId);
            if (tracked != m_trackedResources.end() &&
                tracked->second.handle == pending.handle)
            {
                m_trackedResources.erase(tracked);
            }
            m_pendingUploads.erase(m_pendingUploads.begin() + index);
        }
    }

    bool ResourceSubsystem::TryBuildPendingResource(PendingResource& pending)
    {
        if (m_gateway == nullptr || !pending.resource)
            return pending.resource == nullptr;

        RenderResourceReserveResult reserve;
        const auto tracked = m_trackedResources.find(pending.assetId);
        if (tracked == m_trackedResources.end())
        {
            reserve = m_gateway->ReserveResource(pending.assetId,
                                                 pending.kind);
            if (reserve.code == RenderResourceReserveCode::CapacityExceeded)
            {
                ++m_renderStats.retryAttempts;
                return false;
            }
            if (reserve.code != RenderResourceReserveCode::Reserved &&
                reserve.code != RenderResourceReserveCode::Existing)
            {
                ++m_renderStats.gatewayRejections;
                return true;
            }
            if (reserve.code == RenderResourceReserveCode::Reserved)
                ++m_renderStats.reservations;
            else
                ++m_renderStats.existingReservations;
            m_trackedResources[pending.assetId] =
                TrackedResource{pending.kind, reserve.handle};
            pending.operation = RenderResourceContentOperation::Create;
        }
        else
        {
            if (tracked->second.kind != pending.kind)
            {
                ++m_renderStats.gatewayRejections;
                return true;
            }
            reserve.handle = tracked->second.handle;
            reserve.status = m_gateway->QueryResourceStatus(reserve.handle);
            reserve.code = RenderResourceReserveCode::Existing;
        }

        const RenderResourceStatus status =
            m_gateway->QueryResourceStatus(reserve.handle);
        if (status.code != RenderResourceStatusCode::Current)
        {
            m_trackedResources.erase(pending.assetId);
            pending.operation = RenderResourceContentOperation::Create;
            ++m_renderStats.gatewayRejections;
            return false;
        }
        if (pending.operation == RenderResourceContentOperation::Replace)
        {
            if (status.state != RenderResourcePublicState::GPUReady ||
                status.replacementState !=
                    RenderResourceReplacementState::None)
            {
                return false;
            }
        }
        else if (status.state != RenderResourcePublicState::Reserved)
        {
            return status.state == RenderResourcePublicState::GPUReady;
        }

        const auto queued = std::find_if(
            m_pendingUploads.begin(),
            m_pendingUploads.end(),
            [handle = reserve.handle](const PendingUpload& upload)
            {
                return upload.handle == handle;
            });
        if (queued != m_pendingUploads.end() ||
            m_retainedRequests.find(reserve.handle) !=
                m_retainedRequests.end())
        {
            return true;
        }

        const RenderUploadRequestBuildResult build =
            RenderUploadRequestBuilder::Build(
                *pending.resource,
                reserve.handle,
                m_nextRequestSequence++,
                [this](AssetId assetId, RenderResourceKind kind)
                {
                    return ResolveDependency(assetId, kind);
                },
                pending.priority,
                pending.sourceRevision,
                pending.operation);
        if (build.code ==
            RenderUploadRequestBuildCode::DependencyUnavailable)
        {
            ++m_renderStats.retryAttempts;
            return false;
        }
        if (build.code != RenderUploadRequestBuildCode::Built ||
            !build.request)
        {
            ++m_renderStats.buildFailures;
            if (pending.operation ==
                RenderResourceContentOperation::Replace)
            {
                ++m_renderStats.replacementFailures;
                FailModelTextureStreamingForAsset(
                    pending.assetId,
                    "A streamed texture replacement request could not be built");
                return true;
            }
            const RenderReleaseResult release =
                m_gateway->RequestRelease(reserve.handle);
            if (release.code == RenderReleaseCode::Accepted)
                ++m_renderStats.releasesAccepted;
            m_trackedResources.erase(pending.assetId);
            return true;
        }

        m_pendingUploads.push_back(PendingUpload{
            pending.assetId,
            pending.kind,
            reserve.handle,
            build.request,
            pending.priority,
            pending.fifoOrder});
        return true;
    }

    RenderResourceHandle ResourceSubsystem::ResolveDependency(
        AssetId assetId,
        RenderResourceKind kind) const
    {
        const auto tracked = m_trackedResources.find(assetId);
        if (tracked == m_trackedResources.end() ||
            tracked->second.kind != kind)
        {
            return {};
        }
        const RenderResourceStatus status =
            m_gateway->QueryResourceStatus(tracked->second.handle);
        // Composite uploads are submitted only after their dependencies are
        // usable by Render. Enqueuing a material beside its texture uploads
        // would otherwise turn ordinary asynchronous ordering into a terminal
        // DependencyUnavailable failure on the Render Thread.
        return status.code == RenderResourceStatusCode::Current &&
                       status.state == RenderResourcePublicState::GPUReady
                   ? tracked->second.handle
                   : RenderResourceHandle{};
    }

    void ResourceSubsystem::ReleaseAsset(AssetId assetId)
    {
        RemovePendingAsset(assetId);
        const auto tracked = m_trackedResources.find(assetId);
        if (tracked == m_trackedResources.end())
            return;
        if (m_gateway == nullptr)
        {
            m_trackedResources.erase(tracked);
            return;
        }

        const RenderReleaseResult release =
            m_gateway->RequestRelease(tracked->second.handle);
        if (release.code == RenderReleaseCode::Accepted)
        {
            ++m_renderStats.releasesAccepted;
            m_trackedResources.erase(tracked);
        }
        else if (release.code == RenderReleaseCode::AlreadyPending ||
                 release.code == RenderReleaseCode::StaleGeneration)
        {
            m_trackedResources.erase(tracked);
        }
        else
        {
            ++m_renderStats.gatewayRejections;
        }
    }

    void ResourceSubsystem::RemovePendingAsset(AssetId assetId)
    {
        std::erase_if(m_pendingResources,
                      [assetId](const PendingResource& pending)
                      {
                          return pending.assetId == assetId;
                      });
        auto upload = m_pendingUploads.begin();
        while (upload != m_pendingUploads.end())
        {
            if (upload->assetId != assetId)
            {
                ++upload;
                continue;
            }
            if (upload->request)
            {
                m_localTerminalRequests.push_back(
                    std::move(upload->request));
            }
            upload = m_pendingUploads.erase(upload);
        }
    }

    RenderResourceKind ResourceSubsystem::ToRenderResourceKind(
        ResourceType type)
    {
        switch (type)
        {
            case ResourceType::Mesh: return RenderResourceKind::Mesh;
            case ResourceType::Texture: return RenderResourceKind::Texture;
            case ResourceType::Material: return RenderResourceKind::Material;
            default: return RenderResourceKind::Invalid;
        }
    }

    RenderUploadPriority ResourceSubsystem::GetUploadPriority(
        ResourceType type)
    {
        return type == ResourceType::Texture ? RenderUploadPriority::High
                                             : RenderUploadPriority::Normal;
    }
} // namespace RVX::Resource
