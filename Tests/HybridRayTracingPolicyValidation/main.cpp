#include "Render/RayTracing/HybridRayTracingFramePlan.h"

#include <gtest/gtest.h>

#include <limits>

namespace RVX
{
namespace
{
    RenderFrameIdentity MakeFrameIdentity(uint64 sequence)
    {
        return {
            sequence,
            7,
            11,
            13};
    }

    RenderViewIdentity MakeViewIdentity()
    {
        RenderViewIdentity result;
        result.sceneRuntimeId = 17;
        result.cameraEntityPacked = 19;
        result.role = ViewRole::Main;
        return result;
    }

    RenderViewHistoryIdentity MakeHistoryIdentity(
        const RenderViewIdentity& view)
    {
        RenderViewHistoryIdentity result;
        result.previousFrame = MakeFrameIdentity(1);
        result.view = view;
        result.extent = {961, 541};
        result.familyId = 23;
        result.worldRevision = 29;
        result.temporalEpoch = 31;
        result.formatRevision = 37;
        result.surfaceGeneration = 11;
        result.cameraCutRevision = 41;
        return result;
    }

    HybridRayTracingEffectReadiness MakeReadyEffect()
    {
        HybridRayTracingEffectReadiness result;
        result.requested = true;
        result.compiled = true;
        result.hardwareSupported = true;
        result.implementationSupported = true;
        result.pipelineReady = true;
        result.inputsReady = true;
        result.dispatchReady = true;
        return result;
    }

    HybridRayTracingFramePlanRequest MakeRequest()
    {
        HybridRayTracingFramePlanRequest result;
        result.frameIdentity = MakeFrameIdentity(2);
        result.viewIdentity = MakeViewIdentity();
        result.reflectionHistoryIdentity = MakeHistoryIdentity(result.viewIdentity);
        result.outputExtent = {1921, 1081};
        result.recipe = RenderPipelineRecipe::HybridRayTracing;
        result.quality = RenderQualityTier::High;
        result.profileHash = ComputeRenderPipelineProfileHash(
            result.recipe, result.quality);
        result.rayTracingScenePreparationGeneration = 43;
        result.rayTracingSceneStructuralHash = 47;
        result.shadow = MakeReadyEffect();
        result.reflection = MakeReadyEffect();
        return result;
    }
} // namespace

TEST(HybridRayTracingPolicyValidation, HalfResolutionUsesCeilWithoutOverflow)
{
    const HybridRayTracingHalfResolution odd =
        ResolveHybridRayTracingHalfResolution({1921, 1081});
    EXPECT_TRUE(odd.IsValid());
    EXPECT_EQ(odd.halfResolution.width, 961U);
    EXPECT_EQ(odd.halfResolution.height, 541U);

    const HybridRayTracingHalfResolution maximum =
        ResolveHybridRayTracingHalfResolution(
            {std::numeric_limits<uint32>::max(),
             std::numeric_limits<uint32>::max()});
    EXPECT_TRUE(maximum.IsValid());
    EXPECT_EQ(maximum.halfResolution.width, 2147483648U);
    EXPECT_EQ(maximum.halfResolution.height, 2147483648U);
}

TEST(HybridRayTracingPolicyValidation, OptionalEffectsUseDeclaredFallbackOrder)
{
    HybridRayTracingFramePlanRequest request = MakeRequest();
    request.shadow = {};
    request.shadow.requested = true;
    request.reflection = {};
    request.reflection.requested = true;
    request.rasterCSMReady = true;
    request.screenSpaceReflectionReady = true;
    request.reflectionProbeReady = true;
    request.imageBasedLightingReady = true;

    const HybridRayTracingFramePlan plan =
        HybridRayTracingFramePlan::Build(request);
    EXPECT_TRUE(plan.IsValid());
    EXPECT_EQ(plan.GetShadowRoute(), HybridRayTracingShadowRoute::RasterCSM);
    EXPECT_EQ(plan.GetReflectionRoute(),
              HybridRayTracingReflectionRoute::ScreenSpaceReflection);

    request.screenSpaceReflectionReady = false;
    const HybridRayTracingFramePlan probePlan =
        HybridRayTracingFramePlan::Build(request);
    EXPECT_TRUE(probePlan.IsValid());
    EXPECT_EQ(probePlan.GetReflectionRoute(),
              HybridRayTracingReflectionRoute::ReflectionProbe);

    request.reflectionProbeReady = false;
    const HybridRayTracingFramePlan iblPlan =
        HybridRayTracingFramePlan::Build(request);
    EXPECT_TRUE(iblPlan.IsValid());
    EXPECT_EQ(iblPlan.GetReflectionRoute(),
              HybridRayTracingReflectionRoute::ImageBasedLighting);
}

TEST(HybridRayTracingPolicyValidation, RequiredEffectsFailClosedWhenNotRequested)
{
    HybridRayTracingFramePlanRequest request = MakeRequest();
    request.shadowRequirement = HybridRayTracingRequirement::Required;
    request.reflectionRequirement = HybridRayTracingRequirement::Required;
    request.shadow.requested = false;
    request.reflection.requested = false;
    request.rasterCSMReady = true;
    request.screenSpaceReflectionReady = true;

    const HybridRayTracingFramePlan plan =
        HybridRayTracingFramePlan::Build(request);
    EXPECT_TRUE(plan.HasRequiredFailure());
    EXPECT_FALSE(plan.IsValid());
    EXPECT_EQ(plan.GetShadowRoute(), HybridRayTracingShadowRoute::Failed);
    EXPECT_EQ(plan.GetReflectionRoute(),
              HybridRayTracingReflectionRoute::Failed);
}

TEST(HybridRayTracingPolicyValidation, ReceiptRequiresSubmissionAndBothDenoiseStages)
{
    const HybridRayTracingFramePlan plan =
        HybridRayTracingFramePlan::Build(MakeRequest());
    ASSERT_TRUE(plan.IsValid());

    const HybridRayTracingExecutionReceipt receipt =
        HybridRayTracingExecutionReceipt::FromPlan(plan);
    EXPECT_TRUE(receipt.IsStructurallyConsistentWith(plan));
    EXPECT_FALSE(receipt.IsExecutionSuccessfulFor(plan));

    HybridRayTracingExecutionObservation missingTemporalDenoise;
    missingTemporalDenoise.graphExecutionSucceeded = true;
    missingTemporalDenoise.sceneBuildAccepted = true;
    missingTemporalDenoise.submissionAccepted = true;
    missingTemporalDenoise.shadowDispatchRecorded = true;
    missingTemporalDenoise.reflectionDispatchRecorded = true;
    missingTemporalDenoise.reflectionSpatialDenoiseRecorded = true;
    missingTemporalDenoise.reflectionCompositeRecorded = true;
    missingTemporalDenoise.reflectionCompositeUsedDenoisedSource = true;
    EXPECT_FALSE(HybridRayTracingExecutionReceipt::FromPlan(
        plan, missingTemporalDenoise).IsExecutionSuccessfulFor(plan));

    missingTemporalDenoise.reflectionTemporalDenoiseRecorded = true;
    EXPECT_TRUE(HybridRayTracingExecutionReceipt::FromPlan(
        plan, missingTemporalDenoise).IsExecutionSuccessfulFor(plan));
    EXPECT_FALSE(HybridRayTracingExecutionReceipt::FromPlan(
        plan, missingTemporalDenoise).IsPresentationQualifiedFor(plan));
    missingTemporalDenoise.presentationQualified = true;
    EXPECT_TRUE(HybridRayTracingExecutionReceipt::FromPlan(
        plan, missingTemporalDenoise).IsPresentationQualifiedFor(plan));

    missingTemporalDenoise.rasterCSMExecuted = true;
    EXPECT_FALSE(HybridRayTracingExecutionReceipt::FromPlan(
        plan, missingTemporalDenoise).IsExecutionSuccessfulFor(plan));
    missingTemporalDenoise.rasterCSMExecuted = false;
    missingTemporalDenoise.screenSpaceReflectionExecuted = true;
    EXPECT_FALSE(HybridRayTracingExecutionReceipt::FromPlan(
        plan, missingTemporalDenoise).IsExecutionSuccessfulFor(plan));
}

TEST(HybridRayTracingPolicyValidation,
     RayTracedHistoryMustPrecedeAndMatchTheHalfResolutionView)
{
    HybridRayTracingFramePlanRequest request = MakeRequest();
    request.reflectionHistoryIdentity.previousFrame = MakeFrameIdentity(2);
    EXPECT_FALSE(HybridRayTracingFramePlan::Build(request).IsValid());

    request = MakeRequest();
    request.reflectionHistoryIdentity.extent = request.outputExtent;
    EXPECT_FALSE(HybridRayTracingFramePlan::Build(request).IsValid());

    request = MakeRequest();
    request.reflectionHistoryReset = RenderHistoryResetReason::Resize;
    request.reflectionHistoryIdentity = {};
    EXPECT_TRUE(HybridRayTracingFramePlan::Build(request).IsValid());
}

TEST(HybridRayTracingPolicyValidation,
     OptionalFallbackReceiptRequiresTheSelectedRouteToExecute)
{
    HybridRayTracingFramePlanRequest request = MakeRequest();
    request.shadow = {};
    request.shadow.requested = true;
    request.rasterCSMReady = true;
    request.reflection = {};
    request.reflection.requested = true;
    request.screenSpaceReflectionReady = true;
    const HybridRayTracingFramePlan plan =
        HybridRayTracingFramePlan::Build(request);
    ASSERT_TRUE(plan.IsValid());

    HybridRayTracingExecutionObservation observation;
    observation.graphExecutionSucceeded = true;
    observation.submissionAccepted = true;
    EXPECT_FALSE(HybridRayTracingExecutionReceipt::FromPlan(
        plan, observation).IsExecutionSuccessfulFor(plan));

    observation.rasterCSMExecuted = true;
    observation.screenSpaceReflectionExecuted = true;
    EXPECT_TRUE(HybridRayTracingExecutionReceipt::FromPlan(
        plan, observation).IsExecutionSuccessfulFor(plan));

    observation.reflectionProbeResolved = true;
    EXPECT_FALSE(HybridRayTracingExecutionReceipt::FromPlan(
        plan, observation).IsExecutionSuccessfulFor(plan));
    observation.reflectionProbeResolved = false;
    observation.reflectionDispatchRecorded = true;
    EXPECT_FALSE(HybridRayTracingExecutionReceipt::FromPlan(
        plan, observation).IsExecutionSuccessfulFor(plan));
}

TEST(HybridRayTracingPolicyValidation,
     ReceiptBindsTheCompleteFrozenPlanIdentity)
{
    const HybridRayTracingFramePlan plan =
        HybridRayTracingFramePlan::Build(MakeRequest());
    ASSERT_TRUE(plan.IsValid());

    HybridRayTracingExecutionObservation observation;
    observation.graphExecutionSucceeded = true;
    observation.sceneBuildAccepted = true;
    observation.shadowDispatchRecorded = true;
    observation.reflectionDispatchRecorded = true;
    observation.reflectionSpatialDenoiseRecorded = true;
    observation.reflectionTemporalDenoiseRecorded = true;
    observation.reflectionCompositeRecorded = true;
    observation.reflectionCompositeUsedDenoisedSource = true;
    observation.submissionAccepted = true;
    const HybridRayTracingExecutionReceipt receipt =
        HybridRayTracingExecutionReceipt::FromPlan(plan, observation);
    ASSERT_TRUE(receipt.IsExecutionSuccessfulFor(plan));
    EXPECT_NE(receipt.GetPlanIdentityHash(), 0U);

    HybridRayTracingFramePlanRequest changedRequest = MakeRequest();
    changedRequest.reflectionHistoryReset = RenderHistoryResetReason::Resize;
    changedRequest.reflectionHistoryIdentity = {};
    const HybridRayTracingFramePlan changedPlan =
        HybridRayTracingFramePlan::Build(changedRequest);
    ASSERT_TRUE(changedPlan.IsValid());
    EXPECT_NE(plan.GetIdentityHash(), changedPlan.GetIdentityHash());
    EXPECT_FALSE(receipt.IsStructurallyConsistentWith(changedPlan));

    changedRequest = MakeRequest();
    changedRequest.quality = RenderQualityTier::Ultra;
    changedRequest.profileHash = ComputeRenderPipelineProfileHash(
        changedRequest.recipe, changedRequest.quality);
    const HybridRayTracingFramePlan changedProfilePlan =
        HybridRayTracingFramePlan::Build(changedRequest);
    ASSERT_TRUE(changedProfilePlan.IsValid());
    EXPECT_FALSE(receipt.IsStructurallyConsistentWith(changedProfilePlan));

    changedRequest = MakeRequest();
    ++changedRequest.rayTracingScenePreparationGeneration;
    const HybridRayTracingFramePlan changedScenePlan =
        HybridRayTracingFramePlan::Build(changedRequest);
    ASSERT_TRUE(changedScenePlan.IsValid());
    EXPECT_FALSE(receipt.IsStructurallyConsistentWith(changedScenePlan));
}
} // namespace RVX
