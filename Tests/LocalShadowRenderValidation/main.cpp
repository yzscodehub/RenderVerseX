#include "Render/Lighting/LocalShadowFramePlan.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <limits>
#include <vector>

namespace
{
    RVX::LightingLightCandidate MakeLocalLight(
        RVX::uint64 lightId,
        RVX::RenderLightType type,
        RVX::uint32 priority,
        bool castsShadows)
    {
        RVX::LightingLightCandidate light;
        light.lightId = lightId;
        light.type = type;
        light.priority = priority;
        light.position = RVX::Vec3(
            static_cast<float>(lightId), 2.0f, -3.0f);
        light.direction = type == RVX::RenderLightType::Spot
            ? RVX::Vec3(0.0f, -1.0f, 0.0f)
            : RVX::Vec3(0.0f);
        light.color = RVX::Vec3(1.0f, 0.75f, 0.5f);
        light.normalizedIntensity = 12.0f;
        light.range = 20.0f;
        light.innerConeRadians = type == RVX::RenderLightType::Spot
            ? 0.25f : 0.0f;
        light.outerConeRadians = type == RVX::RenderLightType::Spot
            ? 0.5f : 0.0f;
        light.castsShadows = castsShadows;
        return light;
    }

    RVX::LightingFramePlan MakeLightingPlan(
        std::vector<RVX::LightingLightCandidate> candidates)
    {
        RVX::LightingFramePlanRequest request;
        request.frameIdentity = {
            .frameSequence = 101,
            .sceneRevision = 103,
            .surfaceGeneration = 107,
            .configurationRevision = 109};
        request.viewIdentity = {
            .sceneRuntimeId = 113,
            .cameraEntityPacked = 127,
            .subViewIndex = 0,
            .role = RVX::ViewRole::Main};
        request.sceneLightStateHash = 131;
        request.lightingConfigurationRevision = 137;
        request.qualityRevision = 139;
        request.quality = RVX::RenderQualityTier::High;
        request.pointLightCapacity = 16;
        request.spotLightCapacity = 16;
        request.allowLocalLightTruncation = false;
        request.csm = {};
        request.ibl = {};
        request.ibl.normalizedIntensity = 0.0f;
        request.lightCandidates = std::move(candidates);

        const RVX::LightingFramePlanBuildResult result =
            RVX::LightingFramePlanBuilder{}.Build(request);
        EXPECT_TRUE(result.Succeeded()) << result.diagnostic;
        EXPECT_TRUE(result.plan.IsValid());
        return result.plan;
    }

    RVX::LightingFramePlan MakeRepresentativeLightingPlan()
    {
        return MakeLightingPlan({
            MakeLocalLight(40, RVX::RenderLightType::Point, 9, true),
            MakeLocalLight(20, RVX::RenderLightType::Spot, 9, true),
            MakeLocalLight(30, RVX::RenderLightType::Point, 5, true),
            MakeLocalLight(10, RVX::RenderLightType::Spot, 20, false),
        });
    }

    RVX::LocalShadowFramePlanRequest MakeAtlasRequest()
    {
        RVX::LocalShadowFramePlanRequest request;
        request.atlas = {
            .pageResolution = 512,
            .pageCapacity = 32,
            .pointLightCapacity = 4,
            .spotLightCapacity = 4,
            .overflowPolicy =
                RVX::LocalShadowOverflowPolicy::StableUnshadowedFallback};
        request.atlasGeneration = 17;
        return request;
    }
} // namespace

TEST(LocalShadowRenderValidation,
     ConsumesSealedLocalOrderAndAllocatesContiguousPointAndSpotPages)
{
    const RVX::LightingFramePlan lightingPlan =
        MakeRepresentativeLightingPlan();
    const RVX::LocalShadowFramePlanBuildResult result =
        RVX::LocalShadowFramePlanBuilder{}.Build(
            lightingPlan, MakeAtlasRequest());

    ASSERT_TRUE(result.Succeeded()) << result.diagnostic;
    ASSERT_TRUE(result.plan.IsValid());
    ASSERT_EQ(3U, result.plan.GetAllocations().size());

    const auto& allocations = result.plan.GetAllocations();
    EXPECT_EQ(20U, allocations[0].lightId);
    EXPECT_EQ(RVX::RenderLightType::Spot, allocations[0].type);
    EXPECT_EQ(1U, allocations[0].lightBufferIndex);
    EXPECT_EQ(0U, allocations[0].firstPage);
    EXPECT_EQ(RVX::RVX_LOCAL_SHADOW_SPOT_PAGE_COUNT,
              allocations[0].pageCount);

    EXPECT_EQ(40U, allocations[1].lightId);
    EXPECT_EQ(RVX::RenderLightType::Point, allocations[1].type);
    EXPECT_EQ(0U, allocations[1].lightBufferIndex);
    EXPECT_EQ(1U, allocations[1].firstPage);
    EXPECT_EQ(RVX::RVX_LOCAL_SHADOW_POINT_PAGE_COUNT,
              allocations[1].pageCount);

    EXPECT_EQ(30U, allocations[2].lightId);
    EXPECT_EQ(RVX::RenderLightType::Point, allocations[2].type);
    EXPECT_EQ(1U, allocations[2].lightBufferIndex);
    EXPECT_EQ(7U, allocations[2].firstPage);
    EXPECT_EQ(RVX::RVX_LOCAL_SHADOW_POINT_PAGE_COUNT,
              allocations[2].pageCount);

    const RVX::LocalShadowFramePlanEvidence& evidence =
        result.plan.GetEvidence();
    EXPECT_EQ(13U, evidence.requestedPageCount);
    EXPECT_EQ(13U, evidence.selectedPageCount);
    EXPECT_EQ(0U, evidence.overflowPageCount);
    EXPECT_EQ(2U, evidence.requestedPointLightCount);
    EXPECT_EQ(2U, evidence.selectedPointLightCount);
    EXPECT_EQ(1U, evidence.requestedSpotLightCount);
    EXPECT_EQ(1U, evidence.selectedSpotLightCount);
    EXPECT_FALSE(evidence.overflowed);
}

TEST(LocalShadowRenderValidation,
     ReorderedSourceCandidatesProduceTheSameSealedAllocationIdentity)
{
    std::vector<RVX::LightingLightCandidate> candidates = {
        MakeLocalLight(40, RVX::RenderLightType::Point, 9, true),
        MakeLocalLight(20, RVX::RenderLightType::Spot, 9, true),
        MakeLocalLight(30, RVX::RenderLightType::Point, 5, true),
        MakeLocalLight(10, RVX::RenderLightType::Spot, 20, false),
    };
    const RVX::LightingFramePlan firstLightingPlan =
        MakeLightingPlan(candidates);
    std::reverse(candidates.begin(), candidates.end());
    const RVX::LightingFramePlan reorderedLightingPlan =
        MakeLightingPlan(candidates);
    ASSERT_EQ(firstLightingPlan.GetIdentityHash(),
              reorderedLightingPlan.GetIdentityHash());

    const RVX::LocalShadowFramePlanRequest request = MakeAtlasRequest();
    const RVX::LocalShadowFramePlanBuildResult first =
        RVX::LocalShadowFramePlanBuilder{}.Build(firstLightingPlan, request);
    const RVX::LocalShadowFramePlanBuildResult reordered =
        RVX::LocalShadowFramePlanBuilder{}.Build(
            reorderedLightingPlan, request);
    ASSERT_TRUE(first.Succeeded()) << first.diagnostic;
    ASSERT_TRUE(reordered.Succeeded()) << reordered.diagnostic;
    EXPECT_EQ(first.plan.GetIdentityHash(), reordered.plan.GetIdentityHash());
    EXPECT_EQ(first.plan.GetAllocations(), reordered.plan.GetAllocations());
}

TEST(LocalShadowRenderValidation,
     StableFallbackKeepsLightsButPublishesExactAtlasOverflowEvidence)
{
    const RVX::LightingFramePlan lightingPlan =
        MakeRepresentativeLightingPlan();
    RVX::LocalShadowFramePlanRequest request = MakeAtlasRequest();
    request.atlas.pageCapacity = 7;
    request.atlas.pointLightCapacity = 1;
    request.atlas.spotLightCapacity = 1;

    const RVX::LocalShadowFramePlanBuildResult result =
        RVX::LocalShadowFramePlanBuilder{}.Build(lightingPlan, request);
    ASSERT_TRUE(result.Succeeded()) << result.diagnostic;
    ASSERT_TRUE(result.plan.IsValid());
    ASSERT_EQ(2U, result.plan.GetAllocations().size());
    EXPECT_EQ(20U, result.plan.GetAllocations()[0].lightId);
    EXPECT_EQ(40U, result.plan.GetAllocations()[1].lightId);

    const RVX::LocalShadowFramePlanEvidence& evidence =
        result.plan.GetEvidence();
    EXPECT_EQ(13U, evidence.requestedPageCount);
    EXPECT_EQ(7U, evidence.selectedPageCount);
    EXPECT_EQ(6U, evidence.overflowPageCount);
    EXPECT_EQ(2U, evidence.requestedPointLightCount);
    EXPECT_EQ(1U, evidence.selectedPointLightCount);
    EXPECT_EQ(1U, evidence.overflowPointLightCount);
    EXPECT_EQ(1U, evidence.requestedSpotLightCount);
    EXPECT_EQ(1U, evidence.selectedSpotLightCount);
    EXPECT_EQ(0U, evidence.overflowSpotLightCount);
    EXPECT_TRUE(evidence.overflowed);

    // Local-light admission belongs to LightingFramePlan and is unchanged by
    // an atlas fallback: only shadow visibility is truncated.
    EXPECT_EQ(4U, lightingPlan.GetLocalLights().size());
}

TEST(LocalShadowRenderValidation,
     FailPolicyRejectsCapacityOverflowWithoutPublishingAPartialPlan)
{
    const RVX::LightingFramePlan lightingPlan =
        MakeRepresentativeLightingPlan();
    RVX::LocalShadowFramePlanRequest request = MakeAtlasRequest();
    request.atlas.pageCapacity = 7;
    request.atlas.pointLightCapacity = 1;
    request.atlas.spotLightCapacity = 1;
    request.atlas.overflowPolicy = RVX::LocalShadowOverflowPolicy::Fail;

    const RVX::LocalShadowFramePlanBuildResult result =
        RVX::LocalShadowFramePlanBuilder{}.Build(lightingPlan, request);
    EXPECT_EQ(RVX::LocalShadowFramePlanCode::CapacityExceeded, result.code);
    EXPECT_FALSE(result.Succeeded());
    EXPECT_FALSE(result.plan.IsValid());
    EXPECT_TRUE(result.evidence.overflowed);
    EXPECT_EQ(13U, result.evidence.requestedPageCount);
    EXPECT_EQ(7U, result.evidence.selectedPageCount);
    EXPECT_EQ(6U, result.evidence.overflowPageCount);
    EXPECT_FALSE(result.diagnostic.empty());
}

TEST(LocalShadowRenderValidation,
     GenerationAndLightingIdentityRejectStaleAllocationHandles)
{
    const RVX::LightingFramePlan lightingPlan =
        MakeRepresentativeLightingPlan();
    const RVX::LocalShadowFramePlanRequest request = MakeAtlasRequest();
    const RVX::LocalShadowFramePlanBuildResult result =
        RVX::LocalShadowFramePlanBuilder{}.Build(lightingPlan, request);
    ASSERT_TRUE(result.Succeeded()) << result.diagnostic;
    ASSERT_TRUE(result.plan.IsCompatibleWith(
        lightingPlan, request.atlasGeneration));

    const RVX::LocalShadowAllocation* const selected =
        result.plan.FindAllocation(40);
    ASSERT_NE(nullptr, selected);
    EXPECT_EQ(result.plan.GetIdentityHash(),
              selected->handle.planIdentityHash);
    EXPECT_EQ(selected,
              result.plan.ResolveAllocation(
                  selected->handle, lightingPlan.GetIdentityHash(),
                  request.atlasGeneration));
    EXPECT_EQ(nullptr,
              result.plan.ResolveAllocation(
                  selected->handle, lightingPlan.GetIdentityHash(),
                  request.atlasGeneration + 1));
    EXPECT_EQ(nullptr,
              result.plan.ResolveAllocation(
                  selected->handle, lightingPlan.GetIdentityHash() + 1,
                  request.atlasGeneration));

    RVX::LocalShadowAtlasHandle wrongType = selected->handle;
    wrongType.type = RVX::RenderLightType::Spot;
    EXPECT_EQ(nullptr,
              result.plan.ResolveAllocation(
                  wrongType, lightingPlan.GetIdentityHash(),
                  request.atlasGeneration));

    RVX::RenderFrameIdentity staleFrame = lightingPlan.GetFrameIdentity();
    ++staleFrame.frameSequence;
    EXPECT_FALSE(result.plan.Matches(
        staleFrame, lightingPlan.GetViewIdentity(),
        lightingPlan.GetIdentityHash(), request.atlasGeneration));

    RVX::LocalShadowFramePlanRequest nextRequest = request;
    ++nextRequest.atlasGeneration;
    const RVX::LocalShadowFramePlanBuildResult next =
        RVX::LocalShadowFramePlanBuilder{}.Build(
            lightingPlan, nextRequest);
    ASSERT_TRUE(next.Succeeded()) << next.diagnostic;
    EXPECT_NE(result.plan.GetIdentityHash(), next.plan.GetIdentityHash());
    EXPECT_EQ(nullptr,
              next.plan.ResolveAllocation(
                  selected->handle, lightingPlan.GetIdentityHash(),
                  nextRequest.atlasGeneration));

    // Atlas ownership can remain at the same resource generation while the
    // exact light selection changes. The plan identity carried by the handle
    // prevents a same-slot/same-type stale alias in that case.
    const RVX::LightingFramePlan replacementLightingPlan = MakeLightingPlan({
        MakeLocalLight(99, RVX::RenderLightType::Point, 9, true),
        MakeLocalLight(98, RVX::RenderLightType::Spot, 10, true),
    });
    const RVX::LocalShadowFramePlanBuildResult replacement =
        RVX::LocalShadowFramePlanBuilder{}.Build(
            replacementLightingPlan, request);
    ASSERT_TRUE(replacement.Succeeded()) << replacement.diagnostic;
    ASSERT_EQ(selected->handle.slot,
              replacement.plan.GetAllocations()[1].handle.slot);
    ASSERT_EQ(selected->handle.type,
              replacement.plan.GetAllocations()[1].handle.type);
    EXPECT_EQ(nullptr,
              replacement.plan.ResolveAllocation(
                  selected->handle,
                  replacementLightingPlan.GetIdentityHash(),
                  request.atlasGeneration));
}

TEST(LocalShadowRenderValidation,
     ExpectedIdentitiesPinTheExactSealedInputsAndDeterministicOutput)
{
    const RVX::LightingFramePlan lightingPlan =
        MakeRepresentativeLightingPlan();
    RVX::LocalShadowFramePlanRequest request = MakeAtlasRequest();
    const RVX::LocalShadowFramePlanBuildResult baseline =
        RVX::LocalShadowFramePlanBuilder{}.Build(lightingPlan, request);
    ASSERT_TRUE(baseline.Succeeded()) << baseline.diagnostic;

    request.expectedLightingPlanIdentity = lightingPlan.GetIdentityHash();
    request.expectedIdentityHash = baseline.plan.GetIdentityHash();
    const RVX::LocalShadowFramePlanBuildResult pinned =
        RVX::LocalShadowFramePlanBuilder{}.Build(lightingPlan, request);
    ASSERT_TRUE(pinned.Succeeded()) << pinned.diagnostic;

    RVX::LocalShadowFramePlanRequest staleLighting = request;
    ++staleLighting.expectedLightingPlanIdentity;
    EXPECT_EQ(
        RVX::LocalShadowFramePlanCode::LightingPlanIdentityMismatch,
        RVX::LocalShadowFramePlanBuilder{}
            .Build(lightingPlan, staleLighting)
            .code);

    RVX::LocalShadowFramePlanRequest changedGeneration = request;
    ++changedGeneration.atlasGeneration;
    EXPECT_EQ(RVX::LocalShadowFramePlanCode::IdentityMismatch,
              RVX::LocalShadowFramePlanBuilder{}
                  .Build(lightingPlan, changedGeneration)
                  .code);
}

TEST(LocalShadowRenderValidation,
     RejectsInvalidSchemaGenerationConfigurationAndCheckedArithmetic)
{
    const RVX::LightingFramePlan lightingPlan =
        MakeRepresentativeLightingPlan();

    EXPECT_EQ(RVX::LocalShadowFramePlanCode::InvalidLightingPlan,
              RVX::LocalShadowFramePlanBuilder{}
                  .Build(RVX::LightingFramePlan{}, MakeAtlasRequest())
                  .code);

    RVX::LocalShadowFramePlanRequest invalidSchema = MakeAtlasRequest();
    ++invalidSchema.schemaVersion;
    EXPECT_EQ(RVX::LocalShadowFramePlanCode::InvalidSchema,
              RVX::LocalShadowFramePlanBuilder{}
                  .Build(lightingPlan, invalidSchema)
                  .code);

    RVX::LocalShadowFramePlanRequest invalidGeneration = MakeAtlasRequest();
    invalidGeneration.atlasGeneration = 0;
    EXPECT_EQ(RVX::LocalShadowFramePlanCode::InvalidAtlasGeneration,
              RVX::LocalShadowFramePlanBuilder{}
                  .Build(lightingPlan, invalidGeneration)
                  .code);

    RVX::LocalShadowFramePlanRequest invalidResolution = MakeAtlasRequest();
    invalidResolution.atlas.pageResolution = 0;
    EXPECT_EQ(RVX::LocalShadowFramePlanCode::InvalidAtlasConfiguration,
              RVX::LocalShadowFramePlanBuilder{}
                  .Build(lightingPlan, invalidResolution)
                  .code);

    RVX::LocalShadowFramePlanRequest invalidPolicy = MakeAtlasRequest();
    invalidPolicy.atlas.overflowPolicy =
        static_cast<RVX::LocalShadowOverflowPolicy>(255);
    EXPECT_EQ(RVX::LocalShadowFramePlanCode::InvalidAtlasConfiguration,
              RVX::LocalShadowFramePlanBuilder{}
                  .Build(lightingPlan, invalidPolicy)
                  .code);

    RVX::LocalShadowFramePlanRequest multiplyOverflow = MakeAtlasRequest();
    multiplyOverflow.atlas.pointLightCapacity =
        std::numeric_limits<RVX::uint32>::max();
    multiplyOverflow.atlas.spotLightCapacity = 0;
    EXPECT_EQ(RVX::LocalShadowFramePlanCode::ArithmeticOverflow,
              RVX::LocalShadowFramePlanBuilder{}
                  .Build(lightingPlan, multiplyOverflow)
                  .code);

    RVX::LocalShadowFramePlanRequest additionOverflow = MakeAtlasRequest();
    additionOverflow.atlas.pointLightCapacity =
        std::numeric_limits<RVX::uint32>::max() /
        RVX::RVX_LOCAL_SHADOW_POINT_PAGE_COUNT;
    additionOverflow.atlas.spotLightCapacity =
        RVX::RVX_LOCAL_SHADOW_POINT_PAGE_COUNT;
    EXPECT_EQ(RVX::LocalShadowFramePlanCode::ArithmeticOverflow,
              RVX::LocalShadowFramePlanBuilder{}
                  .Build(lightingPlan, additionOverflow)
                  .code);
}

TEST(LocalShadowRenderValidation,
     EmptyShadowRequestPublishesAValidZeroWorkImmutablePlan)
{
    const RVX::LightingFramePlan lightingPlan = MakeLightingPlan({
        MakeLocalLight(1, RVX::RenderLightType::Point, 1, false),
        MakeLocalLight(2, RVX::RenderLightType::Spot, 1, false),
    });
    RVX::LocalShadowFramePlanRequest request = MakeAtlasRequest();
    request.atlas.pageCapacity = 0;
    request.atlas.pointLightCapacity = 0;
    request.atlas.spotLightCapacity = 0;
    request.atlas.overflowPolicy = RVX::LocalShadowOverflowPolicy::Fail;

    const RVX::LocalShadowFramePlanBuildResult result =
        RVX::LocalShadowFramePlanBuilder{}.Build(lightingPlan, request);
    ASSERT_TRUE(result.Succeeded()) << result.diagnostic;
    EXPECT_TRUE(result.plan.IsValid());
    EXPECT_TRUE(result.plan.GetAllocations().empty());
    EXPECT_EQ(0U, result.plan.GetEvidence().requestedPageCount);
    EXPECT_FALSE(result.plan.GetEvidence().overflowed);
}
