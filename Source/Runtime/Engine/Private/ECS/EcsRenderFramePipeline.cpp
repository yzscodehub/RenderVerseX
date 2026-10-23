/**
 * @file EcsRenderFramePipeline.cpp
 * @brief Engine-owned pure ECS frame publication implementation.
 */

#include "EcsRenderFramePipeline.h"

#include "Render/RenderSubsystem.h"

#include <utility>

namespace RVX
{
bool EcsRenderFrameTransportCandidate::IsStructurallyValid() const noexcept
{
    return candidateIdentity.IsValid() && sourceSceneRuntimeId.IsValid() &&
           sourceSnapshotRevision != 0 && targetRenderSceneRevision != 0 &&
           frameSequence != 0 && frameV5 != nullptr &&
           frameV5->GetHeader().sequence == frameSequence &&
           frameV5->GetHeader().requiredSceneRevision == targetRenderSceneRevision &&
           (sceneUpdate == nullptr ||
            (sceneUpdate->IsStructurallyValid() &&
             sceneUpdate->targetSceneRevision == targetRenderSceneRevision));
}

EcsRenderSubsystemFrameTransport::EcsRenderSubsystemFrameTransport(
    RenderSubsystem& render) noexcept
    : m_render(render)
{
}

EcsSceneRenderPublicationProgress
EcsRenderSubsystemFrameTransport::ReadCompletedProgress() const noexcept
{
    const RenderDiagnosticsSnapshot diagnostics = m_render.GetDiagnosticsSnapshot();
    const RenderRuntimeResult runtime = m_render.GetLastRuntimeResult();
    return {
        .appliedRenderSceneRevision = diagnostics.sceneValues.available
                                          ? diagnostics.sceneValues.appliedSceneRevision
                                          : 0,
        .presentedFrameSequence = diagnostics.lastPresentedFrameSequence,
        .deviceLost = runtime.code == RenderRuntimeCode::DeviceLost ||
                      (diagnostics.lastFailure.available &&
                       diagnostics.lastFailure.runtime.code ==
                           RenderRuntimeCode::DeviceLost),
    };
}

EcsRenderFrameTransportReceipt EcsRenderSubsystemFrameTransport::TryPublish(
    EcsRenderFrameTransportCandidate candidate) noexcept
{
    EcsRenderFrameTransportReceipt receipt;
    receipt.candidateIdentity = candidate.candidateIdentity;
    receipt.sourceSceneRuntimeId = candidate.sourceSceneRuntimeId;
    receipt.sourceSnapshotRevision = candidate.sourceSnapshotRevision;
    receipt.targetRenderSceneRevision = candidate.targetRenderSceneRevision;
    receipt.frameSequence = candidate.frameSequence;

    if (!candidate.IsStructurallyValid())
    {
        receipt.completedProgress = ReadCompletedProgress();
        return receipt;
    }

    const RenderFramePublishResult result = m_render.TryPublishFrameSet(
        std::move(candidate.sceneUpdate), std::move(candidate.frameV5));
    if (result.code == RenderFramePublishCode::Accepted ||
        result.code == RenderFramePublishCode::ReplacedOlder)
    {
        receipt.disposition = EcsRenderFrameTransportDisposition::Accepted;
    }
    else if (result.sceneUpdateAccepted)
    {
        // The immutable Scene update is now Render-owned, but the paired frame
        // was dropped. The extractor must force a full-reset frame next time.
        receipt.disposition =
            EcsRenderFrameTransportDisposition::SceneUpdateAcceptedWithoutFrame;
    }
    receipt.completedProgress = ReadCompletedProgress();
    return receipt;
}

EcsRenderFramePipeline::EcsRenderFramePipeline(
    IEcsRenderFrameTransport& transport) noexcept
    : m_transport(transport),
      m_proofGateway(m_extractor)
{
}

EcsRenderFramePipeline::~EcsRenderFramePipeline() = default;

bool EcsRenderFramePipeline::IsProgressMonotonic(
    EcsSceneRenderPublicationProgress previous,
    EcsSceneRenderPublicationProgress next) noexcept
{
    if (previous.deviceLost)
    {
        return next.deviceLost;
    }
    return next.deviceLost ||
           (next.appliedRenderSceneRevision >=
                previous.appliedRenderSceneRevision &&
            next.presentedFrameSequence >= previous.presentedFrameSequence);
}

bool EcsRenderFramePipeline::Matches(
    const EcsRenderFrameTransportReceipt& receipt,
    const EcsRenderFrameTransportCandidate& candidate) noexcept
{
    return receipt.candidateIdentity == candidate.candidateIdentity &&
           receipt.sourceSceneRuntimeId == candidate.sourceSceneRuntimeId &&
           receipt.sourceSnapshotRevision == candidate.sourceSnapshotRevision &&
           receipt.targetRenderSceneRevision == candidate.targetRenderSceneRevision &&
           receipt.frameSequence == candidate.frameSequence;
}

EcsFramePublicationDisposition EcsRenderFramePipeline::ToPublicationDisposition(
    EcsRenderFrameTransportDisposition disposition) noexcept
{
    switch (disposition)
    {
        case EcsRenderFrameTransportDisposition::Accepted:
            return EcsFramePublicationDisposition::Accepted;
        case EcsRenderFrameTransportDisposition::SceneUpdateAcceptedWithoutFrame:
            return EcsFramePublicationDisposition::SceneUpdateAcceptedWithoutFrame;
        case EcsRenderFrameTransportDisposition::NotAccepted:
        default: return EcsFramePublicationDisposition::NotAccepted;
    }
}

EcsRenderFramePipelineResult EcsRenderFramePipeline::Publish(
    const SceneECS::FrozenSceneSnapshot& snapshot,
    const EcsFrameExtractionInput& input)
{
    EcsRenderFramePipelineResult result;
    EcsFrozenSceneBridgeOutput source;
    if (!m_bridge.Build(snapshot, source, &result.bridge))
    {
        result.code = EcsRenderFramePipelineCode::BridgeFailed;
        return result;
    }
    if (source.sceneRuntimeId != snapshot.sceneRuntimeId ||
        source.snapshotRevision != snapshot.revision ||
        !source.sceneRuntimeId.IsValid() || source.snapshotRevision == 0)
    {
        result.code = EcsRenderFramePipelineCode::BridgeFailed;
        return result;
    }

    EcsFrameExtractionResult extraction = m_extractor.Extract(input, source);
    result.extractionCode = extraction.code;
    if (!extraction.IsComplete())
    {
        result.code = EcsRenderFramePipelineCode::ExtractionFailed;
        return result;
    }
    const bool frameOnly = extraction.IsFrameOnly();
    result.completedExtractionDiagnostics = extraction.diagnostics;
    if (!m_proofGateway.ObserveExtractionCandidate(source, extraction))
    {
        // Transport never received ownership. Explicitly terminalize the
        // extractor candidate as dropped so a rejected proof observation
        // cannot poison every later frame with a permanently pending value.
        static_cast<void>(m_extractor.ResolveLastPublication(
            EcsFramePublicationDisposition::NotAccepted));
        result.code = EcsRenderFramePipelineCode::CandidateObservationRejected;
        return result;
    }

    EcsRenderFrameTransportCandidate candidate;
    candidate.candidateIdentity = extraction.candidateIdentity;
    candidate.sourceSceneRuntimeId = extraction.sourceSceneRuntimeId;
    candidate.sourceSnapshotRevision = extraction.sourceSnapshotRevision;
    // Extractor completeness makes a null scene update an explicitly
    // qualified frame-only candidate; all other candidates retain the exact
    // Scene update/revision pairing checked by the transport candidate.
    candidate.targetRenderSceneRevision = extraction.targetRenderSceneRevision;
    candidate.frameSequence = extraction.frameV5->GetHeader().sequence;
    candidate.sceneUpdate = std::move(extraction.sceneUpdate);
    candidate.frameV5 = std::move(extraction.frameV5);

    const EcsRenderFrameTransportCandidate receiptExpected{
        .candidateIdentity = candidate.candidateIdentity,
        .sourceSceneRuntimeId = candidate.sourceSceneRuntimeId,
        .sourceSnapshotRevision = candidate.sourceSnapshotRevision,
        .targetRenderSceneRevision = candidate.targetRenderSceneRevision,
        .frameSequence = candidate.frameSequence,
    };
    const EcsRenderFrameTransportReceipt transportReceipt =
        m_transport.TryPublish(std::move(candidate));
    const bool receiptMatches = Matches(transportReceipt, receiptExpected);
    const bool legalDisposition =
        !frameOnly || transportReceipt.disposition !=
                          EcsRenderFrameTransportDisposition::
                              SceneUpdateAcceptedWithoutFrame;
    const bool progressMonotonic = IsProgressMonotonic(
        m_completedProgress, transportReceipt.completedProgress);

    // A malformed receipt or a regressing progress report cannot be allowed to
    // establish an ECS baseline. Resolve the candidate as dropped using the
    // last trusted completed progress so both extractor and proof gateway are
    // left with no unresolved candidate.
    const EcsFramePublicationDisposition disposition =
        receiptMatches && legalDisposition && progressMonotonic
            ? ToPublicationDisposition(transportReceipt.disposition)
            : EcsFramePublicationDisposition::NotAccepted;
    const EcsSceneRenderPublicationProgress trustedProgress =
        progressMonotonic ? transportReceipt.completedProgress : m_completedProgress;
    const EcsRenderSceneRetirementResolution retirementResolution =
        m_extractor.ResolveLastPublication(disposition);
    const bool proofResolved = m_proofGateway.ResolveObservedCandidate(
        disposition, retirementResolution, trustedProgress);

    EcsRenderFramePublicationReceipt publication;
    publication.candidateIdentity = receiptExpected.candidateIdentity;
    publication.sourceSceneRuntimeId = receiptExpected.sourceSceneRuntimeId;
    publication.frozenSourceSnapshotRevision =
        receiptExpected.sourceSnapshotRevision;
    publication.targetRenderSceneRevision =
        receiptExpected.targetRenderSceneRevision;
    publication.carryingFrameSequence = receiptExpected.frameSequence;
    publication.disposition = disposition;
    publication.completedProgress = trustedProgress;
    publication.transportProgressWasMonotonic = progressMonotonic;
    result.publication = publication;

    if (!proofResolved)
    {
        result.code = EcsRenderFramePipelineCode::ProofResolutionRejected;
        return result;
    }
    if (!receiptMatches)
    {
        result.code = EcsRenderFramePipelineCode::TransportReceiptMismatch;
        return result;
    }
    if (!progressMonotonic)
    {
        result.code = EcsRenderFramePipelineCode::NonMonotonicTransportProgress;
        return result;
    }

    m_completedProgress = transportReceipt.completedProgress;
    switch (disposition)
    {
        case EcsFramePublicationDisposition::Accepted:
            result.code = EcsRenderFramePipelineCode::Published;
            break;
        case EcsFramePublicationDisposition::SceneUpdateAcceptedWithoutFrame:
            result.code =
                EcsRenderFramePipelineCode::SceneUpdatePublishedWithoutFrame;
            break;
        case EcsFramePublicationDisposition::NotAccepted:
        default:
            result.code = EcsRenderFramePipelineCode::TransportNotAccepted;
            break;
    }
    return result;
}

bool EcsRenderFramePipeline::ObserveCompletedProgress(
    EcsSceneRenderPublicationProgress progress)
{
    if (!IsProgressMonotonic(m_completedProgress, progress) ||
        !m_proofGateway.ObservePublicationProgress(progress))
    {
        return false;
    }
    m_completedProgress = progress;
    return true;
}

std::optional<
    ResourceSceneAdapters::EcsSceneAssetMinimumResidentPresentationReceipt>
EcsRenderFramePipeline::BuildMinimumResidentPresentationReceipt(
    ECS::SceneRuntimeId sceneRuntimeId,
    ECS::EntityHandle rootEntity,
    std::span<const ResourceSceneAdapters::EcsSceneAssetRenderableVisibilityVersion>
        renderableVisibilityVersions) const
{
    return m_proofGateway.BuildMinimumResidentPresentationReceipt(
        sceneRuntimeId, rootEntity, renderableVisibilityVersions);
}

std::optional<ResourceSceneAdapters::EcsEnvironmentPresentationReceipt>
EcsRenderFramePipeline::BuildEnvironmentPresentationReceipt(
    ECS::SceneRuntimeId sceneRuntimeId,
    ECS::EntityHandle skyboxEntity,
    uint64 skyboxWriteVersion) const
{
    return m_proofGateway.BuildEnvironmentPresentationReceipt(
        sceneRuntimeId, skyboxEntity, skyboxWriteVersion);
}
} // namespace RVX
