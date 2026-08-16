#include "Scenes/AnimationCharacterSample.h"

#include "Core/Log.h"
#include "Samples/SampleCLI.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string_view>

namespace RVX
{
    /** @brief Narrow value-only test view; no legacy runtime object is exposed. */
    class AnimationCharacterSampleValidationAccess final
    {
    public:
        static void SetAwaitingPalette(AnimationCharacterSample& sample,
                                       uint64 sourceModelAssetValue,
                                       uint64 expectedPoseSequence = 17)
        {
            sample.m_state = AnimationCharacterSample::State::AwaitPresentedPalette;
            sample.m_characterModel.status.modelMetadata.sourceModelAssetId =
                AssetId{sourceModelAssetValue};
            sample.m_finalPoseSequence = expectedPoseSequence;
        }

        static void SetStable(AnimationCharacterSample& sample,
                              SamplePresentedSkinningPaletteReceipt receipt)
        {
            sample.m_state = AnimationCharacterSample::State::Stable;
            sample.m_presentedPalette = receipt;
            for (uint32 index = 0;
                 index < static_cast<uint32>(
                             AnimationCharacterSample::ScenarioAction::Count);
                 ++index)
            {
                auto& action = sample.m_scenarioActions[index];
                action.name = AnimationCharacterSample::GetScenarioActionName(
                    static_cast<AnimationCharacterSample::ScenarioAction>(index));
                action.targetSceneRevision = 1;
                action.completedPresentationSequence = 1;
                action.appliedSceneRevision = 1;
                action.prerequisitesSatisfied = true;
                action.passed = true;
            }
        }

        static void SetFailure(AnimationCharacterSample& sample, std::string failure)
        {
            sample.m_state = AnimationCharacterSample::State::Failed;
            sample.m_failure = std::move(failure);
        }

        static bool FindPalette(
            const AnimationCharacterSample& sample,
            const SampleRenderDiagnostics& diagnostics,
            SamplePresentedSkinningPaletteReceipt& outReceipt)
        {
            return sample.HasExactPresentedPalette(diagnostics, outReceipt);
        }

        static bool ResolveGPUCullingMode(SampleRenderPath renderPath,
                                          RenderGPUDrivenMode& outMode)
        {
            return AnimationCharacterSample::ResolveGPUCullingMode(
                renderPath, outMode);
        }

        static bool ObserveQualifiedRootMotionIntent(
            AnimationCharacterSample& sample,
            uint64 sequence,
            uint64 bodyHandle,
            std::string& outError)
        {
            SceneECS::RootMotionIntent intent;
            intent.sceneRuntimeIdValue = 1;
            intent.targetEntity = ECS::EntityHandle::Create(0u, 0u);
            intent.physicsBodyHandlePacked = bodyHandle;
            intent.rootMotionSequence = sequence;
            sample.m_motionDriver = {
                ECS::SceneRuntimeId{1u}, ECS::EntityHandle::Create(0u, 0u)};
            return sample.ObserveQualifiedRootMotionIntent(intent, outError);
        }

        static void SetQualifiedStableScenario(
            AnimationCharacterSample& sample,
            SamplePresentedSkinningPaletteReceipt receipt)
        {
            sample.m_state = AnimationCharacterSample::State::Stable;
            sample.m_characterModel.status.modelMetadata.sourceModelAssetId = AssetId{42};
            sample.m_rootMotionBound = true;
            sample.m_presentedPalette = receipt;
            sample.m_baselinePoseSequence = 10;
            sample.m_finalPoseSequence = 130;
            sample.m_completedFixedSteps = 120;
            sample.m_completedRootMotionSteps = 120;
            sample.m_rootMotionDistance = 2.0f;
            sample.m_qualificationPhysicsBodyHandlePacked = 0x01000003u;
            sample.m_lastQualifiedRootMotionSequence = 120;
            sample.m_observedQualifiedRootMotionIntentCount = 120;
            for (uint32 index = 0;
                 index < static_cast<uint32>(
                             AnimationCharacterSample::ScenarioAction::Count);
                 ++index)
            {
                auto& action = sample.m_scenarioActions[index];
                action.name = AnimationCharacterSample::GetScenarioActionName(
                    static_cast<AnimationCharacterSample::ScenarioAction>(index));
                action.targetSceneRevision = 100u + index;
                action.completedPresentationSequence = 200u + index;
                action.appliedSceneRevision = 300u + index;
                action.prerequisitesSatisfied = true;
                action.passed = true;
            }
        }

        static void SetStableWithoutPresentationReceipts(
            AnimationCharacterSample& sample,
            SamplePresentedSkinningPaletteReceipt receipt)
        {
            sample.m_state = AnimationCharacterSample::State::Stable;
            sample.m_presentedPalette = receipt;
            for (uint32 index = 0;
                 index < static_cast<uint32>(
                             AnimationCharacterSample::ScenarioAction::Count);
                 ++index)
            {
                auto& action = sample.m_scenarioActions[index];
                action.name = AnimationCharacterSample::GetScenarioActionName(
                    static_cast<AnimationCharacterSample::ScenarioAction>(index));
            }
        }

        static void SetQualifiedAwaitingPalette(
            AnimationCharacterSample& sample,
            uint64 sourceModelAssetValue,
            uint64 finalPoseSequence)
        {
            sample.m_state = AnimationCharacterSample::State::AwaitPresentedPalette;
            sample.m_characterModel.status.modelMetadata.sourceModelAssetId =
                AssetId{sourceModelAssetValue};
            sample.m_rootMotionBound = true;
            sample.m_baselinePoseSequence = finalPoseSequence - 120u;
            sample.m_finalPoseSequence = finalPoseSequence;
            sample.m_completedFixedSteps = 120u;
            sample.m_completedRootMotionSteps = 120u;
            sample.m_rootMotionDistance = 2.0f;
            sample.m_qualificationPhysicsBodyHandlePacked = 0x01000003u;
            sample.m_lastQualifiedRootMotionSequence = 120u;
            sample.m_observedQualifiedRootMotionIntentCount = 120u;
            for (uint32 index = 0;
                 index < static_cast<uint32>(
                             AnimationCharacterSample::ScenarioAction::Count);
                 ++index)
            {
                auto& action = sample.m_scenarioActions[index];
                action.name = AnimationCharacterSample::GetScenarioActionName(
                    static_cast<AnimationCharacterSample::ScenarioAction>(index));
            }
            sample.MarkScenarioActionPrerequisite(
                AnimationCharacterSample::ScenarioAction::BindEcsCharacter);
            sample.MarkScenarioActionPrerequisite(
                AnimationCharacterSample::ScenarioAction::EvaluateFixed120);
            sample.MarkScenarioActionPrerequisite(
                AnimationCharacterSample::ScenarioAction::ConsumeRootMotion120);
        }
    };
} // namespace RVX

namespace
{
    using namespace RVX;

    class LogEnvironment final : public ::testing::Environment
    {
    public:
        void SetUp() override { Log::Initialize(); }
        void TearDown() override { Log::Shutdown(); }
    };

    [[maybe_unused]] ::testing::Environment* const g_logEnvironment =
        ::testing::AddGlobalTestEnvironment(new LogEnvironment());

    [[nodiscard]] SamplePresentedSkinningPaletteReceipt MakePalette(
        uint64 sourceModel,
        uint64 provider = 11,
        uint64 pose = 17)
    {
        return {
            .providerComponentId = provider,
            .sourceModelResourceId = sourceModel,
            .poseSequence = pose,
            .paletteHash = 0x12345678u,
            .paletteCount = 23,
            .lane = RenderSkinningPaletteExecutionLane::Direct,
            .frameSequence = 31,
            .presentationSequence = 29,
        };
    }
} // namespace

TEST(AnimationCharacterSampleValidation, DeclaresPureEcsAssetsAndDirectQualification)
{
    AnimationCharacterSample sample;
    const SampleInfo& info = sample.GetInfo();
    EXPECT_EQ(info.id, "animation-character");
    EXPECT_EQ(info.defaultAssetId, "casual-female");
    EXPECT_EQ(info.defaultRenderPath, SampleRenderPath::Direct);
    ASSERT_EQ(info.additionalDefaultAssetIds.size(), 1u);
    EXPECT_EQ(info.additionalDefaultAssetIds.front(), "water-bottle");
    ASSERT_EQ(info.requiredAnimationAssetIds.size(), 1u);
    EXPECT_EQ(info.requiredAnimationAssetIds.front(),
              "casual-female-walk-root-motion");
}

TEST(AnimationCharacterSampleValidation,
     RequestedRenderPathsMapToTheirExactGPUCullingPolicies)
{
    RenderGPUDrivenMode mode = RenderGPUDrivenMode::Auto;
    ASSERT_TRUE(AnimationCharacterSampleValidationAccess::ResolveGPUCullingMode(
        SampleRenderPath::Direct, mode));
    EXPECT_EQ(mode, RenderGPUDrivenMode::ForceDisabled);

    ASSERT_TRUE(AnimationCharacterSampleValidationAccess::ResolveGPUCullingMode(
        SampleRenderPath::GPUDriven, mode));
    EXPECT_EQ(mode, RenderGPUDrivenMode::ForceEnabled);

    ASSERT_TRUE(AnimationCharacterSampleValidationAccess::ResolveGPUCullingMode(
        SampleRenderPath::Auto, mode));
    EXPECT_EQ(mode, RenderGPUDrivenMode::Auto);

    EXPECT_FALSE(AnimationCharacterSampleValidationAccess::ResolveGPUCullingMode(
        static_cast<SampleRenderPath>(255u), mode));
}

TEST(AnimationCharacterSampleValidation, RequiresOneDeterministicBuiltInFixedStep)
{
    AnimationCharacterSample sample;
    const SampleWorldRequirements requirements = sample.GetWorldRequirements();
    EXPECT_EQ(requirements.world.physics.backend,
              Physics::PhysicsBackendType::BuiltIn);
    EXPECT_FLOAT_EQ(requirements.world.physics.fixedTimeStep, 1.0f / 60.0f);
    EXPECT_EQ(requirements.world.physics.maxSubSteps, 1u);
}

TEST(AnimationCharacterSampleValidation, AssessmentContractNamesEcsEvidence)
{
    AnimationCharacterSample sample;
    const SampleAssessmentContract contract = sample.GetAssessmentContract();
    ASSERT_TRUE(contract.IsValid());
    EXPECT_EQ(contract.revision, "2");
    EXPECT_EQ(contract.actions.size(), 4u);
    EXPECT_EQ(contract.invariants.size(), 4u);
    EXPECT_EQ(contract.metrics.size(), 5u);
    EXPECT_EQ(contract.capabilities.size(), 2u);

    const auto hasCode = [](const auto& values, std::string_view expected)
    {
        return std::any_of(values.begin(), values.end(),
                           [expected](const auto& value)
                           {
                               return value.code.GetValue() == expected;
                           });
    };
    EXPECT_TRUE(hasCode(contract.actions,
                        "ANIMATION.CHARACTER.ACTION.BIND_ECS_CHARACTER"));
    EXPECT_TRUE(hasCode(contract.actions,
                        "ANIMATION.CHARACTER.ACTION.CONSUME_ROOT_MOTION_120"));
    EXPECT_TRUE(hasCode(contract.invariants,
                        "ANIMATION.CHARACTER.EXACT_ECS_BINDING"));
    EXPECT_TRUE(hasCode(contract.invariants,
                        "ANIMATION.CHARACTER.PRESENTED_PALETTE"));
}

TEST(AnimationCharacterSampleValidation,
     ReportProjectsStableRevisionTwoQualificationReceipts)
{
    AnimationCharacterSample sample;
    AnimationCharacterSampleValidationAccess::SetQualifiedStableScenario(
        sample, MakePalette(42, 11, 130));
    SampleReport report;
    SampleFeatureReporter reporter(report);
    sample.AppendReport(reporter);

    EXPECT_EQ(report.scenario.contractRevision, 2u);
    EXPECT_EQ(report.scenario.phase, "stable");
    ASSERT_EQ(report.scenario.actions.size(), 4u);
    for (const SampleReportScenarioAction& action : report.scenario.actions)
    {
        EXPECT_TRUE(action.passed);
        EXPECT_NE(action.targetSceneRevision, 0u);
        EXPECT_NE(action.completedPresentationSequence, 0u);
        EXPECT_GE(action.appliedSceneRevision, action.targetSceneRevision);
    }
    ASSERT_EQ(report.scenario.invariants.size(), 4u);
    for (const SampleReportScenarioInvariant& invariant : report.scenario.invariants)
    {
        EXPECT_TRUE(invariant.passed);
        EXPECT_FALSE(invariant.evidence.empty());
    }
    const auto findMetric = [&report](std::string_view name)
    {
        const auto metric = std::find_if(
            report.scenario.metrics.begin(), report.scenario.metrics.end(),
            [name](const SampleReportScenarioMetric& value)
            {
                return value.name == name;
            });
        return metric == report.scenario.metrics.end() ? 0u : metric->value;
    };
    EXPECT_EQ(findMetric("fixedSteps"), 120u);
    EXPECT_EQ(findMetric("poseEvaluations"), 120u);
    EXPECT_EQ(findMetric("rootMotionIntents"), 120u);
    EXPECT_EQ(findMetric("rootMotionMillimetres"), 2000u);
    EXPECT_EQ(findMetric("paletteHash"), 0x12345678u);

    const auto hasFeature = [&report](std::string_view feature)
    {
        return std::find(report.enabledFeatures.begin(), report.enabledFeatures.end(),
                         feature) != report.enabledFeatures.end();
    };
    EXPECT_TRUE(hasFeature("ModelResourceLoad"));
    EXPECT_TRUE(hasFeature("SceneInstantiation"));
    EXPECT_TRUE(hasFeature("EcsCharacterModel"));
    EXPECT_TRUE(hasFeature("EcsSkinningPalette"));
    EXPECT_TRUE(hasFeature("EcsAnimationPhysicsBinding"));
    EXPECT_TRUE(hasFeature("EcsFixedAnimation"));
    EXPECT_TRUE(hasFeature("EcsPresentedSkinningPalette"));
    EXPECT_TRUE(hasFeature("RenderPathPolicySelection"));
    EXPECT_TRUE(hasFeature("DirectPathRequested"));
    EXPECT_NE(std::find(report.unsupportedFeatures.begin(),
                        report.unsupportedFeatures.end(), "Jolt"),
              report.unsupportedFeatures.end());
}

TEST(AnimationCharacterSampleValidation,
     ReportDoesNotClaimStableBeforeActionPresentationReceipts)
{
    AnimationCharacterSample sample;
    AnimationCharacterSampleValidationAccess::SetStableWithoutPresentationReceipts(
        sample, MakePalette(42, 11, 130));
    SampleReport report;
    SampleFeatureReporter reporter(report);
    sample.AppendReport(reporter);

    EXPECT_EQ(report.scenario.contractRevision, 2u);
    EXPECT_EQ(report.scenario.phase, "pending");
    ASSERT_EQ(report.scenario.actions.size(), 4u);
    EXPECT_TRUE(std::all_of(
        report.scenario.actions.begin(), report.scenario.actions.end(),
        [](const SampleReportScenarioAction& action) { return !action.passed; }));
}

TEST(AnimationCharacterSampleValidation, RejectsChangedPhysicsBodyEpoch)
{
    AnimationCharacterSample sample;
    std::string error;
    EXPECT_TRUE(AnimationCharacterSampleValidationAccess::ObserveQualifiedRootMotionIntent(
        sample, 41, 0x01000003u, error));
    EXPECT_TRUE(AnimationCharacterSampleValidationAccess::ObserveQualifiedRootMotionIntent(
        sample, 42, 0x01000003u, error));
    EXPECT_FALSE(AnimationCharacterSampleValidationAccess::ObserveQualifiedRootMotionIntent(
        sample, 43, 0x01000004u, error));
    EXPECT_EQ(error,
              "The exact Physics-body handle changed during the 120-step root-motion epoch.");
}

TEST(AnimationCharacterSampleValidation,
     PresentedPaletteRequiresExactModelIdentityAndDirectCompletion)
{
    AnimationCharacterSample sample;
    AnimationCharacterSampleValidationAccess::SetAwaitingPalette(sample, 42);

    SampleRenderDiagnostics diagnostics;
    diagnostics.presentedSkinningPalettesAvailable = true;
    diagnostics.presentedSkinningPalettes.push_back(MakePalette(7));
    diagnostics.presentedSkinningPalettes.push_back(MakePalette(42));
    SamplePresentedSkinningPaletteReceipt found;
    EXPECT_TRUE(AnimationCharacterSampleValidationAccess::FindPalette(
        sample, diagnostics, found));
    EXPECT_EQ(found.sourceModelResourceId, 42u);
    EXPECT_TRUE(found.IsValid());

    diagnostics.presentedSkinningPalettes.back().poseSequence = 16;
    EXPECT_FALSE(AnimationCharacterSampleValidationAccess::FindPalette(
        sample, diagnostics, found));
    diagnostics.presentedSkinningPalettes.back() = MakePalette(42);

    diagnostics.presentedSkinningPalettes.back().lane =
        static_cast<RenderSkinningPaletteExecutionLane>(255u);
    EXPECT_FALSE(AnimationCharacterSampleValidationAccess::FindPalette(
        sample, diagnostics, found));
    diagnostics.presentedSkinningPalettes.back() = MakePalette(42);
    diagnostics.presentedSkinningPalettesOverflow = true;
    EXPECT_FALSE(AnimationCharacterSampleValidationAccess::FindPalette(
        sample, diagnostics, found));
}

TEST(AnimationCharacterSampleValidation,
     FrameOnlyFinalPaletteBindsActionsToRetainedRenderSceneRevision)
{
    AnimationCharacterSample sample;
    AnimationCharacterSampleValidationAccess::SetQualifiedAwaitingPalette(
        sample, 42u, 121u);

    SamplePresentedSkinningPaletteReceipt receipt = MakePalette(42u, 11u, 121u);
    receipt.frameSequence = 157u;
    receipt.presentationSequence = 157u;

    SampleRenderDiagnostics diagnostics;
    diagnostics.rendered = true;
    diagnostics.visibleObjectCount = 6u;
    diagnostics.renderSceneValuesAvailable = true;
    diagnostics.renderSceneFrameSequence = 157u;
    diagnostics.renderSceneAppliedRevision = 145u;
    diagnostics.renderSceneRequiredRevision = 145u;
    diagnostics.acceptedExtractionDiagnosticsAvailable = true;
    diagnostics.acceptedExtractionLastSourceFrameSequence = 156u;
    diagnostics.acceptedExtractionLastSceneRevision = 145u;
    diagnostics.lastPresentedFrameSequence =
        DiagnosticValue<uint64>::Available(157u);
    diagnostics.presentedSkinningPalettesAvailable = true;
    diagnostics.presentedSkinningPalettes.push_back(receipt);

    SampleAssessmentChannel assessment;
    sample.ObserveDiagnostics(diagnostics, assessment);

    EXPECT_TRUE(sample.GetReadiness(diagnostics).IsReady());
    EXPECT_TRUE(sample.ShouldBeginFinalRenderDrain(diagnostics));

    SampleReport report;
    SampleFeatureReporter reporter(report);
    sample.AppendReport(reporter);
    EXPECT_EQ(report.scenario.phase, "stable");
    ASSERT_EQ(report.scenario.actions.size(), 4u);
    for (const SampleReportScenarioAction& action : report.scenario.actions)
    {
        EXPECT_TRUE(action.passed) << action.name;
        EXPECT_EQ(action.targetSceneRevision, 145u) << action.name;
        EXPECT_EQ(action.appliedSceneRevision, 145u) << action.name;
        EXPECT_EQ(action.completedPresentationSequence, 157u) << action.name;
    }
}

TEST(AnimationCharacterSampleValidation,
     LaterExactPaletteCannotRebindCompletedActionEvidence)
{
    AnimationCharacterSample sample;
    AnimationCharacterSampleValidationAccess::SetQualifiedAwaitingPalette(
        sample, 42u, 121u);

    SamplePresentedSkinningPaletteReceipt firstReceipt =
        MakePalette(42u, 11u, 121u);
    firstReceipt.frameSequence = 157u;
    firstReceipt.presentationSequence = 157u;

    SampleRenderDiagnostics firstObservation;
    firstObservation.rendered = true;
    firstObservation.visibleObjectCount = 6u;
    firstObservation.renderSceneValuesAvailable = true;
    firstObservation.renderSceneAppliedRevision = 145u;
    firstObservation.presentedSkinningPalettesAvailable = true;
    firstObservation.presentedSkinningPalettes.push_back(firstReceipt);

    SampleAssessmentChannel assessment;
    sample.ObserveDiagnostics(firstObservation, assessment);

    SampleRenderDiagnostics completionObservation = firstObservation;
    completionObservation.presentedSkinningPalettes.clear();
    completionObservation.lastPresentedFrameSequence =
        DiagnosticValue<uint64>::Available(157u);
    sample.ObserveDiagnostics(completionObservation, assessment);

    SampleRenderDiagnostics laterExactObservation = completionObservation;
    laterExactObservation.renderSceneAppliedRevision = 146u;
    laterExactObservation.lastPresentedFrameSequence =
        DiagnosticValue<uint64>::Available(158u);
    firstReceipt.frameSequence = 158u;
    firstReceipt.presentationSequence = 158u;
    laterExactObservation.presentedSkinningPalettes.push_back(firstReceipt);
    sample.ObserveDiagnostics(laterExactObservation, assessment);

    EXPECT_TRUE(sample.GetReadiness(laterExactObservation).IsReady());
    SampleReport report;
    SampleFeatureReporter reporter(report);
    sample.AppendReport(reporter);
    EXPECT_EQ(report.scenario.phase, "stable");
    ASSERT_EQ(report.scenario.actions.size(), 4u);
    for (const SampleReportScenarioAction& action : report.scenario.actions)
    {
        EXPECT_TRUE(action.passed) << action.name;
        EXPECT_EQ(action.targetSceneRevision, 145u) << action.name;
        EXPECT_EQ(action.appliedSceneRevision, 145u) << action.name;
        EXPECT_EQ(action.completedPresentationSequence, 157u) << action.name;
    }
}

TEST(AnimationCharacterSampleValidation,
     StableReadinessRequiresRenderedVisibleGeometryAndValidPalette)
{
    AnimationCharacterSample sample;
    AnimationCharacterSampleValidationAccess::SetStable(sample, MakePalette(42));
    SampleRenderDiagnostics diagnostics;
    diagnostics.rendered = true;
    diagnostics.visibleObjectCount = 6;
    EXPECT_TRUE(sample.GetReadiness(diagnostics).IsReady());
    EXPECT_TRUE(sample.ShouldBeginFinalRenderDrain(diagnostics));

    diagnostics.visibleObjectCount = 0;
    EXPECT_EQ(sample.GetReadiness(diagnostics).state, SampleReadinessState::Pending);
    EXPECT_FALSE(sample.ShouldBeginFinalRenderDrain(diagnostics));
}

TEST(AnimationCharacterSampleValidation, FailureEvidenceDominatesPresentation)
{
    AnimationCharacterSample sample;
    AnimationCharacterSampleValidationAccess::SetStable(sample, MakePalette(42));
    AnimationCharacterSampleValidationAccess::SetFailure(
        sample, "root-motion sequence gap");
    SampleRenderDiagnostics diagnostics;
    diagnostics.rendered = true;
    diagnostics.visibleObjectCount = 6;
    const SampleReadiness readiness = sample.GetReadiness(diagnostics);
    EXPECT_TRUE(readiness.IsFailed());
    EXPECT_EQ(readiness.reason, "root-motion sequence gap");
}
