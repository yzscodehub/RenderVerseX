#pragma once

/**
 * @file SceneLifecycleSample.h
 * @brief Deterministic pure-ECS Scene lifecycle architecture probe.
 */

#include "Core/Math/AABB.h"
#include "Resource/ResourceDiagnostics.h"
#include "Samples/ModelCameraFraming.h"
#include "Samples/Sample.h"
#include "Samples/SampleModelLoader.h"
#include "Samples/SampleOrbitCameraController.h"
#include "Scene/ECS/RenderFragments.h"
#include "Scene/ECS/SceneEcsRuntime.h"
#include "World/ECS/WorldEcsCameraService.h"

#include <array>
#include <string>

namespace RVX
{
    /**
     * @brief Exercises ECS command barriers, entity generations, hierarchy,
     * camera selection, fragment writes, and cleanup-gated retirement.
     *
     * The sample retains only scene-qualified entity refs, command receipts,
     * copied fragment values, and loader status snapshots between frames.
     */
    class SceneLifecycleSample final : public ISample
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
        [[nodiscard]] bool ShouldBeginFinalRenderDrain(
            const SampleRenderDiagnostics& diagnostics) const override;
        bool ValidateResult(const SampleRenderDiagnostics& diagnostics,
                            std::string& outError) const override;
        void Shutdown(SampleContext& context) override;

    private:
        enum class ScenarioState : uint8
        {
            AwaitSpawn = 0,
            ReparentKeepLocal,
            ReparentKeepWorld,
            RecycleSecondaryCamera,
            PresentSecondaryCamera,
            WaitingForSecondaryPresentation,
            RestorePrimaryCamera,
            WaitingForPrimaryPresentation,
            MutateLight,
            MutateMaterialSlots,
            DestroyAndReuseEntitySlot,
            WaitingForStablePresentation,
            Stable,
            Failed
        };

        enum class ScenarioAction : uint8
        {
            Spawn = 0,
            ReparentKeepLocal,
            ReparentKeepWorld,
            RecycleSecondaryCamera,
            PresentSecondaryCamera,
            RestorePrimaryCamera,
            MutateLight,
            MutateMaterialSlots,
            DestroyAndReuseEntitySlot,
            Count
        };

        struct ScenarioActionEvidence
        {
            const char* name = "";
            uint64 targetSceneRevision = 0;
            uint64 minimumPresentationSequence = 0;
            uint64 completedPresentationSequence = 0;
            uint64 appliedSceneRevision = 0;
            bool passed = false;
        };

        [[nodiscard]] bool AdvanceInitialSpawns(SampleContext& context);
        [[nodiscard]] bool AdvanceKeepLocal(SampleContext& context);
        [[nodiscard]] bool AdvanceKeepWorld(SampleContext& context);
        [[nodiscard]] bool AdvanceSecondaryCameraRecycle(SampleContext& context);
        [[nodiscard]] bool PresentSecondaryCamera(SampleContext& context);
        [[nodiscard]] bool RestorePrimaryCamera(SampleContext& context);
        [[nodiscard]] bool MutateLight(SampleContext& context);
        [[nodiscard]] bool MutateMaterialSlots(SampleContext& context);
        [[nodiscard]] bool AdvanceDestroyAndReuse(SampleContext& context);
        [[nodiscard]] bool ConfigureModelPresentation(SampleContext& context);
        [[nodiscard]] bool ConfigureSecondaryCamera(SampleContext& context,
                                                     WorldECS::WorldEcsCameraRef camera);
        [[nodiscard]] bool QueueReparent(SampleContext& context,
                                         SceneECS::ReparentMode mode,
                                         SceneECS::SceneCommandReceipt& outReceipt,
                                         SceneECS::SceneCommandBufferReceipt& outBufferReceipt);
        [[nodiscard]] bool QueueFragmentWrite(
            SampleContext& context,
            SceneECS::SceneCommandReceipt& outReceipt,
            SceneECS::SceneCommandBufferReceipt& outBufferReceipt,
            const SceneECS::Light& light);
        [[nodiscard]] bool QueueMaterialSlotWrite(SampleContext& context);
        [[nodiscard]] bool ValidateAppliedReceipt(
            const SceneECS::SceneCommandReceipt& receipt,
            const SceneECS::SceneCommandBufferReceipt& bufferReceipt,
            std::string& outError) const;
        [[nodiscard]] bool ValidateHierarchyAuthority(
            const SampleContext& context,
            SceneECS::SceneEntityRef expectedParent) const;
        [[nodiscard]] bool IsActionPresentationCovered(
            const SampleRenderDiagnostics& diagnostics,
            const ScenarioActionEvidence& action) const;
        [[nodiscard]] bool AreTerminalQueuesDrained(
            const SampleRenderDiagnostics& diagnostics) const;
        [[nodiscard]] bool IsRenderPathQualified(
            const SampleRenderDiagnostics& diagnostics) const;
        [[nodiscard]] bool HasExactFinalPopulation(
            const SampleContext& context) const;
        [[nodiscard]] bool AreAllActionsPresented() const noexcept;
        void CaptureTerminalDiagnostics(SampleContext& context);
        void CompleteAction(ScenarioAction action,
                            const SampleRenderDiagnostics& diagnostics,
                            SampleAssessmentChannel& assessment);
        void MarkInvariant(SampleAssessmentChannel& assessment,
                           AssessmentCode code,
                           std::string description);
        void SetActionTarget(ScenarioAction action,
                             bool requiresSceneMutation = true);
        [[nodiscard]] ScenarioActionEvidence& GetAction(
            ScenarioAction action) noexcept;
        [[nodiscard]] const ScenarioActionEvidence& GetAction(
            ScenarioAction action) const noexcept;
        void Fail(SampleAssessmentChannel& assessment,
                  AssessmentCode invariantCode,
                  std::string message);
        void Fail(SampleContext& context,
                  AssessmentCode invariantCode,
                  std::string message);

        LoadedSampleModel m_model;
        AABB m_presentationBounds;
        ModelCameraFrame m_cameraFrame;
        SampleOrbitCameraController m_orbitCamera;
        SampleRenderPath m_renderPath = SampleRenderPath::Auto;

        ScenarioState m_state = ScenarioState::AwaitSpawn;
        std::string m_failure;
        std::array<ScenarioActionEvidence,
                   static_cast<size_t>(ScenarioAction::Count)>
            m_actionEvidence{};

        SceneECS::SceneEntityRef m_parentA;
        SceneECS::SceneEntityRef m_parentB;
        SceneECS::SceneEntityRef m_child;
        SceneECS::SceneEntityRef m_light;
        SceneECS::SceneEntityRef m_replacement;
        SceneECS::SceneEntityRef m_materialEntity;
        WorldECS::WorldEcsCameraRef m_secondaryCamera;
        WorldECS::WorldEcsCameraRef m_retiredSecondaryCamera;

        SceneECS::SceneEntityReceipt m_spawnParentA;
        SceneECS::SceneEntityReceipt m_spawnParentB;
        SceneECS::SceneEntityReceipt m_spawnChild;
        SceneECS::SceneEntityReceipt m_spawnLight;
        SceneECS::SceneCommandReceipt m_spawnLightFragment;
        SceneECS::SceneCommandReceipt m_spawnLightVisibility;
        SceneECS::SceneCommandBufferReceipt m_spawnBuffer;
        SceneECS::SceneCommandReceipt m_keepLocal;
        SceneECS::SceneCommandBufferReceipt m_keepLocalBuffer;
        SceneECS::SceneCommandReceipt m_keepWorld;
        SceneECS::SceneCommandBufferReceipt m_keepWorldBuffer;
        SceneECS::SceneCommandReceipt m_lightMutation;
        SceneECS::SceneCommandBufferReceipt m_lightMutationBuffer;
        SceneECS::SceneCommandReceipt m_materialMutation;
        SceneECS::SceneCommandBufferReceipt m_materialMutationBuffer;

        SceneECS::LocalTransform m_childLocalBeforeKeepLocal;
        Mat4 m_childWorldBeforeKeepWorld{1.0f};
        SceneECS::MaterialSlots m_materialSlotsBefore;
        SceneECS::MaterialSlots m_materialSlotsAfter;

        uint64 m_primaryCutBefore = 0;
        uint64 m_primaryCameraIdentity = 0;
        uint64 m_secondaryCameraPresentationBaseline = 0;
        uint64 m_primaryCameraPresentationBaseline = 0;
        uint64 m_lastObservedPresentationSequence = 0;
        uint64 m_lastObservedRenderSceneRevision = 0;
        uint64 m_lightWriteVersionBeforeMutation = 0;
        uint64 m_lightWriteVersionAfterMutation = 0;
        uint64 m_materialWriteVersionBeforeMutation = 0;
        uint64 m_materialWriteVersionAfterMutation = 0;
        uint64 m_renderLightStateHashBefore = 0;
        uint64 m_lastObservedRenderLightStateHash = 0;
        uint64 m_terminalDiagnosticsCapturePresentationSequence = 0;
        uint32 m_expectedFinalOwnedEntityCount = 0;
        uint32 m_finalOwnedEntityCount = 0;
        uint32 m_lastObservedRenderPendingUploadCount = 0;
        uint32 m_lastObservedRenderRetirementEntryCount = 0;
        SceneECS::SceneEcsDiagnosticsSnapshot m_terminalSceneDiagnostics{};
        Resource::ResourceDiagnosticsSnapshot m_terminalResourceDiagnostics{};

        bool m_spawnSubmitted = false;
        bool m_reparentKeepLocalQueued = false;
        bool m_reparentKeepWorldQueued = false;
        bool m_secondaryCameraDestroyRequested = false;
        bool m_secondaryCameraReplacementCreated = false;
        bool m_lightMutationQueued = false;
        bool m_materialMutationQueued = false;
        bool m_childDestroyRequested = false;
        bool m_renderLightBaselineCaptured = false;
        bool m_childPendingDestroyObserved = false;
        bool m_terminalSceneDiagnosticsCaptured = false;
        bool m_terminalResourceDiagnosticsAvailable = false;
        bool m_terminalQueuesDrained = false;
        bool m_presentationReady = false;
        bool m_assessmentActionAppliedMarked = false;
        std::string m_terminalResourceDiagnosticsReason;
    };
} // namespace RVX
