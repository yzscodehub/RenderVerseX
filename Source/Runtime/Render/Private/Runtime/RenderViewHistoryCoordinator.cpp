#include "Runtime/RenderViewHistoryCoordinator.h"

#include "RenderContracts/RenderFrameValidation.h"

namespace RVX
{
RenderHistoryAvailability RenderViewHistoryCoordinator::Evaluate(
    const RenderFramePacketV5& frame,
    bool committedResourceAvailable) noexcept
{
    const RenderViewHistoryIdentity& incoming = frame.GetHistoryIdentity();
    const RenderViewHistoryIdentity* previous =
        m_lastPresentedIdentity.has_value() ? &*m_lastPresentedIdentity : nullptr;
    RenderHistoryAvailability result = EvaluateRenderHistoryAvailability(
        previous,
        incoming,
        frame.GetHeader().explicitDiscontinuity,
        committedResourceAvailable);

    if (previous != nullptr)
    {
        const uint64 incomingSequence =
            incoming.previousFrame.frameSequence;
        const uint64 previousSequence =
            previous->previousFrame.frameSequence;
        const bool isNextPresentedFrame =
            incomingSequence > previousSequence &&
            incomingSequence - previousSequence == 1U;
        if (!isNextPresentedFrame)
        {
            // Temporal resources describe the immediately preceding presented
            // frame. Mailbox replacement or an abandoned render can leave a
            // forward sequence gap, which is no more reusable than rollback.
            result.resetReasons |=
                RenderHistoryResetReason::SceneDiscontinuity;
        }
    }
    result.resetReasons |= m_pendingResetReasons;
    result.available = result.resetReasons == RenderHistoryResetReason::None;
    m_evaluatedIdentity = incoming;
    m_evaluatedResetRevision = m_resetRevision;
    return result;
}

bool RenderViewHistoryCoordinator::CommitPresented(
    const RenderViewHistoryIdentity& identity) noexcept
{
    if (!CanCommitPresented(identity))
    {
        return false;
    }

    m_lastPresentedIdentity = identity;
    m_evaluatedIdentity.reset();
    // Consume only the reset snapshot observed by Evaluate. A reset arriving
    // after evaluation belongs to a later frame and must remain fail-closed.
    if (!m_resetRevisionSaturated &&
        m_evaluatedResetRevision == m_resetRevision)
    {
        m_pendingResetReasons = RenderHistoryResetReason::None;
    }
    return true;
}

bool RenderViewHistoryCoordinator::CanCommitPresented(
    const RenderViewHistoryIdentity& identity) const noexcept
{
    return identity.IsValid() && m_evaluatedIdentity.has_value() &&
           *m_evaluatedIdentity == identity &&
           (!m_lastPresentedIdentity.has_value() ||
            identity.previousFrame.frameSequence >
                m_lastPresentedIdentity->previousFrame.frameSequence);
}

void RenderViewHistoryCoordinator::Abandon(
    const RenderViewHistoryIdentity& identity) noexcept
{
    if (m_evaluatedIdentity.has_value() && *m_evaluatedIdentity == identity)
        m_evaluatedIdentity.reset();
}

void RenderViewHistoryCoordinator::RequestReset(
    RenderHistoryResetReason reasons) noexcept
{
    if (reasons == RenderHistoryResetReason::None)
        return;
    m_pendingResetReasons |= reasons;
    if (m_resetRevision != std::numeric_limits<uint64>::max())
    {
        ++m_resetRevision;
    }
    else
    {
        // Once the token space is exhausted, never consume pending resets.
        m_resetRevisionSaturated = true;
    }
}

void RenderViewHistoryCoordinator::Clear() noexcept
{
    m_lastPresentedIdentity.reset();
    m_evaluatedIdentity.reset();
    m_pendingResetReasons = RenderHistoryResetReason::None;
    m_resetRevision = 0;
    m_evaluatedResetRevision = 0;
    m_resetRevisionSaturated = false;
}
} // namespace RVX
