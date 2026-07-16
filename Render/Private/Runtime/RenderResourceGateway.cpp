#include "Runtime/RenderResourceGateway.h"

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
        observed.result = m_uploadQueue.TryEnqueue(request, &observed.queue);
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
        return observed;
    }

    RenderResourceStatus RenderResourceGateway::QueryResourceStatus(
        RenderResourceHandle handle) const noexcept
    {
        return m_statusTable.Query(handle);
    }

    void RenderResourceGateway::BeginShutdown() noexcept
    {
        m_shuttingDown.store(true, std::memory_order_release);
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
