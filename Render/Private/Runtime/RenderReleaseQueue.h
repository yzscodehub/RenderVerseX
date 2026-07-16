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

    struct RenderReleaseQueueSnapshot
    {
        uint32 pendingCount = 0;
        uint32 oldestPendingGeneration = 0;
        uint32 highWaterMark = 0;
    };

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
            RenderResourceHandle handle,
            RenderReleaseQueueSnapshot* observation = nullptr) noexcept;
        /**
         * @brief Pop at most one ring entry and return it only while its exact
         * generation is still Current and Evicting; otherwise consume that
         * obsolete entry and return an invalid handle.
         */
        [[nodiscard]] RenderResourceHandle TryDequeue() noexcept;
        [[nodiscard]] uint32 GetUsableCapacity() const noexcept;
        [[nodiscard]] uint32 GetPendingCount() const noexcept;
        [[nodiscard]] RenderReleaseQueueSnapshot GetSnapshot() const noexcept;

    private:
        friend struct RenderReleaseQueueTestAccess;

        void Wake() const noexcept;
        void RuntimeFatal(const char* message) const noexcept;

        RenderResourceReservationDirectory& m_reservationDirectory;
        const RenderResourceStatusTable& m_statusTable;
        uint32 m_usableCapacity = 0;
        mutable std::mutex m_mutex;
        std::vector<RenderResourceHandle> m_slots;
        uint32 m_head = 0;
        uint32 m_count = 0;
        uint32 m_highWaterMark = 0;
        WakeFunction m_wakeFunction = nullptr;
        void* m_wakeContext = nullptr;
        RuntimeFatalFunction m_runtimeFatalFunction = nullptr;
        void* m_runtimeFatalContext = nullptr;
    };
} // namespace RVX
