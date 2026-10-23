#include "Render/RayTracing/HybridRayTracingFramePlan.h"
#include "Render/RayTracing/RayTracingScene.h"
#include "Render/RayTracing/RayTracingSceneManager.h"

#include <gtest/gtest.h>

namespace RVX
{
namespace
{
    RenderFrameIdentity MakeFrameIdentity(uint64 sequence)
    {
        return {sequence, 7u, 11u, 13u};
    }

    RenderViewIdentity MakeViewIdentity()
    {
        RenderViewIdentity identity;
        identity.sceneRuntimeId = 17u;
        identity.cameraEntityPacked = 19u;
        identity.role = ViewRole::Main;
        return identity;
    }

    RenderViewHistoryIdentity MakeHistoryIdentity(const RenderViewIdentity& view)
    {
        RenderViewHistoryIdentity identity;
        identity.previousFrame = MakeFrameIdentity(1u);
        identity.view = view;
        identity.extent = {960u, 540u};
        identity.familyId = 23u;
        identity.worldRevision = 29u;
        identity.temporalEpoch = 31u;
        identity.formatRevision = 37u;
        identity.surfaceGeneration = 11u;
        identity.cameraCutRevision = 41u;
        return identity;
    }

    HybridRayTracingEffectReadiness MakeReadyEffect()
    {
        HybridRayTracingEffectReadiness readiness;
        readiness.requested = true;
        readiness.compiled = true;
        readiness.hardwareSupported = true;
        readiness.implementationSupported = true;
        readiness.pipelineReady = true;
        readiness.inputsReady = true;
        readiness.dispatchReady = true;
        return readiness;
    }

    HybridRayTracingFramePlanRequest MakeHybridRequest()
    {
        HybridRayTracingFramePlanRequest request;
        request.frameIdentity = MakeFrameIdentity(2u);
        request.viewIdentity = MakeViewIdentity();
        request.reflectionHistoryIdentity = MakeHistoryIdentity(request.viewIdentity);
        request.outputExtent = {1920u, 1080u};
        request.recipe = RenderPipelineRecipe::HybridRayTracing;
        request.quality = RenderQualityTier::High;
        request.profileHash = ComputeRenderPipelineProfileHash(
            request.recipe, request.quality);
        request.rayTracingScenePreparationGeneration = 43u;
        request.rayTracingSceneStructuralHash = 47u;
        request.shadow = MakeReadyEffect();
        request.reflection = MakeReadyEffect();
        return request;
    }

    RayTracingSceneBuildPlan MakeScenePlan()
    {
        RayTracingSceneBuildPlan plan;
        RayTracingBLASBuild blas;
        blas.key.mesh = {7u, 3u};
        blas.key.meshId = (static_cast<uint64>(7u) << 32u) | 3u;
        blas.key.meshContentRevision = 41u;
        blas.key.submeshIndex = 2u;
        plan.blasBuilds.push_back(blas);

        RayTracingTLASInstance instance;
        instance.blasIndex = 0u;
        instance.entityId = 101u;
        instance.materialId = 303u;
        instance.desc.instanceMask = 0x7Fu;
        instance.desc.flags = RHIRayTracingInstanceFlags::ForceNoOpaque;
        instance.alphaTest.enabled = true;
        instance.alphaTest.status = RayTracingMaskedMetadataStatus::Complete;
        instance.alphaTest.baseColorUVSet = 1u;
        instance.alphaTest.baseColorTextureId = 909u;
        plan.instances.push_back(instance);
        return plan;
    }
} // namespace

TEST(HybridRayTracingContractValidation,
     RequiredAndOptionalRoutesProduceStrictReceipts)
{
    HybridRayTracingFramePlanRequest requiredRequest = MakeHybridRequest();
    requiredRequest.shadowRequirement = HybridRayTracingRequirement::Required;
    requiredRequest.reflectionRequirement = HybridRayTracingRequirement::Required;
    const HybridRayTracingFramePlan requiredPlan =
        HybridRayTracingFramePlan::Build(requiredRequest);

    ASSERT_TRUE(requiredPlan.IsValid());
    EXPECT_EQ(requiredPlan.GetShadowRoute(), HybridRayTracingShadowRoute::RayTraced);
    EXPECT_EQ(requiredPlan.GetReflectionRoute(),
              HybridRayTracingReflectionRoute::RayTraced);

    HybridRayTracingExecutionObservation requiredObservation;
    requiredObservation.graphExecutionSucceeded = true;
    requiredObservation.sceneBuildAccepted = true;
    requiredObservation.submissionAccepted = true;
    requiredObservation.shadowDispatchRecorded = true;
    requiredObservation.reflectionDispatchRecorded = true;
    requiredObservation.reflectionSpatialDenoiseRecorded = true;
    requiredObservation.reflectionTemporalDenoiseRecorded = true;
    requiredObservation.reflectionCompositeRecorded = true;
    requiredObservation.reflectionCompositeUsedDenoisedSource = true;
    const HybridRayTracingExecutionReceipt requiredReceipt =
        HybridRayTracingExecutionReceipt::FromPlan(requiredPlan, requiredObservation);
    EXPECT_TRUE(requiredReceipt.IsStructurallyConsistentWith(requiredPlan));
    EXPECT_TRUE(requiredReceipt.IsExecutionSuccessfulFor(requiredPlan));

    HybridRayTracingFramePlanRequest optionalRequest = MakeHybridRequest();
    optionalRequest.shadow = {};
    optionalRequest.shadow.requested = true;
    optionalRequest.reflection = {};
    optionalRequest.reflection.requested = true;
    optionalRequest.rasterCSMReady = true;
    optionalRequest.screenSpaceReflectionReady = true;
    const HybridRayTracingFramePlan optionalPlan =
        HybridRayTracingFramePlan::Build(optionalRequest);

    ASSERT_TRUE(optionalPlan.IsValid());
    EXPECT_EQ(optionalPlan.GetShadowRoute(), HybridRayTracingShadowRoute::RasterCSM);
    EXPECT_EQ(optionalPlan.GetReflectionRoute(),
              HybridRayTracingReflectionRoute::ScreenSpaceReflection);

    HybridRayTracingExecutionObservation optionalObservation;
    optionalObservation.graphExecutionSucceeded = true;
    optionalObservation.submissionAccepted = true;
    optionalObservation.rasterCSMExecuted = true;
    optionalObservation.screenSpaceReflectionExecuted = true;
    const HybridRayTracingExecutionReceipt optionalReceipt =
        HybridRayTracingExecutionReceipt::FromPlan(optionalPlan, optionalObservation);
    EXPECT_TRUE(optionalReceipt.IsExecutionSuccessfulFor(optionalPlan));

    optionalRequest.shadowRequirement = HybridRayTracingRequirement::Required;
    optionalRequest.reflectionRequirement = HybridRayTracingRequirement::Required;
    const HybridRayTracingFramePlan rejectedPlan =
        HybridRayTracingFramePlan::Build(optionalRequest);
    EXPECT_TRUE(rejectedPlan.HasRequiredFailure());
    EXPECT_FALSE(rejectedPlan.IsValid());
}

TEST(HybridRayTracingContractValidation,
     SceneLifecycleIsGenerationAndCompletionSafe)
{
    RayTracingSceneBuildPlan plan = MakeScenePlan();
    const RayTracingTLASStructuralSignature initialSignature =
        BuildRayTracingTLASStructuralSignature(plan);

    plan.instances.front().desc.transform[0] = 42.0f;
    EXPECT_EQ(initialSignature, BuildRayTracingTLASStructuralSignature(plan));

    ++plan.blasBuilds.front().key.meshContentRevision;
    EXPECT_NE(initialSignature, BuildRayTracingTLASStructuralSignature(plan));

    const RayTracingSceneRecordingToken invalidToken;
    const RayTracingSceneRecordingToken acceptedToken{1u, 2u};
    EXPECT_FALSE(invalidToken.IsValid());
    EXPECT_TRUE(acceptedToken.IsValid());

    EXPECT_FALSE(IsRayTracingBLASCompactionPublished(
        RayTracingBLASCompactionStatus::QueryRecorded));
    EXPECT_FALSE(IsRayTracingBLASCompactionPublished(
        RayTracingBLASCompactionStatus::AwaitingCopyCompletion));
    EXPECT_TRUE(IsRayTracingBLASCompactionSubmissionBound(
        RayTracingBLASCompactionStatus::AwaitingCopyCompletion));
    EXPECT_TRUE(IsRayTracingBLASCompactionPublished(
        RayTracingBLASCompactionStatus::Compacted));
    EXPECT_EQ(RayTracingBLASCompactionStatus::ReadyToRecordCopy,
              ReleaseUnsubmittedRayTracingBLASCompaction(
                  RayTracingBLASCompactionStatus::CopyRecorded));
}
} // namespace RVX
