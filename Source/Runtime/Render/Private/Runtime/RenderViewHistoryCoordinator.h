#pragma once

/**
 * @file RenderViewHistoryCoordinator.h
 * @brief Render-private presentation-qualified temporal history authority.
 */

#include "RenderContracts/RenderFramePacketV5.h"

#include <limits>
#include <optional>

namespace RVX
{
    /**
     * @brief Tracks one fail-closed view history until multi-view execution exists.
     *
     * Evaluate never advances committed state. Only CommitPresented for the
     * exact most recently evaluated identity can do so. Submission failure,
     * mailbox replacement, or abandoned rendering therefore cannot consume a
     * pending reset or advertise an unpresented temporal resource.
     */
    class RenderViewHistoryCoordinator final
    {
    public:
        [[nodiscard]] RenderHistoryAvailability Evaluate(
            const RenderFramePacketV5& frame,
            bool committedResourceAvailable) noexcept;

        /** @brief Commit only the exact evaluated identity after presentation. */
        [[nodiscard]] bool CommitPresented(
            const RenderViewHistoryIdentity& identity) noexcept;

        /** @brief Abandon an exact evaluated identity without advancing history. */
        void Abandon(const RenderViewHistoryIdentity& identity) noexcept;

        /** @brief Preserve an external reset request until a matching presentation. */
        void RequestReset(RenderHistoryResetReason reasons) noexcept;

        void Clear() noexcept;

        [[nodiscard]] const std::optional<RenderViewHistoryIdentity>&
            GetLastPresentedIdentity() const noexcept
        {
            return m_lastPresentedIdentity;
        }

        /** @brief Whether a reset newer than the last committed evaluation remains. */
        [[nodiscard]] bool HasPendingReset() const noexcept
        {
            return m_pendingResetReasons != RenderHistoryResetReason::None;
        }

    private:
        std::optional<RenderViewHistoryIdentity> m_lastPresentedIdentity;
        std::optional<RenderViewHistoryIdentity> m_evaluatedIdentity;
        RenderHistoryResetReason m_pendingResetReasons =
            RenderHistoryResetReason::None;
        /** Monotonic token prevents a reset requested after Evaluate from being lost. */
        uint64 m_resetRevision = 0;
        uint64 m_evaluatedResetRevision = 0;
        bool m_resetRevisionSaturated = false;
    };
} // namespace RVX
