#pragma once

/**
 * @file RenderResourceGateway.h
 * @brief Update-side gateway composition for fixed status and bounded queues.
 */

#include "Render/RenderTransportTypes.h"
#include "RenderContracts/IRenderResourceGateway.h"
#include "Runtime/RenderReleaseQueue.h"
#include "Runtime/RenderResourceReservationDirectory.h"
#include "Runtime/RenderResourceStatusTable.h"
#include "Runtime/RenderUploadQueue.h"

#include <atomic>
#include <vector>

namespace RVX
{
    struct RenderGatewayUploadEnqueueResult
    {
        RenderUploadEnqueueResult result{};
        RenderUploadQueueSnapshot queue{};
    };

    struct RenderGatewayReleaseResult
    {
        RenderReleaseResult result{};
        RenderReleaseQueueSnapshot queue{};
    };

    class RenderResourceGateway final : public IRenderResourceGateway
    {
    public:
        using WakeFunction = void (*)(void*) noexcept;
        using RuntimeFatalFunction = void (*)(void*, const char*) noexcept;

        explicit RenderResourceGateway(
            const RenderTransportConfig& config,
            WakeFunction wakeFunction = nullptr,
            void* wakeContext = nullptr,
            RuntimeFatalFunction runtimeFatalFunction = nullptr,
            void* runtimeFatalContext = nullptr);
        ~RenderResourceGateway() override = default;

        RenderResourceGateway(const RenderResourceGateway&) = delete;
        RenderResourceGateway& operator=(const RenderResourceGateway&) = delete;

        RenderResourceReserveResult ReserveResource(
            AssetId assetId,
            RenderResourceKind kind) noexcept override;
        RenderUploadEnqueueResult TryEnqueueUpload(
            const ResourceUploadRequestRef& request) noexcept override;
        RenderReleaseResult RequestRelease(
            RenderResourceHandle handle) noexcept override;
        [[nodiscard]] RenderResourceStatus QueryResourceStatus(
            RenderResourceHandle handle) const noexcept override;

        void BeginShutdown() noexcept;
        [[nodiscard]] ResourceUploadRequestRef TryDequeueUpload() noexcept;
        [[nodiscard]] RenderResourceHandle TryDequeueRelease() noexcept;
        [[nodiscard]] RenderGatewayUploadEnqueueResult
            TryEnqueueUploadObserved(
                const ResourceUploadRequestRef& request) noexcept;
        [[nodiscard]] RenderGatewayReleaseResult RequestReleaseObserved(
            RenderResourceHandle handle) noexcept;
        [[nodiscard]] RenderUploadQueueSnapshot
            GetUploadQueueSnapshot() const noexcept;
        [[nodiscard]] RenderReleaseQueueSnapshot
            GetReleaseQueueSnapshot() const noexcept;
        [[nodiscard]] uint32 GetRetainedUploadCount() const noexcept;
        [[nodiscard]] uint64 GetRetainedUploadBytes() const noexcept;
        [[nodiscard]] RenderResourceStatusTable& GetStatusTable() noexcept;
        [[nodiscard]] const RenderResourceStatusTable&
            GetStatusTable() const noexcept;

    private:
        struct ReservationIdentity
        {
            AssetId assetId;
            RenderResourceKind kind = RenderResourceKind::Invalid;
            uint32 generation = 0;
        };

        static uint32 ValidateConfigAndGetStatusCapacity(
            const RenderTransportConfig& config);

        std::atomic<bool> m_shuttingDown = false;
        RenderResourceStatusTable m_statusTable;
        RenderResourceReservationDirectory m_reservationDirectory;
        RenderUploadQueue m_uploadQueue;
        RenderReleaseQueue m_releaseQueue;
        std::vector<ReservationIdentity> m_reservationIdentities;
    };
} // namespace RVX
