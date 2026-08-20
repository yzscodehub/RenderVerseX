#pragma once

/**
 * @file EcsRenderFramePipeline.h
 * @brief Engine-owned value pipeline from frozen ECS snapshots to Render publication.
 */

#include "EcsSceneRenderProofGateway.h"
#include "RenderExtraction/EcsFrozenSceneBridge.h"

#include <memory>
#include <optional>
#include <span>

namespace RVX
{
    class RenderSubsystem;

    /** @brief One immutable candidate transferred to the Render transport. */
    struct EcsRenderFrameTransportCandidate
    {
        EcsFrameExtractionCandidateIdentity candidateIdentity;
        ECS::SceneRuntimeId sourceSceneRuntimeId;
        uint64 sourceSnapshotRevision = 0;
        uint64 targetRenderSceneRevision = 0;
        uint64 frameSequence = 0;
        std::unique_ptr<const RenderSceneUpdateBatch> sceneUpdate;
        std::unique_ptr<const RenderFramePacketV5> frameV5;

        [[nodiscard]] bool IsStructurallyValid() const noexcept;
    };

    /** @brief Terminal ownership outcome for exactly one transport candidate. */
    enum class EcsRenderFrameTransportDisposition : uint8
    {
        Accepted = 0,
        SceneUpdateAcceptedWithoutFrame,
        NotAccepted,
    };

    /** @brief Value receipt emitted by a transport after it owns or drops a candidate. */
    struct EcsRenderFrameTransportReceipt
    {
        EcsRenderFrameTransportDisposition disposition =
            EcsRenderFrameTransportDisposition::NotAccepted;
        EcsFrameExtractionCandidateIdentity candidateIdentity;
        ECS::SceneRuntimeId sourceSceneRuntimeId;
        uint64 sourceSnapshotRevision = 0;
        uint64 targetRenderSceneRevision = 0;
        uint64 frameSequence = 0;
        EcsSceneRenderPublicationProgress completedProgress;
    };

    /**
     * @brief Narrow Engine-to-Render transport seam.
     *
     * The transport receives sole ownership of both immutable packets and must
     * return one exact value receipt. It deliberately exposes neither a Scene,
     * World, Actor nor RHI object to the ECS publication pipeline.
     */
    class IEcsRenderFrameTransport
    {
    public:
        virtual ~IEcsRenderFrameTransport() = default;

        [[nodiscard]] virtual EcsRenderFrameTransportReceipt TryPublish(
            EcsRenderFrameTransportCandidate candidate) noexcept = 0;
    };

    /**
     * @brief Production RenderSubsystem adapter for the ECS frame pipeline.
     *
     * It calls only public RenderSubsystem value APIs and keeps Scene, World,
     * Actor and RHI ownership outside the publication boundary.
     */
    class EcsRenderSubsystemFrameTransport final : public IEcsRenderFrameTransport
    {
    public:
        explicit EcsRenderSubsystemFrameTransport(RenderSubsystem& render) noexcept;

        [[nodiscard]] EcsRenderFrameTransportReceipt TryPublish(
            EcsRenderFrameTransportCandidate candidate) noexcept override;

    private:
        [[nodiscard]] EcsSceneRenderPublicationProgress
        ReadCompletedProgress() const noexcept;

        RenderSubsystem& m_render;
    };

    enum class EcsRenderFramePipelineCode : uint8
    {
        Published = 0,
        SceneUpdatePublishedWithoutFrame,
        TransportNotAccepted,
        BridgeFailed,
        ExtractionFailed,
        CandidateObservationRejected,
        TransportReceiptMismatch,
        NonMonotonicTransportProgress,
        ProofResolutionRejected,
    };

    /** @brief End-to-end immutable receipt for a candidate which reached transport. */
    struct EcsRenderFramePublicationReceipt
    {
        EcsFrameExtractionCandidateIdentity candidateIdentity;
        ECS::SceneRuntimeId sourceSceneRuntimeId;
        uint64 frozenSourceSnapshotRevision = 0;
        uint64 targetRenderSceneRevision = 0;
        uint64 carryingFrameSequence = 0;
        EcsFramePublicationDisposition disposition =
            EcsFramePublicationDisposition::NotAccepted;
        EcsSceneRenderPublicationProgress completedProgress;
        bool transportProgressWasMonotonic = true;

        [[nodiscard]] bool IsValid() const noexcept
        {
            return candidateIdentity.IsValid() && sourceSceneRuntimeId.IsValid() &&
                   frozenSourceSnapshotRevision != 0 &&
                   targetRenderSceneRevision != 0 && carryingFrameSequence != 0;
        }
    };

    /** @brief Full result without references into frozen ECS or Render packet storage. */
    struct EcsRenderFramePipelineResult
    {
        EcsRenderFramePipelineCode code = EcsRenderFramePipelineCode::BridgeFailed;
        EcsFrozenSceneBridgeResult bridge;
        EcsFrameExtractionResultCode extractionCode =
            EcsFrameExtractionResultCode::SealFailed;
        /** Complete value diagnostics copied before packet ownership moves to Render. */
        std::optional<RenderExtractionDiagnostics> completedExtractionDiagnostics;
        std::optional<EcsRenderFramePublicationReceipt> publication;

        [[nodiscard]] bool IsPublished() const noexcept
        {
            return code == EcsRenderFramePipelineCode::Published ||
                   code == EcsRenderFramePipelineCode::
                               SceneUpdatePublishedWithoutFrame;
        }
    };

    /**
     * @brief Pure ECS frame publication owner.
     *
     * This owner owns the bridge, extractor and exact proof gateway, enforcing
     * the order: build -> extract -> observe -> transport -> resolve extractor
     * -> resolve proof. Resource integration is strictly value-based through
     * EcsRenderAssetResolver and the exposed proof services below.
     */
    class EcsRenderFramePipeline final
    {
    public:
        explicit EcsRenderFramePipeline(IEcsRenderFrameTransport& transport) noexcept;
        ~EcsRenderFramePipeline();

        EcsRenderFramePipeline(const EcsRenderFramePipeline&) = delete;
        EcsRenderFramePipeline& operator=(const EcsRenderFramePipeline&) = delete;

        [[nodiscard]] EcsRenderFramePipelineResult Publish(
            const SceneECS::FrozenSceneSnapshot& snapshot,
            const EcsFrameExtractionInput& input);

        /** @brief Advance only monotonic Render completion evidence. */
        [[nodiscard]] bool ObserveCompletedProgress(
            EcsSceneRenderPublicationProgress progress);

        [[nodiscard]] ResourceSceneAdapters::IEcsSceneAssetRetirementProofGateway&
        GetAssetRetirementProofGateway() noexcept
        {
            return m_proofGateway;
        }

        [[nodiscard]] const ResourceSceneAdapters::IEcsSceneAssetRetirementProofGateway&
        GetAssetRetirementProofGateway() const noexcept
        {
            return m_proofGateway;
        }

        [[nodiscard]] std::optional<
            ResourceSceneAdapters::EcsSceneAssetMinimumResidentPresentationReceipt>
        BuildMinimumResidentPresentationReceipt(
            ECS::SceneRuntimeId sceneRuntimeId,
            ECS::EntityHandle rootEntity,
            std::span<const ResourceSceneAdapters::
                          EcsSceneAssetRenderableVisibilityVersion>
                renderableVisibilityVersions) const;

        /** @brief Forward exact accepted-presentation proof for one ECS Skybox. */
        [[nodiscard]] std::optional<ResourceSceneAdapters::EcsEnvironmentPresentationReceipt>
        BuildEnvironmentPresentationReceipt(
            ECS::SceneRuntimeId sceneRuntimeId,
            ECS::EntityHandle skyboxEntity,
            uint64 skyboxWriteVersion) const;

        [[nodiscard]] const EcsFrameExtractor& GetExtractor() const noexcept
        {
            return m_extractor;
        }

        [[nodiscard]] EcsSceneRenderPublicationProgress
        GetCompletedProgress() const noexcept
        {
            return m_completedProgress;
        }

        /** @brief Stop is safe only after every candidate and proof token is consumed. */
        [[nodiscard]] bool HasOutstandingProofs() const noexcept
        {
            return m_proofGateway.HasOutstandingProofs();
        }

    private:
        [[nodiscard]] static bool IsProgressMonotonic(
            EcsSceneRenderPublicationProgress previous,
            EcsSceneRenderPublicationProgress next) noexcept;
        [[nodiscard]] static bool Matches(
            const EcsRenderFrameTransportReceipt& receipt,
            const EcsRenderFrameTransportCandidate& candidate) noexcept;
        [[nodiscard]] static EcsFramePublicationDisposition ToPublicationDisposition(
            EcsRenderFrameTransportDisposition disposition) noexcept;

        IEcsRenderFrameTransport& m_transport;
        EcsFrozenSceneBridge m_bridge;
        EcsFrameExtractor m_extractor;
        EcsSceneRenderProofGateway m_proofGateway;
        EcsSceneRenderPublicationProgress m_completedProgress;
    };
} // namespace RVX
