#pragma once

/**
 * @file RenderReleaseQueue.h
 * @brief Bounded release-handle ring with pre-publication eviction CAS.
 */

#include "RenderContracts/IRenderResourceGateway.h"
#include "Runtime/RenderResourceReservationDirectory.h"
#include "Runtime/RenderResourceStatusTable.h"

#include <mutex>
#include <vector>

namespace RVX
{
    struct RenderReleaseQueueTestAccess;

    class RenderReleaseQueue final
    {
    public:
        using WakeFunction = void (*)(void*) noexcept;
        using RuntimeFatalFunction = void (*)(void*, const char*) noexcept;

        RenderReleaseQueue(
            RenderResourceReservationDirectory& reservationDirectory,
            const RenderResourceStatusTable& statusTable,
            uint32 statusSlotCapacity,
            WakeFunction wakeFunction = nullptr,
            void* wakeContext = nullptr,
            RuntimeFatalFunction runtimeFatalFunction = nullptr,
            void* runtimeFatalContext = nullptr);
        RenderReleaseQueue(const RenderReleaseQueue&) = delete;
        RenderReleaseQueue& operator=(const RenderReleaseQueue&) = delete;

        RenderReleaseResult RequestRelease(
            RenderResourceHandle handle) noexcept;
        [[nodiscard]] RenderResourceHandle TryDequeue() noexcept;
        [[nodiscard]] uint32 GetUsableCapacity() const noexcept;
        [[nodiscard]] uint32 GetPendingCount() const noexcept;

    private:
        friend struct RenderReleaseQueueTestAccess;

        void Wake() const noexcept;
        void RuntimeFatal(const char* message) const noexcept;

        RenderResourceReservationDirectory& m_reservationDirectory;
        uint32 m_usableCapacity = 0;
        mutable std::mutex m_mutex;
        std::vector<RenderResourceHandle> m_slots;
        uint32 m_head = 0;
        uint32 m_count = 0;
        WakeFunction m_wakeFunction = nullptr;
        void* m_wakeContext = nullptr;
        RuntimeFatalFunction m_runtimeFatalFunction = nullptr;
        void* m_runtimeFatalContext = nullptr;
    };
} // namespace RVX
