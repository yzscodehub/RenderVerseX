#include "Runtime/RenderResourceReservationDirectory.h"

#include <limits>

namespace RVX
{
namespace
{
    [[nodiscard]] bool IsLiveState(RenderResourcePublicState state) noexcept
    {
        return state == RenderResourcePublicState::Reserved ||
               state == RenderResourcePublicState::UploadQueued ||
               state == RenderResourcePublicState::Uploading ||
               state == RenderResourcePublicState::GPUReady ||
               state == RenderResourcePublicState::Failed ||
               state == RenderResourcePublicState::ReplacementQueued ||
               state == RenderResourcePublicState::Replacing;
    }
} // namespace

    RenderResourceReservationDirectory::RenderResourceReservationDirectory(
        RenderResourceStatusTable& statusTable)
        : m_statusTable(statusTable)
    {
        const uint32 capacity = statusTable.GetCapacity();
        m_reservations.reserve(capacity - 1U);
        m_freeSlots.reserve(capacity - 1U);
        m_retiredSlots.resize(capacity, false);
        m_pendingReleases.reserve(capacity - 1U);
        m_assetsBySlot.resize(capacity);

        for (uint32 slot = capacity - 1U; slot != 0; --slot)
        {
            PackedRenderResourceStatus status;
            if (!statusTable.LoadSlot(slot, status) ||
                status.state != RenderResourcePublicState::Released)
            {
                m_retiredSlots[slot] = true;
                continue;
            }

            if (status.generation == std::numeric_limits<uint32>::max())
            {
                m_retiredSlots[slot] = true;
                continue;
            }

            m_freeSlots.push_back(slot);
        }
    }

    RenderResourceReserveResult
        RenderResourceReservationDirectory::ReserveResource(
            AssetId assetId,
            RenderResourceKind kind) noexcept
    {
        RenderResourceReserveResult result;
        if (!assetId.IsValid())
        {
            return result;
        }
        if (kind == RenderResourceKind::Invalid)
        {
            result.code = RenderResourceReserveCode::KindMismatch;
            return result;
        }

        const auto existing = m_reservations.find(assetId);
        if (existing != m_reservations.end())
        {
            if (existing->second.kind != kind)
            {
                result.code = RenderResourceReserveCode::KindMismatch;
                return result;
            }

            result.code = RenderResourceReserveCode::Existing;
            result.handle = existing->second.handle;
            result.status = m_statusTable.Query(result.handle);
            return result;
        }

        ReclaimReleasedSlots();
        while (!m_freeSlots.empty())
        {
            const uint32 slot = m_freeSlots.back();
            m_freeSlots.pop_back();
            if (m_retiredSlots[slot])
            {
                continue;
            }

            PackedRenderResourceStatus current;
            if (!m_statusTable.LoadSlot(slot, current) ||
                current.state != RenderResourcePublicState::Released)
            {
                m_retiredSlots[slot] = true;
                continue;
            }
            if (current.generation == std::numeric_limits<uint32>::max())
            {
                m_retiredSlots[slot] = true;
                continue;
            }

            const RenderResourceHandle currentHandle{slot, current.generation};
            const RenderResourceHandle newHandle{slot,
                                                 current.generation + 1U};
            try
            {
                const auto [reservation, inserted] = m_reservations.emplace(
                    assetId, ReservationEntry{kind, newHandle});
                if (!inserted)
                {
                    result.code = RenderResourceReserveCode::Existing;
                    result.handle = reservation->second.handle;
                    result.status = m_statusTable.Query(result.handle);
                    m_freeSlots.push_back(slot);
                    return result;
                }
            }
            catch (...)
            {
                m_freeSlots.push_back(slot);
                result.code = RenderResourceReserveCode::CapacityExceeded;
                return result;
            }

            const PackedRenderResourceStatus desired{
                newHandle.generation,
                RenderResourcePublicState::Reserved,
                RenderResourceFailureCode::None};
            if (!m_statusTable.CompareExchange(currentHandle,
                                               current,
                                               desired,
                                               RenderStatusWriter::Update))
            {
                m_reservations.erase(assetId);
                PackedRenderResourceStatus refreshed;
                if (m_statusTable.LoadSlot(slot, refreshed) &&
                    refreshed.state == RenderResourcePublicState::Released &&
                    refreshed.generation !=
                        std::numeric_limits<uint32>::max())
                {
                    m_freeSlots.push_back(slot);
                }
                else
                {
                    m_retiredSlots[slot] = true;
                }
                continue;
            }

            m_assetsBySlot[slot] = assetId;
            result.code = RenderResourceReserveCode::Reserved;
            result.handle = newHandle;
            result.status.code = RenderResourceStatusCode::Current;
            result.status.state = RenderResourcePublicState::Reserved;
            result.status.failure = RenderResourceFailureCode::None;
            return result;
        }

        result.code = RenderResourceReserveCode::CapacityExceeded;
        return result;
    }

    RenderReleaseResult RenderResourceReservationDirectory::RequestRelease(
        RenderResourceHandle handle) noexcept
    {
        RenderReleaseResult result;
        if (!handle.IsValid() || handle.slot >= m_assetsBySlot.size())
        {
            return result;
        }

        const AssetId assetId = m_assetsBySlot[handle.slot];
        if (!assetId.IsValid())
        {
            const RenderResourceStatus status = m_statusTable.Query(handle);
            if (status.code == RenderResourceStatusCode::Current &&
                (status.state == RenderResourcePublicState::Evicting ||
                 status.state == RenderResourcePublicState::Released))
            {
                result.code = RenderReleaseCode::AlreadyPending;
            }
            return result;
        }

        const auto reservation = m_reservations.find(assetId);
        if (reservation == m_reservations.end() ||
            reservation->second.handle != handle)
        {
            return result;
        }

        for (;;)
        {
            const RenderResourceStatus status = m_statusTable.Query(handle);
            if (status.code != RenderResourceStatusCode::Current)
            {
                return result;
            }
            if (status.state == RenderResourcePublicState::Evicting ||
                status.state == RenderResourcePublicState::Released)
            {
                result.code = RenderReleaseCode::AlreadyPending;
                return result;
            }
            if (!IsLiveState(status.state))
            {
                return result;
            }

            const PackedRenderResourceStatus expected{
                handle.generation, status.state, status.failure};
            const PackedRenderResourceStatus desired{
                handle.generation,
                RenderResourcePublicState::Evicting,
                status.failure};
            if (!m_statusTable.CompareExchange(handle,
                                               expected,
                                               desired,
                                               RenderStatusWriter::Update))
            {
                continue;
            }

            m_pendingReleases.push_back(handle);
            m_reservations.erase(reservation);
            m_assetsBySlot[handle.slot] = AssetId{};
            result.code = RenderReleaseCode::Accepted;
            return result;
        }
    }

    void RenderResourceReservationDirectory::ReclaimReleasedSlots() noexcept
    {
        size_t pendingIndex = 0;
        while (pendingIndex < m_pendingReleases.size())
        {
            const RenderResourceHandle handle =
                m_pendingReleases[pendingIndex];
            const RenderResourceStatus status = m_statusTable.Query(handle);
            if (status.code != RenderResourceStatusCode::Current ||
                status.state != RenderResourcePublicState::Released)
            {
                ++pendingIndex;
                continue;
            }

            if (handle.generation == std::numeric_limits<uint32>::max())
            {
                m_retiredSlots[handle.slot] = true;
            }
            else
            {
                m_freeSlots.push_back(handle.slot);
            }

            m_pendingReleases[pendingIndex] = m_pendingReleases.back();
            m_pendingReleases.pop_back();
        }
    }
} // namespace RVX
