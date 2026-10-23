#pragma once

/**
 * @file EcsSceneRenderProofGateway.h
 * @brief Engine-private value proof bridge between ECS extraction and asset lifetime.
 */

#include "RenderExtraction/ECS/EcsFrameExtractor.h"
#include "ResourceSceneAdapters/ECS/EcsEnvironmentPresentationReceipt.h"
#include "ResourceSceneAdapters/ECS/EcsSceneAssetLoadCoordinator.h"

#include <optional>
#include <span>
#include <string>
#include <vector>

namespace RVX
{
    /**
     * @brief Immutable Render transport progress supplied by the Engine owner.
     *
     * A nonzero value is a completed, monotonic acknowledgement from the
     * Render host, not an optimistic candidate sequence.  The gateway does
     * not access Render, RHI, or a mutable scene to obtain these values.
     */
    struct EcsSceneRenderPublicationProgress
    {
        uint64 appliedRenderSceneRevision = 0;
        uint64 presentedFrameSequence = 0;
        bool deviceLost = false;
    };

    /**
     * @brief Engine-private owner-thread proof gateway for pure ECS assets.
     *
     * The Engine must call ObserveExtractionCandidate immediately after a
     * complete EcsFrameExtractor::Extract and then ResolveObservedCandidate
     * exactly once with the disposition returned to that extractor.  A
     * receipt is available only if the immutable progress proves that exact
     * accepted candidate has reached both RenderScene and presentation.
     */
    class EcsSceneRenderProofGateway final
        : public ResourceSceneAdapters::IEcsSceneAssetRetirementProofGateway
    {
    public:
        explicit EcsSceneRenderProofGateway(EcsFrameExtractor& extractor) noexcept;
        ~EcsSceneRenderProofGateway() override;

        EcsSceneRenderProofGateway(const EcsSceneRenderProofGateway&) = delete;
        EcsSceneRenderProofGateway& operator=(const EcsSceneRenderProofGateway&) = delete;

        /** Capture one complete, still-unresolved ECS extraction candidate. */
        [[nodiscard]] bool ObserveExtractionCandidate(
            const EcsFrozenSceneBridgeOutput& source,
            const EcsFrameExtractionResult& extraction);

        /**
         * @brief Resolve the captured candidate with the same transport outcome
         * passed to EcsFrameExtractor::ResolveLastPublication.
         */
        [[nodiscard]] bool ResolveObservedCandidate(
            EcsFramePublicationDisposition disposition,
            const EcsRenderSceneRetirementResolution& retirementResolution,
            EcsSceneRenderPublicationProgress progress);

        /** Advance immutable completed transport progress without a candidate. */
        [[nodiscard]] bool ObservePublicationProgress(
            EcsSceneRenderPublicationProgress progress);

        /**
         * @brief Build exact activation evidence for a subset carried by one
         * accepted and actually presented frozen ECS candidate.
         */
        [[nodiscard]] std::optional<
            ResourceSceneAdapters::EcsSceneAssetMinimumResidentPresentationReceipt>
        BuildMinimumResidentPresentationReceipt(
            ECS::SceneRuntimeId sceneRuntimeId,
            ECS::EntityHandle rootEntity,
            std::span<const ResourceSceneAdapters::
                          EcsSceneAssetRenderableVisibilityVersion>
                renderableVisibilityVersions) const;

        /** @brief Build presentation proof for exactly one generation-qualified Skybox source. */
        [[nodiscard]] std::optional<ResourceSceneAdapters::EcsEnvironmentPresentationReceipt>
        BuildEnvironmentPresentationReceipt(
            ECS::SceneRuntimeId sceneRuntimeId,
            ECS::EntityHandle skyboxEntity,
            uint64 skyboxWriteVersion) const;

        // =====================================================================
        // IEcsSceneAssetRetirementProofGateway
        // =====================================================================
        [[nodiscard]] ResourceSceneAdapters::EcsSceneAssetRetirementBeginReceipt
        BeginRetirement(
            const ResourceSceneAdapters::EcsSceneAssetRetirementRequest& request) override;
        [[nodiscard]] ResourceSceneAdapters::EcsSceneAssetRetirementProof
        QueryRetirementProof(
            ResourceSceneAdapters::EcsSceneAssetRetirementToken token) const override;
        [[nodiscard]] bool AcknowledgeRetirementProof(
            ResourceSceneAdapters::EcsSceneAssetRetirementToken token) override;

        /** @brief True while any candidate or unacknowledged proof token is retained. */
        [[nodiscard]] bool HasOutstandingProofs() const noexcept;

    public:
        // Value records are public only because focused validation constructs no
        // engine objects; product code uses the methods above exclusively.
        struct RenderedMember
        {
            ECS::SceneRuntimeId sceneRuntimeId;
            ECS::EntityHandle entity = ECS::EntityHandle::Invalid();
            EcsRenderSceneRetainedMemberType type =
                EcsRenderSceneRetainedMemberType::Invalid;
            uint64 visibilityWriteVersion = 0;
            uint64 skyboxWriteVersion = 0;

            bool operator==(const RenderedMember&) const = default;
        };

        struct Candidate
        {
            EcsFrameExtractionCandidateIdentity identity;
            ECS::SceneRuntimeId sceneRuntimeId;
            uint64 frozenSourceSnapshotRevision = 0;
            uint64 renderSceneRevision = 0;
            uint64 frameSequence = 0;
            ECS::SceneRuntimeId acceptedSceneRuntimeIdAtObserve;
            uint64 acceptedSceneRevisionAtObserve = 0;
            uint64 acceptedFrameSequenceAtObserve = 0;
            bool fullReset = false;
            std::vector<RenderedMember> members;
        };

        struct RemovalEvidence
        {
            RenderedMember member;
            uint64 renderSceneRevision = 0;
            uint64 carryingFrameSequence = 0;
        };

        struct RetirementEntry
        {
            ResourceSceneAdapters::EcsSceneAssetRetirementToken token;
            uint64 nextGeneration = 1;
            ResourceSceneAdapters::EcsSceneAssetRetirementRequest request;
            EcsRenderSceneRetirementCorrelationId correlation;
            ResourceSceneAdapters::EcsSceneAssetRetirementProofState state =
                ResourceSceneAdapters::EcsSceneAssetRetirementProofState::Pending;
            uint64 appliedRenderSceneRevision = 0;
            uint64 requiredPresentedFrameSequence = 0;
            bool awaitingExtractorBarrier = false;
            std::vector<RenderedMember> barrierMembers;
            std::vector<RenderedMember> awaitingUnpresentedRemovals;
            std::string diagnostic;
        };

    private:

        [[nodiscard]] bool IsTrustedFor(ECS::SceneRuntimeId sceneRuntimeId) const noexcept;
        [[nodiscard]] bool IsProgressMonotonic(
            const EcsSceneRenderPublicationProgress& progress) const noexcept;
        void ApplyProgress(EcsSceneRenderPublicationProgress progress) noexcept;
        void RecordReliableCandidate(const Candidate& candidate,
                                     bool frameAccepted) noexcept;
        void RefreshRetirementEntries() noexcept;
        [[nodiscard]] RetirementEntry* Resolve(
            ResourceSceneAdapters::EcsSceneAssetRetirementToken token) noexcept;
        [[nodiscard]] const RetirementEntry* Resolve(
            ResourceSceneAdapters::EcsSceneAssetRetirementToken token) const noexcept;
        [[nodiscard]] std::optional<RemovalEvidence> FindRemovalEvidence(
            const RenderedMember& member) const;

        EcsFrameExtractor& m_extractor;
        std::optional<Candidate> m_pendingCandidate;
        std::vector<RenderedMember> m_currentReliableMembers;
        std::vector<RenderedMember> m_lastPresentedMembers;
        std::vector<RenderedMember> m_knownReliableMembers;
        std::vector<RemovalEvidence> m_removalEvidence;
        std::vector<RenderedMember> m_unpresentedRemovals;
        std::vector<Candidate> m_presentedCandidates;
        std::vector<RetirementEntry> m_retirements;
        EcsSceneRenderPublicationProgress m_progress;
        ECS::SceneRuntimeId m_currentReliableRuntime;
        bool m_hasTrustedReliableSnapshot = false;
        bool m_historyContinuityLost = false;
        bool m_presentationHistoryContinuityLost = false;
        uint64 m_nextCorrelation = 1;
    };
} // namespace RVX
