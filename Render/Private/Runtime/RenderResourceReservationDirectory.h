#pragma once

/**
 * @file RenderResourceReservationDirectory.h
 * @brief Update-thread-owned resource reservation and generation allocator.
 */

#include "Runtime/RenderResourceStatusTable.h"

#include <unordered_map>
#include <vector>

namespace RVX
{
    struct ReservationEntry
    {
        RenderResourceKind kind = RenderResourceKind::Invalid;
        RenderResourceHandle handle;
    };

    struct RenderResourceReservationDirectoryTestAccess;

    class RenderResourceReservationDirectory final
    {
    public:
        explicit RenderResourceReservationDirectory(
            RenderResourceStatusTable& statusTable);

        RenderResourceReserveResult ReserveResource(
            AssetId assetId,
            RenderResourceKind kind) noexcept;
        RenderReleaseResult RequestRelease(
            RenderResourceHandle handle) noexcept;

    private:
        friend struct RenderResourceReservationDirectoryTestAccess;

        void ReclaimReleasedSlots() noexcept;

        RenderResourceStatusTable& m_statusTable;
        std::unordered_map<AssetId, ReservationEntry, AssetIdHash>
            m_reservations;
        std::vector<uint32> m_freeSlots;
        std::vector<bool> m_retiredSlots;
        std::vector<RenderResourceHandle> m_pendingReleases;
        std::vector<AssetId> m_assetsBySlot;
    };
} // namespace RVX
