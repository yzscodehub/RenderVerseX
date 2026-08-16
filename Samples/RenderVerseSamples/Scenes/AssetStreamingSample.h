#pragma once

/**
 * @file AssetStreamingSample.h
 * @brief Deterministic asynchronous asset-lifetime architecture probe.
 */

#include "Core/Math/AABB.h"
#include "Resource/ResourceDiagnostics.h"
#include "Resource/ResourcePublicationView.h"
#include "Samples/ModelCameraFraming.h"
#include "Samples/Sample.h"
#include "Samples/SampleModelLoader.h"
#include "Samples/SampleOrbitCameraController.h"

#include <array>
#include <optional>
#include <string>
#include <unordered_map>

namespace RVX
{
    /**
     * @brief Exercises catalog-bound asynchronous residency, typed rejection,
     * cancellation, retirement, and reload through public services.
     */
    class AssetStreamingSample final : public ISample
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

    private:
        enum class ScenarioState : uint8
        {
            QueueThreeModelsAndDuplicateSubscriber = 0,
            PresentFallbackFrame,
            VerifyRequestCoalescing,
            CancelDuplicateSubscriber,
            RequestUnknownAssetAndRollback,
            InstantiateMinimumResidentModels,
            CommitRealTextures,
            CancelPublishedLargeModel,
            AwaitExactRetirement,
            ReloadSameAssetId,
            PresentReloadedRevision,
            DrainAllQueues,
            Stable,
            Failed
        };

        enum class ScenarioAction : uint8
        {
            QueueThreeModelsAndDuplicateSubscriber = 0,
            PresentFallbackFrame,
            VerifyRequestCoalescing,
            CancelDuplicateSubscriber,
            RequestUnknownAssetAndRollback,
            InstantiateMinimumResidentModels,
            CommitRealTextures,
            CancelPublishedLargeModel,
            AwaitExactRetirement,
            ReloadSameAssetId,
            PresentReloadedRevision,
            DrainAllQueues,
            Count
        };

        struct ScenarioActionEvidence
        {
            uint64 targetSourceFrameSequence = 0;
            /**
             * @brief The last completed presentation observed before the
             * action became eligible.  A stale frame may never discharge an
             * asynchronous action.
             */
            uint64 minimumPresentationSequence = 0;
            uint64 completedPresentationSequence = 0;
            uint64 appliedSceneRevision = 0;
            bool requested = false;
            bool completionGateSatisfied = false;
            bool assessmentReported = false;
        };

        struct ResourceQueueSnapshot
        {
            uint64 activeOperations = 0;
            uint64 activeSubscribers = 0;
            uint64 pendingAsyncJobs = 0;
            uint64 pendingAsyncCompletions = 0;
            uint64 decodeQueuedCount = 0;
            uint64 decodeActiveCount = 0;
            uint64 cacheEntryCount = 0;
            uint64 pendingPublicationCount = 0;
            uint64 pendingUploadCount = 0;
            uint64 pendingReplacementCount = 0;
            uint64 pendingRollbackCount = 0;
            uint64 pendingRetirementCount = 0;
            uint64 queuedLeaseUnloadCount = 0;
            uint64 closureUnloadRequestCount = 0;
        };

        /** @brief Exact presented fallback-to-ready proof for one streamed texture. */
        struct StreamingTextureReceiptEvidence
        {
            RenderResourceHandle texture{};
            uint64 fallbackCommittedContentRevision = 0;
            uint64 fallbackPresentationSequence = 0;
            uint64 fallbackDescriptorContentKey = 0;
            uint64 fallbackDescriptorRevision = 0;
            uint64 readyCommittedContentRevision = 0;
            uint64 readyPresentationSequence = 0;
            uint64 readyDescriptorContentKey = 0;
            uint64 readyDescriptorRevision = 0;
            uint64 readyNoRebuildPresentationSequence = 0;

            [[nodiscard]] bool HasFallbackReceipt() const noexcept
            {
                return texture.IsValid() &&
                       fallbackCommittedContentRevision != 0 &&
                       fallbackPresentationSequence != 0 &&
                       fallbackDescriptorContentKey != 0 &&
                       fallbackDescriptorRevision != 0;
            }

            [[nodiscard]] bool HasReadyReceipt() const noexcept
            {
                return HasFallbackReceipt() &&
                       readyCommittedContentRevision >
                           fallbackCommittedContentRevision &&
                       readyPresentationSequence > fallbackPresentationSequence &&
                        readyDescriptorContentKey !=
                            fallbackDescriptorContentKey &&
                        readyDescriptorRevision > fallbackDescriptorRevision &&
                        readyNoRebuildPresentationSequence ==
                            readyPresentationSequence;
            }
        };

        enum class TexturePublicationObservation : uint8
        {
            Pending = 0,
            Committed,
            Invalid
        };

        [[nodiscard]] bool ConfigureDefaultScene(SampleContext& context,
                                                  std::string& outError);
        [[nodiscard]] bool QueueRequests(SampleContext& context,
                                         std::string& outError);
        [[nodiscard]] bool UpdateModelReadiness(SampleContext& context);
        [[nodiscard]] bool PresentFallbackFrame(SampleContext& context);
        [[nodiscard]] bool VerifyRequestCoalescing(SampleContext& context);
        [[nodiscard]] bool CancelDuplicateSubscriber(SampleContext& context);
        [[nodiscard]] bool RequestUnknownAssetAndRollback(SampleContext& context);
        [[nodiscard]] bool InstantiateMinimumResidentModels(SampleContext& context);
        [[nodiscard]] bool CommitRealTextures(SampleContext& context);
        [[nodiscard]] bool CancelPublishedLargeModel(SampleContext& context);
        [[nodiscard]] bool AwaitExactRetirement(SampleContext& context);
        [[nodiscard]] bool ReloadSameAssetId(SampleContext& context);
        [[nodiscard]] bool PresentReloadedRevision(SampleContext& context);
        [[nodiscard]] bool DrainAllQueues(SampleContext& context);
        [[nodiscard]] bool UpdateDiagnostics(SampleContext& context,
                                             bool requireClosureProgress,
                                             bool requireStableQueues);
        [[nodiscard]] bool PlaceMinimumResidentModels(SampleContext& context);
        [[nodiscard]] bool PlaceReloadedLargeModel(SampleContext& context);
        [[nodiscard]] bool PlaceModelForPresentation(
            SampleContext& context,
            LoadedSampleModel& model,
            const Vec3& displayAnchor,
            float32 targetExtent,
            std::string_view label,
            AABB& outBounds,
            std::string& outError);
        [[nodiscard]] bool InitializePresentationCamera(
            SampleContext& context,
            const AABB& bounds,
            std::string& outError);
        [[nodiscard]] bool ReframePresentationCamera(
            SampleContext& context,
            const AABB& bounds,
            std::string& outError);
        [[nodiscard]] static bool IsNormalizedGroundedPlacement(
            const AABB& sourceBounds,
            const AABB& placedBounds,
            float32 targetExtent) noexcept;
        [[nodiscard]] static bool ArePresentationBoundsEquivalent(
            const AABB& expected,
            const AABB& actual) noexcept;
        [[nodiscard]] static bool ShouldConsumeOrbitInput(
            bool smoke,
            bool presentationReady,
            bool orbitInitialized,
            bool inputAvailable) noexcept;
        [[nodiscard]] bool ObservePrimaryTexturePublication(
            SampleContext& context);
        void ObservePrimaryTextureReceipts(
            const SampleRenderDiagnostics& diagnostics);
        [[nodiscard]] bool AreInitialModelsCPUReady() const noexcept;
        [[nodiscard]] bool AreInitialModelsFullyResident() const noexcept;
        [[nodiscard]] bool IsActionCompletionGateSatisfied(
            ScenarioAction action) const noexcept;
        [[nodiscard]] bool IsActionPresentationCovered(
            const SampleRenderDiagnostics& diagnostics,
            const ScenarioActionEvidence& evidence) const noexcept;
        [[nodiscard]] static bool IsFallbackBaselineEligible(
            bool minimumResidentPresented,
            bool fullyResident) noexcept;
        [[nodiscard]] static TexturePublicationObservation
            ClassifyTexturePublication(
                const Resource::ResourcePublicationQueryResult& publication)
                noexcept;
        /**
         * @brief Require an exact completed/presented frame with zero Scene
         *        object rebuild/removal work before accepting a ready texture
         *        descriptor receipt.
         */
        [[nodiscard]] static bool IsReadyTextureReceiptFrameQualified(
            const SampleRenderDiagnostics& diagnostics,
            uint64 receiptFrameSequence,
            uint64 receiptPresentationSequence) noexcept;
        [[nodiscard]] static bool IsReloadInstanceReady(
            bool fullyResident,
            bool instanceValid,
            bool rootValid,
            uint64 sceneRevision) noexcept;
        [[nodiscard]] static bool IsRequestedRenderPathExecuted(
            SampleRenderPath requestedPath,
            const SampleRenderDiagnostics& diagnostics) noexcept;
        [[nodiscard]] bool IsActionPresented(ScenarioAction action) const noexcept;
        [[nodiscard]] static size_t GetScenarioActionIndex(
            ScenarioAction action) noexcept;
        [[nodiscard]] static const char* GetScenarioActionName(
            ScenarioAction action) noexcept;
        [[nodiscard]] static const AssessmentAction& GetAssessmentAction(
            ScenarioAction action) noexcept;
        void RecordScenarioAction(SampleContext& context,
                                  ScenarioAction action);
        void ObserveActionPresentations(
            const SampleRenderDiagnostics& diagnostics,
            SampleAssessmentChannel& assessment);
        [[nodiscard]] static ResourceQueueSnapshot MakeQueueSnapshot(
            const Resource::ResourceDiagnosticsSnapshot& diagnostics) noexcept;
        /**
         * @brief True only when every asynchronous/retirement terminal queue
         *        is empty. Cache and active residency holds are deliberately
         *        excluded because the final reloaded asset remains live.
         */
        [[nodiscard]] static bool AreTerminalQueuesDrained(
            const ResourceQueueSnapshot& snapshot) noexcept;
        /**
         * @brief Verify that the completion receipt belongs to the exact
         *        cancelled Sponza lifecycle before a same-id reload may start.
         */
        [[nodiscard]] static bool IsExactOldSponzaRetirementSatisfied(
            bool cancellationValid,
            bool oldLifecycleHandleValid,
            bool oldResourceRequestValid,
            bool cancellationResourceRequestValid,
            bool lifecycleHandleMatches,
            bool resourceRequestMatches,
            bool retirementComplete) noexcept;
        void MarkInvariant(SampleContext& context,
                           const AssessmentInvariant& invariant,
                           AssessmentCheckpoint checkpoint);
        void ReportUnavailable(SampleContext& context,
                               AssessmentCode code,
                               std::string detail);
        void Fail(SampleContext& context,
                  AssessmentCode invariantCode,
                  std::string message);
        void CancelIfLive(SampleContext& context,
                          LoadedSampleModel& model) noexcept;

        friend struct AssetStreamingSampleTestAccess;

        LoadedSampleModel m_primary;
        LoadedSampleModel m_duplicateSubscriber;
        LoadedSampleModel m_corset;
        LoadedSampleModel m_largeModel;
        LoadedSampleModel m_reloadedLargeModel;
        SampleModelCancellation m_duplicateCancellation;
        SampleModelCancellation m_largeCancellation;
        std::unordered_map<Resource::ResourceId,
                           StreamingTextureReceiptEvidence>
            m_primaryTextureEvidence;
        std::array<ScenarioActionEvidence,
                   static_cast<size_t>(ScenarioAction::Count)> m_actions;
        std::optional<Resource::ResourceContentVerificationReceipt>
            m_largeContentIdentity;
        ResourceQueueSnapshot m_lastQueueSnapshot;
        ResourceSceneAdapters::EcsModelAssetLoadRef m_largeLifecycleHandle;
        Resource::ResourceLoadRequestId m_largeResourceRequestId;
        ResourceSceneAdapters::EcsModelAssetLoadRef m_reloadedLargeLifecycleHandle;

        ScenarioState m_state =
            ScenarioState::QueueThreeModelsAndDuplicateSubscriber;
        SampleRenderPath m_renderPath = SampleRenderPath::Auto;
        uint64 m_initialClosureUnloadRequests = 0;
        uint64 m_expectedClosureUnloadRequests = 0;
        uint64 m_fullSceneRebuildCountAtFallback = 0;
        uint64 m_fullSceneRebuildCountAtTextureCommit = 0;
        uint64 m_lastObservedPresentationSequence = 0;
        uint64 m_primaryFallbackPresentationSequence = 0;
        uint64 m_primaryFallbackRenderSceneRevision = 0;
        uint64 m_primaryTextureCommitRevision = 0;
        uint64 m_reloadedSceneRevision = 0;
        uint32 m_updateCount = 0;
        uint32 m_placedModelCount = 0;
        AABB m_presentationBounds;
        AABB m_initialLargePresentationBounds;
        AABB m_reloadedLargePresentationBounds;
        ModelCameraFrame m_cameraFrame;
        SampleOrbitCameraController m_orbitCamera;
        bool m_duplicateCancellationRequested = false;
        bool m_duplicateCancelled = false;
        bool m_unknownRequestRejected = false;
        bool m_unknownRollbackClean = false;
        bool m_coalescedRequestObserved = false;
        bool m_primaryMinimumBeforeFullyResident = false;
        bool m_primaryTextureFallbackBaselineFrozen = false;
        bool m_primaryTextureCommitted = false;
        bool m_textureSceneBaselineCaptured = false;
        bool m_textureSceneCommitCaptured = false;
        bool m_noFullSceneRebuildForTextureCommit = false;
        bool m_largeCancellationCompleted = false;
        bool m_exactOldSponzaRetirementSatisfied = false;
        bool m_reloadedFullyResident = false;
        bool m_reloadedInstanceValid = false;
        bool m_reloadedRootValid = false;
        bool m_reloadedIdentityMatches = false;
        bool m_reloadedLifecycleGenerationAdvanced = false;
        bool m_reloadedPresentationPlaced = false;
        bool m_closureProgressObserved = false;
        bool m_stableQueuesObserved = false;
        bool m_resourceDiagnosticsCapabilityObserved = false;
        bool m_texturePublicationCapabilityObserved = false;
        bool m_skyboxCreated = false;
        bool m_lightCreated = false;
        std::string m_failure;
    };
} // namespace RVX
