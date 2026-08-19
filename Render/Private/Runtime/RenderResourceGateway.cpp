#include "Runtime/RenderResourceGateway.h"

#include "Core/Assert.h"

#include <stdexcept>

namespace RVX
{
    RenderResourceGateway::RenderResourceGateway(
        const RenderTransportConfig& config,
        WakeFunction wakeFunction,
        void* wakeContext,
        RuntimeFatalFunction runtimeFatalFunction,
        void* runtimeFatalContext,
        BeforeMutationFunction beforeMutationFunction,
        void* beforeMutationContext)
        : m_statusTable(ValidateConfigAndGetStatusCapacity(config)),
          m_reservationDirectory(m_statusTable),
          m_uploadQueue(config,
                        m_statusTable,
                        wakeFunction,
                        wakeContext),
          m_releaseQueue(m_reservationDirectory,
                         m_statusTable,
                         config.statusSlotCapacity,
                         wakeFunction,
                         wakeContext,
                         runtimeFatalFunction,
                         runtimeFatalContext),
          m_reservationIdentities(config.statusSlotCapacity),
          m_replacementPublications(config.statusSlotCapacity),
          m_beforeMutationFunction(beforeMutationFunction),
          m_beforeMutationContext(beforeMutationContext)
    {
    }

    RenderResourceReserveResult RenderResourceGateway::ReserveResource(
        AssetId assetId,
        RenderResourceKind kind) noexcept
    {
        if (m_shuttingDown.load(std::memory_order_acquire))
        {
            RenderResourceReserveResult result;
            result.code = RenderResourceReserveCode::ShuttingDown;
            return result;
        }
        if (m_beforeMutationFunction != nullptr)
        {
            m_beforeMutationFunction(
                m_beforeMutationContext,
                RenderGatewayPublicationPath::Reserve);
        }
        RenderResourceReserveResult result =
            m_reservationDirectory.ReserveResource(assetId, kind);
        if (result.handle.IsValid() &&
            result.handle.slot < m_reservationIdentities.size())
        {
            m_reservationIdentities[result.handle.slot] =
                ReservationIdentity{assetId,
                                    kind,
                                    result.handle.generation};
            // ReservationDirectory owns the raw status table. Public callers
            // must never observe its replacement-only transport states.
            result.status = QueryResourceStatus(result.handle);
        }
        return result;
    }

    RenderUploadEnqueueResult RenderResourceGateway::TryEnqueueUpload(
        const ResourceUploadRequestRef& request) noexcept
    {
        return TryEnqueueUploadObserved(request).result;
    }

    RenderGatewayUploadEnqueueResult
        RenderResourceGateway::TryEnqueueUploadObserved(
            const ResourceUploadRequestRef& request) noexcept
    {
        RenderGatewayUploadEnqueueResult observed;
        if (m_shuttingDown.load(std::memory_order_acquire))
        {
            observed.result.code = RenderUploadEnqueueCode::ShuttingDown;
            return observed;
        }
        if (m_beforeMutationFunction != nullptr)
        {
            m_beforeMutationFunction(
                m_beforeMutationContext,
                RenderGatewayPublicationPath::Upload);
        }
        if (request != nullptr && request->GetHandle().IsValid())
        {
            const RenderResourceHandle handle = request->GetHandle();
            const RenderResourceStatus status = m_statusTable.Query(handle);
            if (status.code == RenderResourceStatusCode::Current &&
                handle.slot < m_reservationIdentities.size())
            {
                const ReservationIdentity& identity =
                    m_reservationIdentities[handle.slot];
                if (identity.generation != handle.generation ||
                    identity.assetId != request->GetAssetId() ||
                    identity.kind != request->GetKind())
                {
                    observed.result.code =
                        RenderUploadEnqueueCode::InvalidRequest;
                    return observed;
                }
            }
        }
        if (request != nullptr)
        {
            // Keep the revision ledger locked across the state-table mutation
            // performed by TryEnqueue. This makes the public status projection
            // atomic and seeds the generation high-water mark from Create.
            {
                std::lock_guard lock(m_replacementMutex);
                if (request->GetOperation() ==
                        RenderResourceContentOperation::Replace &&
                    !CanAcceptReplacementLocked(*request))
                {
                    observed.result.code =
                        RenderUploadEnqueueCode::InvalidRequest;
                    return observed;
                }
                observed.result = m_uploadQueue.TryEnqueue(
                    request,
                    &observed.queue,
                    false);
                if (observed.result.code ==
                    RenderUploadEnqueueCode::Accepted)
                {
                    RecordSourceRevisionLocked(*request);
                }
            }
            if (observed.result.code == RenderUploadEnqueueCode::Accepted)
            {
                // Never invoke the external wake callback while holding the
                // revision projection mutex; callbacks may query the gateway.
                m_uploadQueue.NotifyConsumer();
            }
        }
        else
        {
            observed.result = m_uploadQueue.TryEnqueue(request, &observed.queue);
        }
        return observed;
    }

    RenderReleaseResult RenderResourceGateway::RequestRelease(
        RenderResourceHandle handle) noexcept
    {
        return RequestReleaseObserved(handle).result;
    }

    RenderGatewayReleaseResult RenderResourceGateway::RequestReleaseObserved(
        RenderResourceHandle handle) noexcept
    {
        RenderGatewayReleaseResult observed;
        if (m_shuttingDown.load(std::memory_order_acquire))
        {
            observed.result.code = RenderReleaseCode::ShuttingDown;
            return observed;
        }
        if (m_beforeMutationFunction != nullptr)
        {
            m_beforeMutationFunction(
                m_beforeMutationContext,
                RenderGatewayPublicationPath::Release);
        }
        observed.result =
            m_releaseQueue.RequestRelease(handle, &observed.queue);
        if (observed.result.code == RenderReleaseCode::Accepted)
        {
            ForgetReplacementPublication(handle);
        }
        return observed;
    }

    RenderResourceStatus RenderResourceGateway::QueryResourceStatus(
        RenderResourceHandle handle) const noexcept
    {
        // Serialize the state snapshot with replacement enqueue/release ledger
        // updates. Query remains side-effect free.
        std::lock_guard lock(m_replacementMutex);
        return ProjectPublicStatusLocked(handle, m_statusTable.Query(handle));
    }

    RenderResourceStatus RenderResourceGateway::ProjectPublicStatusLocked(
        RenderResourceHandle handle,
        RenderResourceStatus status) const noexcept
    {
        if (status.code != RenderResourceStatusCode::Current)
        {
            return status;
        }

        uint64 pendingSourceRevision = 0;
        if (handle.IsValid() &&
            handle.slot < m_replacementPublications.size())
        {
            const ReplacementPublication& publication =
                m_replacementPublications[handle.slot];
            if (publication.generation == handle.generation)
            {
                pendingSourceRevision = publication.pendingSourceRevision;
            }
        }

        // Replacement queueing/uploading is an internal transport concern.
        // Update-thread consumers must keep rendering the last committed
        // content and make readiness decisions from its public GPUReady state.
        if (status.state == RenderResourcePublicState::ReplacementQueued)
        {
            status.state = RenderResourcePublicState::GPUReady;
            status.replacementState =
                RenderResourceReplacementState::Queued;
            status.pendingSourceRevision = pendingSourceRevision;
        }
        else if (status.state == RenderResourcePublicState::Replacing)
        {
            status.state = RenderResourcePublicState::GPUReady;
            status.replacementState =
                RenderResourceReplacementState::Uploading;
            status.pendingSourceRevision = pendingSourceRevision;
        }
        return status;
    }

    void RenderResourceGateway::BeginShutdown() noexcept
    {
        m_shuttingDown.store(true, std::memory_order_release);
    }

    uint32 RenderResourceGateway::CancelPendingUploadsOnRenderThread(
        RenderResourceFailureCode failure) noexcept
    {
        uint32 cancelled = 0;
        while (ResourceUploadRequestRef request = m_uploadQueue.TryDequeue())
        {
            const RenderResourceHandle handle = request->GetHandle();
            const bool replacement = request->GetOperation() ==
                                     RenderResourceContentOperation::Replace;
            request.reset();

            const RenderResourceStatus queued = m_statusTable.Query(handle);
            if (queued.code != RenderResourceStatusCode::Current ||
                queued.state != (replacement
                                     ? RenderResourcePublicState::ReplacementQueued
                                     : RenderResourcePublicState::UploadQueued))
            {
                continue;
            }
            const RenderResourcePublicState queuedState = replacement
                ? RenderResourcePublicState::ReplacementQueued
                : RenderResourcePublicState::UploadQueued;
            const RenderResourcePublicState uploadingState = replacement
                ? RenderResourcePublicState::Replacing
                : RenderResourcePublicState::Uploading;
            if (!m_statusTable.CompareExchange(
                    handle,
                    PackedRenderResourceStatus{
                        handle.generation,
                        queuedState,
                        queued.failure},
                    PackedRenderResourceStatus{
                        handle.generation,
                        uploadingState,
                        RenderResourceFailureCode::None},
                    RenderStatusWriter::Render))
            {
                continue;
            }
            if (m_statusTable.CompareExchange(
                    handle,
                    PackedRenderResourceStatus{
                        handle.generation,
                        uploadingState,
                        RenderResourceFailureCode::None},
                    PackedRenderResourceStatus{
                        handle.generation,
                        replacement &&
                                failure != RenderResourceFailureCode::DeviceLost
                            ? RenderResourcePublicState::GPUReady
                            : RenderResourcePublicState::Failed,
                        failure},
                    RenderStatusWriter::Render))
            {
                ++cancelled;
            }
        }
        return cancelled;
    }

    ResourceUploadRequestRef RenderResourceGateway::TryDequeueUpload() noexcept
    {
        return m_uploadQueue.TryDequeue();
    }

    RenderResourceHandle RenderResourceGateway::TryDequeueRelease() noexcept
    {
        return m_releaseQueue.TryDequeue();
    }

    RenderUploadQueueSnapshot
        RenderResourceGateway::GetUploadQueueSnapshot() const noexcept
    {
        return m_uploadQueue.GetSnapshot();
    }

    RenderReleaseQueueSnapshot
        RenderResourceGateway::GetReleaseQueueSnapshot() const noexcept
    {
        return m_releaseQueue.GetSnapshot();
    }

    uint32 RenderResourceGateway::GetRetainedUploadCount() const noexcept
    {
        return m_uploadQueue.GetRetainedCount();
    }

    uint64 RenderResourceGateway::GetRetainedUploadBytes() const noexcept
    {
        return m_uploadQueue.GetRetainedBytes();
    }

    RenderResourceStatusTable& RenderResourceGateway::GetStatusTable() noexcept
    {
        return m_statusTable;
    }

    const RenderResourceStatusTable& RenderResourceGateway::GetStatusTable()
        const noexcept
    {
        return m_statusTable;
    }

    bool RenderResourceGateway::CanAcceptReplacementLocked(
        const ResourceUploadRequest& request) const noexcept
    {
        const RenderResourceHandle handle = request.GetHandle();
        if (!handle.IsValid() ||
            request.GetSourceRevision() == 0 ||
            handle.slot >= m_replacementPublications.size())
        {
            return false;
        }
        const ReplacementPublication& publication =
            m_replacementPublications[handle.slot];
        return publication.generation != handle.generation ||
               request.GetSourceRevision() >
                   publication.lastAcceptedSourceRevision;
    }

    void RenderResourceGateway::RecordSourceRevisionLocked(
        const ResourceUploadRequest& request) noexcept
    {
        const RenderResourceHandle handle = request.GetHandle();
        const bool replacement = request.GetOperation() ==
                                 RenderResourceContentOperation::Replace;
        RVX_ASSERT_MSG(handle.IsValid() &&
                           (!replacement || request.GetSourceRevision() != 0) &&
                           handle.slot < m_replacementPublications.size(),
                       "Resource publication requires a valid handle and revision contract");
        ReplacementPublication& publication =
            m_replacementPublications[handle.slot];
        if (replacement)
        {
            RVX_ASSERT_MSG(publication.generation != handle.generation ||
                               request.GetSourceRevision() >
                                   publication.lastAcceptedSourceRevision,
                           "Replacement source revision must strictly increase");
            publication = ReplacementPublication{
                handle.generation,
                request.GetSourceRevision(),
                request.GetSourceRevision()};
            return;
        }

        // Create establishes the baseline revision for this generation. Zero
        // remains valid for compatibility builders, but a nonzero baseline
        // prevents a later replacement from moving content backwards.
        publication = ReplacementPublication{
            handle.generation,
            request.GetSourceRevision(),
            0};
    }

    void RenderResourceGateway::ForgetReplacementPublication(
        RenderResourceHandle handle) const noexcept
    {
        if (!handle.IsValid() ||
            handle.slot >= m_replacementPublications.size())
        {
            return;
        }
        std::lock_guard lock(m_replacementMutex);
        ReplacementPublication& publication =
            m_replacementPublications[handle.slot];
        if (publication.generation == handle.generation)
        {
            publication = {};
        }
    }

    uint32 RenderResourceGateway::ValidateConfigAndGetStatusCapacity(
        const RenderTransportConfig& config)
    {
        if (!config.IsValid())
        {
            throw std::invalid_argument(
                "Render transport configuration is invalid");
        }
        return config.statusSlotCapacity;
    }
} // namespace RVX
