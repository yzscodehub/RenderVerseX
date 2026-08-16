#pragma once

/**
 * @file AnimationCharacterSample.h
 * @brief Pure-ECS fixed-step animation, skinning, root-motion, and Physics qualification.
 */

#include "Core/Math/AABB.h"
#include "Engine/ECS/IWorldEcsRuntimeServices.h"
#include "RenderContracts/RenderFrameTypes.h"
#include "Samples/ModelCameraFraming.h"
#include "Samples/Sample.h"
#include "Samples/SampleAnimationLoader.h"
#include "Samples/SampleCLI.h"
#include "Samples/SampleModelLoader.h"
#include "Samples/SampleOrbitCameraController.h"
#include "Scene/ECS/AnimationFragments.h"
#include "Scene/ECS/PhysicsFragments.h"

#include <array>
#include <string>

namespace RVX
{
    class AnimationCharacterSampleValidationAccess;

    /** @brief Product sample that qualifies the complete pure-ECS character path. */
    class AnimationCharacterSample final : public ISample
    {
    public:
        [[nodiscard]] const SampleInfo& GetInfo() const noexcept override;
        [[nodiscard]] SampleWorldRequirements GetWorldRequirements() const override;
        [[nodiscard]] SampleAssessmentContract GetAssessmentContract() const override;

        bool Setup(SampleContext& context, std::string& outError) override;
        void Update(SampleContext& context, float deltaTime) override;
        void OnInput(SampleContext& context) override;
        void OnViewportResize(SampleContext& context,
                              uint32 width,
                              uint32 height) override;
        void AppendReport(SampleFeatureReporter& reporter) const override;
        void ObserveDiagnostics(const SampleRenderDiagnostics& diagnostics,
                                SampleAssessmentChannel& assessment) override;
        [[nodiscard]] SampleReadiness GetReadiness(
            const SampleRenderDiagnostics& diagnostics) const override;
        [[nodiscard]] bool ShouldBeginFinalRenderDrain(
            const SampleRenderDiagnostics& diagnostics) const override;
        bool ValidateResult(const SampleRenderDiagnostics& diagnostics,
                            std::string& outError) const override;
        void Shutdown(SampleContext& context) override;

    private:
        enum class State : uint8
        {
            AwaitAssets = 0,
            AwaitBridgeReconcile,
            QualifyingFixedSteps,
            AwaitPresentedPalette,
            Stable,
            Failed,
        };

        enum class ScenarioAction : uint8
        {
            BindEcsCharacter = 0,
            EvaluateFixed120,
            ConsumeRootMotion120,
            PresentSkinningPalette,
            Count,
        };

        struct ScenarioActionEvidence
        {
            const char* name = "";
            uint64 targetSceneRevision = 0;
            uint64 minimumPresentationSequence = 0;
            uint64 completedPresentationSequence = 0;
            uint64 appliedSceneRevision = 0;
            bool prerequisitesSatisfied = false;
            bool passed = false;
        };

        [[nodiscard]] bool ConfigureCharacter(SampleContext& context);
        [[nodiscard]] bool ConfigurePresentation(SampleContext& context);
        [[nodiscard]] static bool ResolveGPUCullingMode(
            SampleRenderPath renderPath,
            RenderGPUDrivenMode& outMode) noexcept;
        [[nodiscard]] bool TryBindRootMotionPhysics(SampleContext& context);
        [[nodiscard]] bool CompleteFixedQualification(SampleContext& context);
        [[nodiscard]] bool ObserveQualifiedRootMotionIntent(
            const SceneECS::RootMotionIntent& intent,
            std::string& outError);
        [[nodiscard]] bool HasExactPresentedPalette(
            const SampleRenderDiagnostics& diagnostics,
            SamplePresentedSkinningPaletteReceipt& outReceipt) const;
        [[nodiscard]] bool IsScenarioActionPresentationCovered(
            const SampleRenderDiagnostics& diagnostics,
            const ScenarioActionEvidence& action) const;
        [[nodiscard]] bool AreAllScenarioActionsPresented() const noexcept;
        [[nodiscard]] static const char* GetScenarioActionName(
            ScenarioAction action) noexcept;
        void MarkScenarioActionPrerequisite(ScenarioAction action);
        void BindScenarioActionToExactPresentation(
            ScenarioAction action,
            uint64 sceneRevision,
            uint64 presentationSequence);
        void CompleteScenarioActions(const SampleRenderDiagnostics& diagnostics,
                                     SampleAssessmentChannel& assessment);
        void PublishStableAssessment(SampleAssessmentChannel& assessment);
        void Fail(SampleContext& context,
                  AssessmentCode invariant,
                  std::string message);
        void Fail(SampleAssessmentChannel& assessment,
                  AssessmentCode invariant,
                  std::string message);

        LoadedSampleModel m_characterModel;
        LoadedSampleModel m_gpuProbeModel;
        LoadedSampleAnimation m_rootMotionAnimation;
        SceneECS::SceneEntityRef m_characterRoot;
        SceneECS::SceneEntityRef m_modelPoseSource;
        SceneECS::SceneEntityRef m_motionDriver;
        AABB m_presentationBounds;
        ModelCameraFrame m_cameraFrame;
        SampleOrbitCameraController m_orbitCamera;
        SamplePresentedSkinningPaletteReceipt m_presentedPalette;
        std::array<ScenarioActionEvidence,
                   static_cast<size_t>(ScenarioAction::Count)> m_scenarioActions{};
        WorldEcsRuntimeServicesDiagnostics m_baselineServices;
        WorldEcsRuntimeServicesDiagnostics m_lastServices;
        Vec3 m_rootMotionStart{0.0f};
        Vec3 m_rootMotionEnd{0.0f};
        uint64 m_baselinePoseSequence = 0;
        uint64 m_finalPoseSequence = 0;
        uint64 m_completedFixedSteps = 0;
        uint64 m_completedRootMotionSteps = 0;
        uint64 m_qualificationPhysicsBodyHandlePacked = 0;
        uint64 m_lastQualifiedRootMotionSequence = 0;
        uint64 m_observedQualifiedRootMotionIntentCount = 0;
        uint64 m_lastObservedPresentationSequence = 0;
        float32 m_rootMotionDistance = 0.0f;
        State m_state = State::AwaitAssets;
        SampleRenderPath m_renderPath = SampleRenderPath::Direct;
        std::string m_failure;
        bool m_characterConfigured = false;
        bool m_rootMotionBound = false;
        bool m_stableAssessmentPublished = false;

        friend class AnimationCharacterSampleValidationAccess;
    };
} // namespace RVX
