#pragma once

/**
 * @file EcsFrameExtractor.h
 * @brief Pure-value ECS bridge-output extraction into persistent render contracts.
 */

#include "RenderContracts/RenderFramePacketV5.h"
#include "RenderContracts/RenderSceneUpdate.h"
#include "RenderExtraction/EcsFrozenSceneBridge.h"
#include "RenderExtraction/ECS/EcsRenderSceneRetirement.h"

#include <atomic>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

namespace RVX
{
    /** @brief Opaque value callback used to resolve an AssetId for render contracts. */
    struct EcsRenderAssetResolver
    {
        using ResolveCallback = RenderResourceHandle (*) (
            void* context,
            AssetId assetId,
            RenderResourceKind kind) noexcept;

        void* context = nullptr;
        ResolveCallback resolve = nullptr;

        [[nodiscard]] bool IsValid() const noexcept { return resolve != nullptr; }

        [[nodiscard]] RenderResourceHandle Resolve(
            AssetId assetId,
            RenderResourceKind kind) const noexcept
        {
            return resolve != nullptr ? resolve(context, assetId, kind)
                                   : RenderResourceHandle{};
        }
    };

    enum class EcsFrameExtractionResultCode : uint8
    {
        Complete = 0,
        CandidatePublicationPending,
        InvalidInput,
        InvalidBridgeOutput,
        MissingSelectedCamera,
        StaleSourceRevision,
        NonMonotonicFrameSequence,
        RequiredAssetUnresolved,
        SealFailed,
    };

    /** @brief Output-target and temporal values supplied independently of ECS state. */
    struct EcsFrameExtractionInput
    {
        uint64 sequence = 0;
        uint64 worldRevision = 0;
        uint64 temporalEpoch = 0;
        bool explicitDiscontinuity = false;
        uint32 outputWidth = 0;
        uint32 outputHeight = 0;
        float32 absoluteTime = 0.0f;
        float32 deltaTime = 0.0f;
        RenderFrameSettings settings;
        RenderFrameCaptureRequest captureRequest;
        EcsRenderAssetResolver assetResolver;
    };

    /** @brief Candidate packets emitted from a complete frozen ECS bridge output. */
    struct EcsFrameExtractionResult
    {
        EcsFrameExtractionResultCode code = EcsFrameExtractionResultCode::SealFailed;
        RenderExtractionDiagnostics diagnostics;
        EcsFrameExtractionCandidateIdentity candidateIdentity;
        ECS::SceneRuntimeId sourceSceneRuntimeId;
        uint64 sourceSnapshotRevision = 0;
        /** @brief RenderScene revision required by the carried frame. */
        uint64 targetRenderSceneRevision = 0;
        /** @brief True only when this candidate deliberately carries no Scene update. */
        bool frameOnly = false;
        std::unique_ptr<const RenderSceneUpdateBatch> sceneUpdate;
        std::unique_ptr<const RenderFramePacketV5> frameV5;

        [[nodiscard]] bool IsComplete() const noexcept
        {
            return code == EcsFrameExtractionResultCode::Complete &&
                   candidateIdentity.IsValid() && sourceSceneRuntimeId.IsValid() &&
                   sourceSnapshotRevision != 0 && targetRenderSceneRevision != 0 &&
                   frameV5 != nullptr &&
                   frameV5->GetHeader().requiredSceneRevision ==
                       targetRenderSceneRevision &&
                   ((frameOnly && sceneUpdate == nullptr) ||
                    (!frameOnly && sceneUpdate != nullptr &&
                     sceneUpdate->IsStructurallyValid() &&
                     sceneUpdate->targetSceneRevision ==
                         targetRenderSceneRevision));
        }

        /** @brief Return whether this complete result is the explicit frame-only form. */
        [[nodiscard]] bool IsFrameOnly() const noexcept
        {
            return IsComplete() && frameOnly;
        }
    };

    /**
     * @brief Extracts bridge-owned ECS values without owning Scene, World, Resource, or RHI state.
     *
     * A successful Extract creates an uncommitted candidate. Call
     * ResolveLastPublication after transport disposition. A scene-only
     * acknowledgement advances the retained scene/source baseline but forces
     * the next candidate to be a full reset; it never advances the accepted
     * frame sequence.
     */
    class EcsFrameExtractor final
    {
    public:
        EcsFrameExtractor() noexcept;
        EcsFrameExtractor(const EcsFrameExtractor&) = delete;
        EcsFrameExtractor& operator=(const EcsFrameExtractor&) = delete;

        [[nodiscard]] EcsFrameExtractionResult Extract(
            const EcsFrameExtractionInput& input,
            const EcsFrozenSceneBridgeOutput& source);

        /** @brief Accept or discard one candidate and return any released retirement proofs. */
        [[nodiscard]] EcsRenderSceneRetirementResolution ResolveLastPublication(
            EcsFramePublicationDisposition disposition) noexcept;

        /** @brief Admit an ECS-native removal barrier against the accepted baseline. */
        [[nodiscard]] EcsRenderSceneRetirementBarrierSubmitResult
        SubmitRetirementBarrier(EcsRenderSceneRetirementBarrier barrier);

        [[nodiscard]] bool HasPendingCandidate() const noexcept
        {
            return m_candidateScene.has_value();
        }

        /** @brief Return the opaque identity of the currently unresolved candidate. */
        [[nodiscard]] EcsFrameExtractionCandidateIdentity
        GetPendingCandidateIdentity() const noexcept
        {
            return m_candidateIdentity;
        }

        /** @brief Verify immutable values against the currently unresolved candidate. */
        [[nodiscard]] bool MatchesPendingCandidate(
            EcsFrameExtractionCandidateIdentity identity,
            ECS::SceneRuntimeId sourceSceneRuntimeId,
            uint64 sourceSnapshotRevision,
            uint64 targetSceneRevision,
            uint64 frameSequence) const noexcept;

        [[nodiscard]] uint64 GetAcceptedSceneRevision() const noexcept
        {
            return m_acceptedSceneRevision;
        }

        [[nodiscard]] ECS::SceneRuntimeId GetAcceptedSceneRuntimeId() const noexcept
        {
            return m_acceptedSceneRuntimeId;
        }

        /** @brief Last sequence whose frame transport was explicitly accepted. */
        [[nodiscard]] uint64 GetAcceptedFrameSequence() const noexcept
        {
            return m_acceptedFrameSequence;
        }

    private:
        struct RetainedSceneState
        {
            std::unordered_map<uint64, RenderPrimitiveSnapshot> primitives;
            std::unordered_map<uint64, RenderLightSnapshot> lights;
            std::unordered_map<uint64, ParticleRenderSnapshotItem> particles;
            std::unordered_map<uint64, WaterRenderSnapshotItem> water;
            std::unordered_map<uint64, TerrainRenderSnapshotItem> terrain;
            std::optional<RenderSkySnapshot> sky;
            std::optional<RenderEnvironmentSnapshot> environment;
            uint64 skySourceId = 0;
        };

        struct PendingRetirementIdentity
        {
            EcsRenderSceneRetainedMemberType type =
                EcsRenderSceneRetainedMemberType::Invalid;
            uint64 renderId = 0;
            /** @brief First reliable Scene revision which removed this exact identity. */
            uint64 removalSceneRevision = 0;
        };

        struct PendingRetirementBarrier
        {
            EcsRenderSceneRetirementCorrelationId correlation;
            std::vector<PendingRetirementIdentity> identities;
        };

        [[nodiscard]] static uint64 AllocateExtractorInstanceId() noexcept;

        std::optional<RetainedSceneState> m_acceptedScene;
        std::optional<RetainedSceneState> m_candidateScene;
        inline static std::atomic<uint64> s_nextExtractorInstanceId{1};
        ECS::SceneRuntimeId m_acceptedSceneRuntimeId;
        ECS::SceneRuntimeId m_candidateSceneRuntimeId;
        uint64 m_acceptedSourceRevision = 0;
        uint64 m_candidateSourceRevision = 0;
        uint64 m_acceptedSceneRevision = 0;
        uint64 m_candidateSceneRevision = 0;
        uint64 m_candidateSequence = 0;
        EcsFrameExtractionCandidateIdentity m_candidateIdentity;
        uint64 m_extractorInstanceId = 0;
        uint64 m_nextCandidateIdentitySequence = 1;
        uint64 m_lastCompletedInputSequence = 0;
        uint64 m_acceptedFrameSequence = 0;
        bool m_candidateFullReset = false;
        bool m_forceFullReset = false;
        std::vector<PendingRetirementIdentity>
            m_candidateExplicitRemovalIdentities;
        std::vector<PendingRetirementBarrier> m_pendingRetirementBarriers;
    };

    static_assert(
        static_cast<uint8>(EcsFrameExtractionResultCode::Complete) == 0);
} // namespace RVX
