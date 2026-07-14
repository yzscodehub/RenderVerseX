#include "Runtime/RenderResourceGateway.h"

#include <stdexcept>

namespace RVX
{
    RenderResourceGateway::RenderResourceGateway(
        const RenderTransportConfig& config,
        WakeFunction wakeFunction,
        void* wakeContext,
        RuntimeFatalFunction runtimeFatalFunction,
        void* runtimeFatalContext)
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
          m_reservationIdentities(config.statusSlotCapacity)
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
        if (m_shuttingDown.load(std::memory_order_acquire))
        {
            return RenderUploadEnqueueResult{
                RenderUploadEnqueueCode::ShuttingDown};
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
                    return RenderUploadEnqueueResult{
                        RenderUploadEnqueueCode::InvalidRequest};
                }
            }
        }
        return m_uploadQueue.TryEnqueue(request);
    }

    RenderReleaseResult RenderResourceGateway::RequestRelease(
        RenderResourceHandle handle) noexcept
    {
        if (m_shuttingDown.load(std::memory_order_acquire))
        {
            return RenderReleaseResult{RenderReleaseCode::ShuttingDown};
        }
        return m_releaseQueue.RequestRelease(handle);
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
