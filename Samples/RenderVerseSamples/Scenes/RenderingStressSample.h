#pragma once

/**
 * @file RenderingStressSample.h
 * @brief Deterministic retained-scene and GPU-scene stress qualification sample.
 */

#include "Core/Types.h"
#include "RenderContracts/RenderFrameTypes.h"
#include "Samples/Sample.h"
#include "Samples/SampleCLI.h"
#include "Samples/SampleModelLoader.h"
#include "Samples/SampleOrbitCameraController.h"
#include "Scene/ECS/SceneEcsRuntime.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace RVX
{
    /** @brief Explicit phases reported by the rendering-stress state machine. */
    enum class RenderingStressPhase : uint8
    {
        WaitingForFirstPresentation = 0,
        WaitingForSharedAsset,
        InitialBuild,
        StableBaseline,
        DirtySet,
        StableAfterDirty,
        ChurnDestroy,
        ChurnRecreate,
        ChurnDrain,
        FinalStable,
        Complete,
        Failed
    };

    /** @brief Value-only fields needed to compare deterministic workload windows. */
    struct RenderingStressDiagnosticsSnapshot
    {
        bool available = false;
        bool sceneWorkAvailable = false;
        bool gpuSceneAvailable = false;
        bool extractionAvailable = false;
        uint64 sourceFrameSequence = 0;
        uint64 renderSceneAppliedRevision = 0;
        uint64 renderSceneRequiredRevision = 0;
        SampleRenderMutationEvidenceDiagnostics mutationEvidence{};
        uint32 extractionFullScanCount = 0;
        uint32 extractionChangeFeedChangeCount = 0;
        uint32 extractionActorRebuildCount = 0;
        uint32 extractionProxyVisitCount = 0;
        uint32 extractionComponentVisitCount = 0;
        uint32 extractionFeatureProviderVisitCount = 0;
        bool extractionContinuityLost = false;
        bool acceptedExtractionDiagnosticsAvailable = false;
        uint64 acceptedExtractionPublicationCount = 0;
        uint64 acceptedExtractionLastSourceFrameSequence = 0;
        uint64 acceptedExtractionLastSceneRevision = 0;
        uint64 acceptedExtractionCumulativeFullScanCount = 0;
        uint64 acceptedExtractionCumulativeChangeFeedChangeCount = 0;
        uint64 acceptedExtractionCumulativeActorRebuildCount = 0;
        uint64 acceptedExtractionCumulativeProxyVisitCount = 0;
        uint64 acceptedExtractionCumulativeComponentVisitCount = 0;
        uint64 acceptedExtractionCumulativeFeatureProviderVisitCount = 0;
        uint64 acceptedExtractionContinuityLossCount = 0;
        uint64 sceneFullRebuildCount = 0;
        uint64 sceneIncrementalUpdateCount = 0;
        uint64 sceneStaticReuseCount = 0;
        uint32 sceneLastRebuiltObjectCount = 0;
        uint32 sceneLastRemovedObjectCount = 0;
        uint64 drawPacketBuildCount = 0;
        uint64 drawPacketInvalidationCount = 0;
        uint64 drawPacketEntryCount = 0;
        uint32 renderSceneObjectCount = 0;
        uint32 gpuScenePublishedObjectCount = 0;
        uint32 gpuSceneAddCount = 0;
        uint32 gpuSceneUpdateCount = 0;
        uint32 gpuSceneRemoveCount = 0;
        uint32 gpuSceneNoOpCount = 0;
        uint64 gpuSceneFrameUploadBytes = 0;
        bool gpuSceneFullUpload = false;
        uint64 gpuDrivenInstanceUploadBytes = 0;
        uint64 gpuDrivenCandidateUploadBytes = 0;
        uint64 gpuDrivenActiveRowUploadBytes = 0;
        uint32 gpuDrivenActiveRowCount = 0;
        uint32 gpuDrivenActiveRowHighWatermark = 0;
        uint32 gpuDrivenInstancePatchedRowCount = 0;
        uint32 gpuDrivenCandidatePatchedRowCount = 0;
        uint32 gpuDrivenActiveRowPatchedRowCount = 0;
        uint64 gpuDrivenInstanceFullMaterializationCount = 0;
        uint64 gpuDrivenCandidateFullMaterializationCount = 0;
        uint64 gpuDrivenActiveRowFullMaterializationCount = 0;
        uint64 gpuDrivenContinuityFullMaterializationCount = 0;
        uint64 gpuDrivenCapacityFullMaterializationCount = 0;
        uint64 directRasterInstanceUploadBytes = 0;
        uint64 directRasterInstanceIndexUploadBytes = 0;
        uint32 directRasterInstancePatchedRowCount = 0;
        uint32 directRasterIndexPatchedRowCount = 0;
        uint32 directRasterActiveInstanceCount = 0;
        uint32 directRasterActiveInstanceCapacity = 0;
        uint32 directRasterInstanceFullMaterializationCount = 0;
        uint32 directRasterIndexFullMaterializationCount = 0;
    };

    /** @brief One bounded contiguous range in the deterministic churn index set. */
    struct RenderingStressChurnBatch
    {
        uint32 offset = 0;
        uint32 count = 0;
    };

    /** @brief The execution path proven by completed-frame diagnostics. */
    enum class RenderingStressExecutionPath : uint8
    {
        Unresolved = 0,
        Direct,
        GPUDriven
    };

    /** @brief Deterministic verdict for a single completed-frame window. */
    struct RenderingStressWindowEvaluation
    {
        bool passed = false;
        bool pending = false;
        std::string detail;
    };

    /**
     * @brief Stresses retained Scene extraction with exact dirty and churn sets.
     *
     * The sample deliberately has no backend identity, device, or command-list
     * knowledge. It requests one asynchronous Scene asset to retain the model
     * lease and then creates procedural ECS entities that reference immutable
     * mesh and material asset identities.
     */
    class RenderingStressSample final : public ISample
    {
    public:
        [[nodiscard]] const SampleInfo& GetInfo() const noexcept override;
        [[nodiscard]] SampleAssessmentContract
            GetAssessmentContract() const override;
        bool Setup(SampleContext& context, std::string& outError) override;
        void Update(SampleContext& context, float deltaTime) override;
        void OnInput(SampleContext& context) override;
        void OnViewportResize(SampleContext& context,
                              uint32 width,
                              uint32 height) override;
        void AppendReport(SampleFeatureReporter& reporter) const override;
        void ObserveDiagnostics(const SampleRenderDiagnostics& diagnostics,
                                SampleAssessmentChannel& assessment) override;
        SampleReadiness GetReadiness(
            const SampleRenderDiagnostics& diagnostics) const override;
        bool ValidateResult(const SampleRenderDiagnostics& diagnostics,
                            std::string& outError) const override;
        void Shutdown(SampleContext& context) override;

        /** @brief Return exactly @p selectedCount unique stable entity indices. */
        [[nodiscard]] static std::vector<uint32> BuildDeterministicIndexSet(
            uint32 objectCount,
            uint32 selectedCount,
            uint32 phaseOffset);

        /**
         * @brief Conservative per-frame churn bound for the retained Scene feed.
         *
         * Each procedural entity owns several ECS Fragments, so this is
         * intentionally well below the engine's bounded feed-retention budget.
         */
        [[nodiscard]] static constexpr uint32
            GetDeterministicChurnBatchSize() noexcept
        {
            return 256u;
        }

        /** @brief Partition an exact churn count into deterministic bounded ranges. */
        [[nodiscard]] static std::vector<RenderingStressChurnBatch>
            BuildChurnBatchPlan(uint32 churnObjectCount);

        /** @brief Map public sample path selection to the renderer-neutral policy. */
        [[nodiscard]] static RenderGPUDrivenMode MapRenderPath(
            SampleRenderPath path) noexcept;

        /** @brief Maximum later-frame slot propagation observations per churn batch. */
        [[nodiscard]] static constexpr uint32
            GetChurnDrainCompletedFrameLimit() noexcept
        {
            return RVX_MAX_FRAME_COUNT;
        }

        /** @brief Dirty action already proves one completion-safe stream slot. */
        [[nodiscard]] static constexpr uint32
            GetDirtyDrainCompletedFrameLimit() noexcept
        {
            return RVX_MAX_FRAME_COUNT - 1u;
        }

        /** @brief Bounded evidence retained while accepted action totals arrive. */
        [[nodiscard]] static constexpr uint32
            GetPendingActionReplayFrameLimit() noexcept
        {
            return RVX_MAX_FRAME_COUNT;
        }

        /** @brief Workload creation is forbidden until presentation and asset readiness. */
        [[nodiscard]] static constexpr bool CanBuildWorkload(
            RenderingStressPhase phase,
            bool firstFramePresented,
            bool sharedAssetReady) noexcept
        {
            return phase == RenderingStressPhase::InitialBuild &&
                   firstFramePresented && sharedAssetReady;
        }

        /** @brief True when value-only completed-frame diagnostic channels are published. */
        [[nodiscard]] static bool HasBaseInstrumentation(
            const SampleRenderDiagnostics& diagnostics) noexcept;
        /** @brief True while the completed RenderScene is older than the accepted Scene revision. */
        [[nodiscard]] static bool IsWaitingForRequiredSceneRevision(
            const SampleRenderDiagnostics& diagnostics) noexcept;
        /** @brief Accepted-only updates do not consume a static render window. */
        [[nodiscard]] static bool ShouldConsumeDiagnosticObservation(
            const RenderingStressDiagnosticsSnapshot& previous,
            const RenderingStressDiagnosticsSnapshot& current,
            bool actionRenderEvidencePending) noexcept;
        /** @brief Append one distinct later completed frame without dropping order. */
        [[nodiscard]] static RenderingStressWindowEvaluation
            AppendPendingActionDrainObservation(
                const RenderingStressDiagnosticsSnapshot& pinnedAction,
                std::vector<RenderingStressDiagnosticsSnapshot>& pending,
                const RenderingStressDiagnosticsSnapshot& current);
        /** @brief Preserve queued completed-frame provenance for later drain replay. */
        [[nodiscard]] static RenderingStressWindowEvaluation
            NormalizeActionDrainObservation(
                const RenderingStressDiagnosticsSnapshot& finalizedAction,
                const RenderingStressDiagnosticsSnapshot& queued,
                RenderingStressDiagnosticsSnapshot& outNormalized);

        /** @brief True when diagnostics also prove the selected nonempty execution stream. */
        [[nodiscard]] static bool HasRequiredInstrumentation(
            const SampleRenderDiagnostics& diagnostics,
            SampleRenderPath requestedPath = SampleRenderPath::Auto) noexcept;
        [[nodiscard]] static RenderingStressDiagnosticsSnapshot
            CaptureDiagnostics(const SampleRenderDiagnostics& diagnostics) noexcept;
        /** @brief True once all persistent populations exist, independent of delta timing. */
        [[nodiscard]] static bool HasCompleteInitialPopulation(
            const RenderingStressDiagnosticsSnapshot& snapshot,
            uint32 expectedObjectCount) noexcept;
        /** @brief True when the selected execution path copied no transient stream rows. */
        [[nodiscard]] static bool AreExecutionStreamsQuiescent(
            const RenderingStressDiagnosticsSnapshot& snapshot) noexcept;
        [[nodiscard]] static RenderingStressWindowEvaluation
            EvaluateStaticWindow(const RenderingStressDiagnosticsSnapshot& before,
                                 const RenderingStressDiagnosticsSnapshot& after);
        /** @brief Evaluate the initial retained window, allowing one exact temporal GPU-scene settle. */
        [[nodiscard]] static RenderingStressWindowEvaluation
            EvaluateInitialStaticWindow(
                const RenderingStressDiagnosticsSnapshot& before,
                const RenderingStressDiagnosticsSnapshot& after,
                uint32 expectedObjectCount,
                bool temporalSettleAlreadyObserved);
        [[nodiscard]] static RenderingStressWindowEvaluation
            EvaluateDirtyWindow(const RenderingStressDiagnosticsSnapshot& before,
                                const RenderingStressDiagnosticsSnapshot& after,
                                uint32 expectedDirtyCount,
                                uint32 expectedObjectCount,
                                uint32 expectedGPUDrivenOwnerCount,
                                uint32 expectedDirectRasterOwnerCount);
        /** @brief Compare dirty work across completion-qualified cumulative evidence. */
        [[nodiscard]] static RenderingStressWindowEvaluation
            EvaluateDirtyMutationEvidenceWindow(
                const RenderingStressDiagnosticsSnapshot& before,
                const RenderingStressDiagnosticsSnapshot& after,
                uint64 actionTargetSceneRevision,
                uint32 expectedDirtyCount,
                uint32 expectedObjectCount,
                uint32 expectedGPUDrivenOwnerCount,
                uint32 expectedDirectRasterOwnerCount);
        [[nodiscard]] static RenderingStressWindowEvaluation
            EvaluateDirtyDrainWindow(
                const RenderingStressDiagnosticsSnapshot& actionSnapshot,
                const RenderingStressDiagnosticsSnapshot& current,
                uint32 expectedDirtyCount,
                uint32 expectedObjectCount,
                uint32 expectedGPUDrivenOwnerCount,
                uint32 expectedDirectRasterOwnerCount,
                bool temporalSettleAlreadyObserved);
        [[nodiscard]] static RenderingStressWindowEvaluation
            EvaluateChurnRemoveWindow(
                const RenderingStressDiagnosticsSnapshot& before,
                const RenderingStressDiagnosticsSnapshot& after,
                uint32 expectedChurnCount,
                uint32 expectedBeforePopulation,
                uint32 expectedAfterPopulation,
                uint32 expectedGPUDrivenOwnerCount,
                uint32 expectedDirectRasterOwnerCount);
        /** @brief Compare destroy work across completion-qualified cumulative evidence. */
        [[nodiscard]] static RenderingStressWindowEvaluation
            EvaluateChurnRemoveMutationEvidenceWindow(
                const RenderingStressDiagnosticsSnapshot& before,
                const RenderingStressDiagnosticsSnapshot& after,
                uint64 actionTargetSceneRevision,
                uint32 expectedChurnCount,
                uint32 expectedBeforePopulation,
                uint32 expectedAfterPopulation,
                uint32 expectedGPUDrivenOwnerCount,
                uint32 expectedDirectRasterOwnerCount);
        [[nodiscard]] static RenderingStressWindowEvaluation
            EvaluateChurnAddWindow(const RenderingStressDiagnosticsSnapshot& before,
                                   const RenderingStressDiagnosticsSnapshot& after,
                                   uint32 expectedChurnCount,
                                   uint32 expectedBeforePopulation,
                                   uint32 expectedAfterPopulation,
                                   uint32 expectedGPUDrivenOwnerCount,
                                   uint32 expectedDirectRasterOwnerCount);
        /** @brief Compare recreate work across completion-qualified cumulative evidence. */
        [[nodiscard]] static RenderingStressWindowEvaluation
            EvaluateChurnAddMutationEvidenceWindow(
                const RenderingStressDiagnosticsSnapshot& before,
                const RenderingStressDiagnosticsSnapshot& after,
                uint64 actionTargetSceneRevision,
                uint32 expectedChurnCount,
                uint32 expectedBeforePopulation,
                uint32 expectedAfterPopulation,
                uint32 expectedGPUDrivenOwnerCount,
                uint32 expectedDirectRasterOwnerCount);
        [[nodiscard]] static RenderingStressWindowEvaluation
            EvaluateChurnDrainWindow(
                const RenderingStressDiagnosticsSnapshot& actionSnapshot,
                const RenderingStressDiagnosticsSnapshot& current,
                uint32 expectedPopulation,
                uint32 expectedBatchCount,
                uint32 expectedGPUDrivenOwnerCount,
                uint32 expectedDirectRasterOwnerCount,
                bool allowTemporalSettle = false,
                bool temporalSettleAlreadyObserved = false);

    private:
        [[nodiscard]] bool PrepareSharedAsset(SampleContext& context);
        [[nodiscard]] bool BuildInitialWorkload(SampleContext& context);
        [[nodiscard]] bool ApplyDirtySet(SampleContext& context);
        [[nodiscard]] bool QueueChurnDestroy(SampleContext& context);
        [[nodiscard]] bool AreChurnDestroyReceiptsApplied(SampleContext& context);
        [[nodiscard]] bool CompleteChurnDestroyBatch(SampleContext& context);
        [[nodiscard]] bool RecreateChurnEntities(SampleContext& context);
        [[nodiscard]] bool CompleteChurnRecreateBatch(SampleContext& context);
        [[nodiscard]] bool CaptureInitialExecutionOwnerCounts(
            SampleAssessmentChannel& assessment,
            const RenderingStressDiagnosticsSnapshot& snapshot);
        [[nodiscard]] bool ObserveChurnDrain(
            SampleAssessmentChannel& assessment,
            const RenderingStressDiagnosticsSnapshot& current);
        [[nodiscard]] bool ObserveDirtyDrain(
            SampleAssessmentChannel& assessment,
            const RenderingStressDiagnosticsSnapshot& current);
        [[nodiscard]] bool QueuePendingActionDrainObservation(
            SampleAssessmentChannel& assessment,
            const RenderingStressDiagnosticsSnapshot& current);
        [[nodiscard]] bool ReplayPendingDirtyDrain(
            SampleAssessmentChannel& assessment);
        [[nodiscard]] bool ReplayPendingChurnDrain(
            SampleAssessmentChannel& assessment);
        [[nodiscard]] bool BeginChurnDrain(
            SampleAssessmentChannel& assessment,
            RenderingStressPhase nextPhase,
            uint32 expectedPopulation,
            uint32 batchCount,
            const RenderingStressDiagnosticsSnapshot& actionBaseline,
            const RenderingStressDiagnosticsSnapshot& actionSnapshot,
            uint64 actionTargetSceneRevision,
            bool allowTemporalSettle);
        [[nodiscard]] bool ObserveQuiescentStaticWindow(
            SampleAssessmentChannel& assessment,
            const RenderingStressDiagnosticsSnapshot& current,
            bool allowInitialSlotWarmup);
        [[nodiscard]] bool CreateProceduralEntities(
            SampleContext& context,
            const std::vector<uint32>& entityIndices,
            std::vector<SceneECS::SceneEntityRef>& outEntities);
        void ConfigureCameraForWorkload(SampleContext& context);
        void PublishSnapshot(SampleAssessmentChannel& assessment,
                             const AssessmentCheckpoint& checkpoint,
                             const RenderingStressDiagnosticsSnapshot& snapshot) const;
        void PublishMutationActionEvidence(
            SampleAssessmentChannel& assessment,
            const AssessmentCheckpoint& checkpoint,
            const RenderingStressDiagnosticsSnapshot& baseline,
            const RenderingStressDiagnosticsSnapshot& completed,
            uint64 actionTargetSceneRevision) const;
        void PublishChurnTotals(SampleAssessmentChannel& assessment,
                                const AssessmentCheckpoint& checkpoint) const;
        void MarkInvariant(SampleAssessmentChannel& assessment,
                           const AssessmentInvariant& invariant,
                           const AssessmentCheckpoint& checkpoint) const;
        void ReportInstrumentationGap(SampleAssessmentChannel& assessment,
                                       std::string detail,
                                       bool publishCapabilityUnavailable = true);
        void Fail(SampleAssessmentChannel& assessment,
                  AssessmentCode invariantCode,
                  std::string message);
        void ReportIncomplete(SampleContext& context);
        [[nodiscard]] static const char* GetPhaseName(
            RenderingStressPhase phase) noexcept;

        LoadedSampleModel m_sharedModel;
        std::filesystem::path m_sharedModelPath;
        AssetId m_sharedMeshAssetId{};
        AssetId m_sharedMaterialAssetId{};
        SceneECS::MaterialSlots m_sharedMaterialSlots{};
        SceneECS::Bounds m_sharedMeshBounds{};
        SampleOrbitCameraController m_orbitCamera;
        SampleWorkloadProfile m_workload;
        RenderingStressPhase m_phase =
            RenderingStressPhase::WaitingForFirstPresentation;
        std::vector<SceneECS::SceneEntityRef> m_workloadEntities;
        std::vector<uint32> m_dirtyIndices;
        std::vector<uint32> m_churnIndices;
        std::vector<SceneECS::SceneCommandReceipt> m_churnDestroyReceipts;
        std::optional<RenderingStressDiagnosticsSnapshot> m_latestDiagnostics;
        std::optional<RenderingStressDiagnosticsSnapshot> m_windowBaseline;
        std::optional<RenderingStressDiagnosticsSnapshot> m_dirtyActionBaseline;
        std::optional<RenderingStressDiagnosticsSnapshot> m_churnBatchBaseline;
        std::optional<RenderingStressDiagnosticsSnapshot>
            m_churnBatchActionSnapshot;
        std::optional<RenderingStressDiagnosticsSnapshot> m_churnDrainBaseline;
        std::optional<RenderingStressDiagnosticsSnapshot>
            m_churnDrainActionBaseline;
        /** @brief Completion-covered action held while accepted extraction refreshes in the same frame. */
        std::optional<RenderingStressDiagnosticsSnapshot>
            m_pendingActionRenderEvidence;
        std::vector<RenderingStressDiagnosticsSnapshot>
            m_pendingActionDrainSnapshots;
        std::string m_failure;
        SampleRenderPath m_requestedRenderPath = SampleRenderPath::Auto;
        std::optional<RenderingStressExecutionPath> m_actualExecutionPath;
        bool m_firstFramePresented = false;
        bool m_sharedLoadQueued = false;
        bool m_sharedAssetReady = false;
        bool m_workloadBuilt = false;
        bool m_dirtyApplied = false;
        bool m_dirtyDeltaObserved = false;
        bool m_dirtyTemporalSettleObserved = false;
        bool m_initialTemporalSettleObserved = false;
        bool m_churnDestroyBatchQueued = false;
        bool m_churnDestroyReceiptsApplied = false;
        bool m_churnBatchRemoveObserved = false;
        bool m_churnRemoveObserved = false;
        bool m_churnRecreateBatchQueued = false;
        bool m_churnBatchAddObserved = false;
        uint32 m_churnDestroyCursor = 0;
        uint32 m_churnRecreateCursor = 0;
        uint32 m_churnBatchOffset = 0;
        uint32 m_churnBatchCount = 0;
        uint32 m_churnDestroyedCount = 0;
        uint32 m_churnRecreatedCount = 0;
        uint32 m_gpuDrivenInitialOwnerCount = 0;
        uint32 m_directRasterInitialOwnerCount = 0;
        uint32 m_churnDrainExpectedPopulation = 0;
        uint32 m_churnDrainBatchCount = 0;
        uint32 m_staticStreamWarmupFrameCount = 0;
        uint32 m_dirtyDrainCompletedFrameCount = 0;
        uint32 m_churnDrainCompletedFrameCount = 0;
        uint64 m_actionTargetSceneRevision = 0;
        uint64 m_churnDrainActionTargetSceneRevision = 0;
        RenderingStressPhase m_churnDrainNextPhase =
            RenderingStressPhase::Failed;
        bool m_churnDrainTemporalSettleAllowed = false;
        bool m_churnDrainTemporalSettleObserved = false;
        bool m_initialExecutionOwnerCountsCaptured = false;
        bool m_instrumentationObserved = false;
        bool m_incompleteReported = false;
        bool m_cameraConfigured = false;

        static_assert(RVX_MAX_FRAME_COUNT > 1u,
                      "Rendering-stress action drains require at least two frame slots.");
    };
} // namespace RVX
