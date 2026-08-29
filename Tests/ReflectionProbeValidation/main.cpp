#include "Render/Reflection/ReflectionFramePlan.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <limits>
#include <vector>

namespace
{
    constexpr RVX::uint64 TEST_SCENE_RUNTIME_ID = 101;

    RVX::RenderProbeSnapshot MakeProbe(
        RVX::uint64 id,
        RVX::int32 priority,
        float centerX = 0.0f,
        RVX::uint32 cullingMask = 1U)
    {
        RVX::RenderProbeSnapshot probe;
        probe.probeId = id;
        probe.source = {
            .sceneRuntimeId = TEST_SCENE_RUNTIME_ID,
            .entitySlot = static_cast<RVX::uint32>(id),
            .entityGeneration = 1};
        probe.kind = RVX::RenderProbeKind::Reflection;
        probe.shape = RVX::RenderProbeShape::Box;
        probe.mode = RVX::RenderProbeMode::Baked;
        probe.worldTransform = RVX::Mat4(1.0f);
        probe.worldTransform[3] = RVX::Vec4(centerX, 0.0f, 0.0f, 1.0f);
        probe.influenceExtent = RVX::Vec3(10.0f);
        probe.blendDistance = 10.0f;
        probe.boxProjectionSize = RVX::Vec3(20.0f);
        probe.boxProjectionOffset = RVX::Vec3(0.0f);
        probe.texture = {
            .slot = static_cast<RVX::uint32>(id + 1U),
            .generation = 1};
        probe.cullingMask = cullingMask;
        probe.priority = priority;
        probe.nearClip = 0.1f;
        probe.farClip = 100.0f;
        probe.useBoxProjection = true;
        probe.useHDR = true;
        probe.hasValidData = true;
        return probe;
    }

    RVX::ReflectionFramePlanRequest MakeRequest()
    {
        RVX::ReflectionFramePlanRequest request;
        request.frameIdentity = {
            .frameSequence = 11,
            .sceneRevision = 13,
            .surfaceGeneration = 17,
            .configurationRevision = 19};
        request.viewIdentity = {
            .sceneRuntimeId = TEST_SCENE_RUNTIME_ID,
            .cameraEntityPacked = 23,
            .subViewIndex = 0,
            .role = RVX::ViewRole::Main};
        request.sceneProbeStateHash = 29;
        request.reflectionConfigurationRevision = 31;
        request.viewPosition = RVX::Vec3(0.0f);
        request.viewProjection =
            glm::perspective(glm::radians(70.0f), 1.0f, 0.1f, 100.0f) *
            glm::lookAt(RVX::Vec3(0.0f),
                        RVX::Vec3(0.0f, 0.0f, -1.0f),
                        RVX::Vec3(0.0f, 1.0f, 0.0f));
        request.viewCullingMask = 1U;
        request.ssr = {
            .requested = true,
            .inputsReady = true,
            .configurationIdentityHash = 37,
            .pipelineIdentityHash = 41,
            .depthPyramidIdentityHash = 43,
            .historyIdentityHash = 47};
        request.ibl = {
            .source = RVX::ReflectionIBLSource::Texture,
            .lightingPlanIdentityHash = 53,
            .resourceRevision = 59,
            .prefilteredMipLevels = 6,
            .analyticFallbackIdentityHash = 0};
        request.fallback = {
            .policy = RVX::ReflectionFallbackPolicy::ScreenSpaceProbeIBL,
            .configurationIdentityHash = 61};
        return request;
    }

    RVX::ReflectionFramePlanBuildResult BuildOne(
        RVX::RenderProbeSnapshot probe)
    {
        RVX::ReflectionFramePlanRequest request = MakeRequest();
        request.probeCandidates.push_back(std::move(probe));
        return RVX::ReflectionFramePlanBuilder{}.Build(request);
    }
} // namespace

TEST(ReflectionProbeValidation,
     SelectionIsPriorityThenQuantizedInfluenceThenStableId)
{
    RVX::ReflectionFramePlanRequest request = MakeRequest();
    request.probeCandidates = {
        MakeProbe(40, 5, 2.0f),
        MakeProbe(30, 5, 5.0f),
        MakeProbe(10, 10, 9.0f),
        MakeProbe(20, 5, 5.0f)};
    request.probeCandidates.front().mode = RVX::RenderProbeMode::Custom;

    const RVX::ReflectionFramePlanBuildResult baseline =
        RVX::ReflectionFramePlanBuilder{}.Build(request);
    ASSERT_TRUE(baseline.Succeeded()) << baseline.diagnostic;
    ASSERT_TRUE(baseline.plan.IsValid());
    const auto& probes = baseline.plan.GetSelectedProbes();
    ASSERT_EQ(4U, probes.size());
    EXPECT_EQ(10U, probes[0].snapshot.probeId);
    EXPECT_EQ(40U, probes[1].snapshot.probeId);
    EXPECT_EQ(20U, probes[2].snapshot.probeId);
    EXPECT_EQ(30U, probes[3].snapshot.probeId);
    EXPECT_GT(probes[1].quantizedInfluenceScore,
              probes[2].quantizedInfluenceScore);
    EXPECT_EQ(probes[2].quantizedInfluenceScore,
              probes[3].quantizedInfluenceScore);

    std::reverse(
        request.probeCandidates.begin(), request.probeCandidates.end());
    const RVX::ReflectionFramePlanBuildResult reordered =
        RVX::ReflectionFramePlanBuilder{}.Build(request);
    ASSERT_TRUE(reordered.Succeeded()) << reordered.diagnostic;
    EXPECT_EQ(baseline.plan.GetIdentityHash(),
              reordered.plan.GetIdentityHash());
    ASSERT_EQ(probes.size(), reordered.plan.GetSelectedProbes().size());
    for (size_t index = 0; index < probes.size(); ++index)
    {
        EXPECT_EQ(probes[index].snapshot.probeId,
                  reordered.plan.GetSelectedProbes()[index].snapshot.probeId);
    }
}

TEST(ReflectionProbeValidation, FixedTopNCapacityReportsDeterministicOverflow)
{
    RVX::ReflectionFramePlanRequest request = MakeRequest();
    for (RVX::uint64 id = 1; id <= RVX::RVX_REFLECTION_PROBE_CAPACITY + 2U;
         ++id)
    {
        request.probeCandidates.push_back(MakeProbe(id, 1));
    }

    const RVX::ReflectionFramePlanBuildResult result =
        RVX::ReflectionFramePlanBuilder{}.Build(request);
    ASSERT_TRUE(result.Succeeded()) << result.diagnostic;
    const auto& evidence = result.plan.GetCapacityEvidence();
    EXPECT_EQ(RVX::RVX_REFLECTION_PROBE_CAPACITY, evidence.probeCapacity);
    EXPECT_EQ(10U, evidence.candidateProbeCount);
    EXPECT_EQ(10U, evidence.eligibleProbeCount);
    EXPECT_EQ(RVX::RVX_REFLECTION_PROBE_CAPACITY,
              evidence.selectedProbeCount);
    EXPECT_EQ(2U, evidence.overflowProbeCount);
    EXPECT_TRUE(evidence.overflowed);
    ASSERT_EQ(RVX::RVX_REFLECTION_PROBE_CAPACITY,
              result.plan.GetSelectedProbes().size());
    for (RVX::uint64 index = 0;
         index < RVX::RVX_REFLECTION_PROBE_CAPACITY;
         ++index)
    {
        EXPECT_EQ(index + 1U,
                  result.plan.GetSelectedProbes()[index].snapshot.probeId);
    }
}

TEST(ReflectionProbeValidation,
     LayerAndFrustumFilteringPreserveVolumesIntersectingVisibleSurfaces)
{
    RVX::ReflectionFramePlanRequest request = MakeRequest();
    request.probeCandidates = {
        MakeProbe(1, 1, 0.0f, 1U),
        MakeProbe(2, 100, 0.0f, 2U),
        MakeProbe(3, 100, 12.0f, 1U)};

    const RVX::ReflectionFramePlanBuildResult result =
        RVX::ReflectionFramePlanBuilder{}.Build(request);
    ASSERT_TRUE(result.Succeeded()) << result.diagnostic;
    const auto& evidence = result.plan.GetCapacityEvidence();
    EXPECT_EQ(3U, evidence.candidateProbeCount);
    EXPECT_EQ(2U, evidence.eligibleProbeCount);
    EXPECT_EQ(2U, evidence.selectedProbeCount);
    EXPECT_EQ(1U, evidence.layerRejectedProbeCount);
    EXPECT_EQ(0U, evidence.frustumRejectedProbeCount);
    EXPECT_EQ(0U, evidence.overflowProbeCount);
    EXPECT_FALSE(evidence.overflowed);
    ASSERT_EQ(2U, result.plan.GetSelectedProbes().size());
    EXPECT_EQ(3U, result.plan.GetSelectedProbes()[0].snapshot.probeId);
    EXPECT_FLOAT_EQ(
        0.0f, result.plan.GetSelectedProbes()[0].influenceWeight);
    EXPECT_EQ(1U, result.plan.GetSelectedProbes()[1].snapshot.probeId);
}

TEST(ReflectionProbeValidation,
     OffscreenHighPriorityProbesCannotConsumeVisibleProbeCapacity)
{
    RVX::ReflectionFramePlanRequest request = MakeRequest();
    for (RVX::uint64 id = 1;
         id <= RVX::RVX_REFLECTION_PROBE_CAPACITY + 2U;
         ++id)
    {
        request.probeCandidates.push_back(
            MakeProbe(id, 1000, 100.0f + static_cast<float>(id) * 25.0f));
    }
    request.probeCandidates.push_back(MakeProbe(100, 1, 0.0f));

    const RVX::ReflectionFramePlanBuildResult result =
        RVX::ReflectionFramePlanBuilder{}.Build(request);
    ASSERT_TRUE(result.Succeeded()) << result.diagnostic;
    ASSERT_TRUE(result.plan.IsValid());
    const auto& evidence = result.plan.GetCapacityEvidence();
    EXPECT_EQ(11U, evidence.candidateProbeCount);
    EXPECT_EQ(10U, evidence.frustumRejectedProbeCount);
    EXPECT_EQ(1U, evidence.eligibleProbeCount);
    EXPECT_EQ(1U, evidence.selectedProbeCount);
    EXPECT_EQ(0U, evidence.overflowProbeCount);
    ASSERT_EQ(1U, result.plan.GetSelectedProbes().size());
    EXPECT_EQ(100U, result.plan.GetSelectedProbes().front().snapshot.probeId);
}

TEST(ReflectionProbeValidation,
     InfluenceReferenceHandlesBoxSphereBlendAndBoundary)
{
    RVX::RenderProbeSnapshot box = MakeProbe(1, 1);
    EXPECT_FLOAT_EQ(1.0f,
                    RVX::EvaluateReflectionProbeInfluence(
                        box, RVX::Vec3(0.0f)));
    EXPECT_FLOAT_EQ(0.5f,
                    RVX::EvaluateReflectionProbeInfluence(
                        box, RVX::Vec3(5.0f, 0.0f, 0.0f)));
    EXPECT_FLOAT_EQ(0.0f,
                    RVX::EvaluateReflectionProbeInfluence(
                        box, RVX::Vec3(10.0f, 0.0f, 0.0f)));

    RVX::RenderProbeSnapshot sphere = box;
    sphere.shape = RVX::RenderProbeShape::Sphere;
    sphere.useBoxProjection = false;
    EXPECT_FLOAT_EQ(0.5f,
                    RVX::EvaluateReflectionProbeInfluence(
                        sphere, RVX::Vec3(3.0f, 4.0f, 0.0f)));
    EXPECT_EQ(500000U, RVX::QuantizeReflectionProbeInfluence(0.5f));
    EXPECT_EQ(RVX::RVX_REFLECTION_INFLUENCE_SCORE_MAX,
              RVX::QuantizeReflectionProbeInfluence(2.0f));
    EXPECT_EQ(0U, RVX::QuantizeReflectionProbeInfluence(
                      std::numeric_limits<float>::quiet_NaN()));
}

TEST(ReflectionProbeValidation,
     RejectsUnsupportedOrMalformedProbeSnapshotsFailClosed)
{
    RVX::RenderProbeSnapshot realtime = MakeProbe(1, 1);
    realtime.mode = RVX::RenderProbeMode::Realtime;
    EXPECT_EQ(RVX::ReflectionFramePlanCode::UnsupportedProbeMode,
              BuildOne(realtime).code);

    std::vector<RVX::RenderProbeSnapshot> invalidSnapshots;
    RVX::RenderProbeSnapshot lightProbe = MakeProbe(1, 1);
    lightProbe.kind = RVX::RenderProbeKind::Light;
    invalidSnapshots.push_back(lightProbe);
    RVX::RenderProbeSnapshot invalidMode = MakeProbe(1, 1);
    invalidMode.mode = static_cast<RVX::RenderProbeMode>(255);
    invalidSnapshots.push_back(invalidMode);
    RVX::RenderProbeSnapshot invalidShape = MakeProbe(1, 1);
    invalidShape.shape = static_cast<RVX::RenderProbeShape>(255);
    invalidSnapshots.push_back(invalidShape);
    RVX::RenderProbeSnapshot missingTexture = MakeProbe(1, 1);
    missingTexture.texture = {};
    invalidSnapshots.push_back(missingTexture);
    RVX::RenderProbeSnapshot unavailableButMalformed = MakeProbe(1, 1);
    unavailableButMalformed.hasValidData = false;
    unavailableButMalformed.influenceExtent.x =
        std::numeric_limits<float>::quiet_NaN();
    invalidSnapshots.push_back(unavailableButMalformed);
    RVX::RenderProbeSnapshot emptyCullingMask = MakeProbe(1, 1);
    emptyCullingMask.cullingMask = 0;
    invalidSnapshots.push_back(emptyCullingMask);
    RVX::RenderProbeSnapshot invalidExtent = MakeProbe(1, 1);
    invalidExtent.influenceExtent.x =
        std::numeric_limits<float>::quiet_NaN();
    invalidSnapshots.push_back(invalidExtent);
    RVX::RenderProbeSnapshot invalidBlend = MakeProbe(1, 1);
    invalidBlend.blendDistance = 11.0f;
    invalidSnapshots.push_back(invalidBlend);
    RVX::RenderProbeSnapshot singularTransform = MakeProbe(1, 1);
    singularTransform.worldTransform[0][0] = 0.0f;
    invalidSnapshots.push_back(singularTransform);
    RVX::RenderProbeSnapshot invalidProjection = MakeProbe(1, 1);
    invalidProjection.shape = RVX::RenderProbeShape::Sphere;
    invalidSnapshots.push_back(invalidProjection);
    RVX::RenderProbeSnapshot invalidClip = MakeProbe(1, 1);
    invalidClip.farClip = invalidClip.nearClip;
    invalidSnapshots.push_back(invalidClip);
    RVX::RenderProbeSnapshot invalidSH = MakeProbe(1, 1);
    invalidSH.sphericalHarmonics[0].x =
        std::numeric_limits<float>::infinity();
    invalidSnapshots.push_back(invalidSH);
    RVX::RenderProbeSnapshot wrongScene = MakeProbe(1, 1);
    wrongScene.source.sceneRuntimeId = TEST_SCENE_RUNTIME_ID + 1U;
    invalidSnapshots.push_back(wrongScene);

    for (const RVX::RenderProbeSnapshot& invalid : invalidSnapshots)
    {
        const RVX::ReflectionFramePlanBuildResult result = BuildOne(invalid);
        EXPECT_EQ(RVX::ReflectionFramePlanCode::InvalidProbeSnapshot,
                  result.code)
            << result.diagnostic;
        EXPECT_FALSE(result.Succeeded());
        EXPECT_FALSE(result.plan.IsValid());
    }

    RVX::ReflectionFramePlanRequest duplicate = MakeRequest();
    RVX::RenderProbeSnapshot unavailableDuplicate = MakeProbe(1, 1);
    unavailableDuplicate.hasValidData = false;
    duplicate.probeCandidates = {MakeProbe(1, 1), unavailableDuplicate};
    EXPECT_EQ(RVX::ReflectionFramePlanCode::DuplicateProbeCandidate,
              RVX::ReflectionFramePlanBuilder{}.Build(duplicate).code);
}

TEST(ReflectionProbeValidation,
     DeliberatelyUnavailableProbeIsFilteredBeforeCapacityAdmission)
{
    RVX::ReflectionFramePlanRequest request = MakeRequest();
    RVX::RenderProbeSnapshot unavailable = MakeProbe(10, 100);
    unavailable.hasValidData = false;
    unavailable.texture = {};
    request.probeCandidates = {unavailable, MakeProbe(20, 1)};

    const RVX::ReflectionFramePlanBuildResult result =
        RVX::ReflectionFramePlanBuilder{}.Build(request);
    ASSERT_TRUE(result.Succeeded()) << result.diagnostic;
    ASSERT_TRUE(result.plan.IsValid());

    const auto& evidence = result.plan.GetCapacityEvidence();
    EXPECT_EQ(2U, evidence.candidateProbeCount);
    EXPECT_EQ(1U, evidence.unavailableProbeCount);
    EXPECT_EQ(1U, evidence.eligibleProbeCount);
    EXPECT_EQ(1U, evidence.selectedProbeCount);
    EXPECT_EQ(0U, evidence.layerRejectedProbeCount);
    EXPECT_EQ(0U, evidence.frustumRejectedProbeCount);
    EXPECT_EQ(0U, evidence.overflowProbeCount);
    ASSERT_EQ(1U, result.plan.GetSelectedProbes().size());
    EXPECT_EQ(20U, result.plan.GetSelectedProbes().front().snapshot.probeId);
}

TEST(ReflectionProbeValidation,
     OnlyUnavailableProbesSealAnIBLOnlyFallbackPlan)
{
    RVX::ReflectionFramePlanRequest request = MakeRequest();
    RVX::RenderProbeSnapshot unavailable = MakeProbe(10, 1);
    unavailable.hasValidData = false;
    unavailable.texture = {};
    request.probeCandidates = {unavailable};

    const RVX::ReflectionFramePlanBuildResult result =
        RVX::ReflectionFramePlanBuilder{}.Build(request);
    ASSERT_TRUE(result.Succeeded()) << result.diagnostic;
    ASSERT_TRUE(result.plan.IsValid());

    const auto& evidence = result.plan.GetCapacityEvidence();
    EXPECT_EQ(1U, evidence.candidateProbeCount);
    EXPECT_EQ(1U, evidence.unavailableProbeCount);
    EXPECT_EQ(0U, evidence.eligibleProbeCount);
    EXPECT_EQ(0U, evidence.selectedProbeCount);
    EXPECT_TRUE(result.plan.GetSelectedProbes().empty());

    RVX::ReflectionRouteSample sample;
    sample.iblRadiance = RVX::Vec3(0.2f, 0.3f, 0.4f);
    sample.iblValid = true;
    const RVX::ReflectionRouteResolution route =
        RVX::ResolveReflectionRoute(sample);
    ASSERT_TRUE(route.Succeeded()) << route.diagnostic;
    EXPECT_EQ(RVX::ReflectionResolutionRoute::IBL, route.route);
    EXPECT_EQ(sample.iblRadiance, route.radiance);
}

TEST(ReflectionProbeValidation, PinsCompleteSSRIBLAndFallbackIdentity)
{
    RVX::ReflectionFramePlanRequest request = MakeRequest();
    request.probeCandidates = {MakeProbe(1, 1)};
    const RVX::ReflectionFramePlanBuildResult baseline =
        RVX::ReflectionFramePlanBuilder{}.Build(request);
    ASSERT_TRUE(baseline.Succeeded()) << baseline.diagnostic;

    request.fallback.configurationIdentityHash += 1U;
    const RVX::ReflectionFramePlanBuildResult changed =
        RVX::ReflectionFramePlanBuilder{}.Build(request);
    ASSERT_TRUE(changed.Succeeded()) << changed.diagnostic;
    EXPECT_NE(baseline.plan.GetIdentityHash(), changed.plan.GetIdentityHash());

    request.expectedIdentityHash = baseline.plan.GetIdentityHash();
    const RVX::ReflectionFramePlanBuildResult pinned =
        RVX::ReflectionFramePlanBuilder{}.Build(request);
    EXPECT_EQ(RVX::ReflectionFramePlanCode::IdentityMismatch, pinned.code);
    EXPECT_TRUE(pinned.capacityEvidence.selectedProbeCount != 0);
    EXPECT_FALSE(pinned.plan.IsValid());

    RVX::ReflectionFramePlanRequest availabilityChanged = MakeRequest();
    availabilityChanged.probeCandidates = {MakeProbe(1, 1)};
    const RVX::ReflectionFramePlanBuildResult available =
        RVX::ReflectionFramePlanBuilder{}.Build(availabilityChanged);
    ASSERT_TRUE(available.Succeeded()) << available.diagnostic;
    availabilityChanged.probeCandidates.front().hasValidData = false;
    availabilityChanged.expectedIdentityHash = available.plan.GetIdentityHash();
    EXPECT_EQ(RVX::ReflectionFramePlanCode::IdentityMismatch,
              RVX::ReflectionFramePlanBuilder{}.Build(availabilityChanged)
                  .code);
}

TEST(ReflectionProbeValidation,
     AcceptsExactAnalyticIBLAndRejectsInconsistentSSRAndIBLIdentity)
{
    RVX::ReflectionFramePlanRequest analytic = MakeRequest();
    analytic.ibl = {
        .source = RVX::ReflectionIBLSource::Analytic,
        .lightingPlanIdentityHash = 71,
        .resourceRevision = 0,
        .prefilteredMipLevels = 0,
        .analyticFallbackIdentityHash = 73};
    const RVX::ReflectionFramePlanBuildResult analyticResult =
        RVX::ReflectionFramePlanBuilder{}.Build(analytic);
    ASSERT_TRUE(analyticResult.Succeeded()) << analyticResult.diagnostic;
    EXPECT_TRUE(analyticResult.plan.IsValid());

    RVX::ReflectionFramePlanRequest missingTextureIdentity = MakeRequest();
    missingTextureIdentity.ibl.resourceRevision = 0;
    EXPECT_EQ(RVX::ReflectionFramePlanCode::InvalidIBLIdentity,
              RVX::ReflectionFramePlanBuilder{}
                  .Build(missingTextureIdentity)
                  .code);

    RVX::ReflectionFramePlanRequest disabledWithStaleIdentity = MakeRequest();
    disabledWithStaleIdentity.ssr.requested = false;
    disabledWithStaleIdentity.ssr.inputsReady = false;
    EXPECT_EQ(RVX::ReflectionFramePlanCode::InvalidSSRIdentity,
              RVX::ReflectionFramePlanBuilder{}
                  .Build(disabledWithStaleIdentity)
                  .code);

    RVX::ReflectionFramePlanRequest unavailable = MakeRequest();
    unavailable.ssr = {
        .requested = true,
        .inputsReady = false,
        .configurationIdentityHash = 79,
        .pipelineIdentityHash = 0,
        .depthPyramidIdentityHash = 0,
        .historyIdentityHash = 0};
    EXPECT_TRUE(
        RVX::ReflectionFramePlanBuilder{}.Build(unavailable).Succeeded());

    RVX::ReflectionFramePlanRequest readyWithoutDepth = MakeRequest();
    readyWithoutDepth.ssr.depthPyramidIdentityHash = 0;
    EXPECT_EQ(RVX::ReflectionFramePlanCode::InvalidSSRIdentity,
              RVX::ReflectionFramePlanBuilder{}
                  .Build(readyWithoutDepth)
                  .code);

    RVX::ReflectionFramePlanRequest invalidViewProjection = MakeRequest();
    invalidViewProjection.viewProjection[2][1] =
        std::numeric_limits<float>::quiet_NaN();
    EXPECT_EQ(RVX::ReflectionFramePlanCode::InvalidViewState,
              RVX::ReflectionFramePlanBuilder{}
                  .Build(invalidViewProjection)
                  .code);
}

TEST(ReflectionProbeValidation,
     RouteResolverPreservesIBLWhenSSRAndProbeMiss)
{
    RVX::ReflectionRouteSample sample;
    sample.iblRadiance = RVX::Vec3(0.2f, 0.3f, 0.4f);
    sample.iblValid = true;
    const RVX::ReflectionRouteResolution result =
        RVX::ResolveReflectionRoute(sample);
    ASSERT_TRUE(result.Succeeded()) << result.diagnostic;
    EXPECT_EQ(RVX::ReflectionResolutionRoute::IBL, result.route);
    EXPECT_EQ(sample.iblRadiance, result.radiance);
    EXPECT_FLOAT_EQ(0.0f, result.ssrWeight);
    EXPECT_FLOAT_EQ(0.0f, result.probeWeight);
    EXPECT_FLOAT_EQ(1.0f, result.iblWeight);
}

TEST(ReflectionProbeValidation,
     RouteResolverProducesExplicitSSRProbeIBLWeights)
{
    RVX::ReflectionRouteSample sample;
    sample.ssrRadiance = RVX::Vec3(1.0f, 0.0f, 0.0f);
    sample.ssrConfidence = 0.25f;
    sample.ssrHitValid = true;
    sample.probeRadiance = RVX::Vec3(0.0f, 1.0f, 0.0f);
    sample.probeConfidence = 0.5f;
    sample.probeValid = true;
    sample.iblRadiance = RVX::Vec3(0.0f, 0.0f, 1.0f);
    sample.iblValid = true;

    const RVX::ReflectionRouteResolution result =
        RVX::ResolveReflectionRoute(sample);
    ASSERT_TRUE(result.Succeeded()) << result.diagnostic;
    EXPECT_EQ(RVX::ReflectionResolutionRoute::SSRProbeAndIBL, result.route);
    EXPECT_FLOAT_EQ(0.25f, result.ssrWeight);
    EXPECT_FLOAT_EQ(0.375f, result.probeWeight);
    EXPECT_FLOAT_EQ(0.375f, result.iblWeight);
    EXPECT_EQ(RVX::Vec3(0.25f, 0.375f, 0.375f), result.radiance);

    sample.ssrConfidence = 1.0f;
    const RVX::ReflectionRouteResolution fullSSR =
        RVX::ResolveReflectionRoute(sample);
    ASSERT_TRUE(fullSSR.Succeeded()) << fullSSR.diagnostic;
    EXPECT_EQ(RVX::ReflectionResolutionRoute::SSR, fullSSR.route);
    EXPECT_EQ(sample.ssrRadiance, fullSSR.radiance);
}

TEST(ReflectionProbeValidation,
     RouteResolverRejectsMissingFinalFallbackAndInconsistentConfidence)
{
    RVX::ReflectionRouteSample missingIBL;
    EXPECT_FALSE(RVX::ResolveReflectionRoute(missingIBL).Succeeded());

    RVX::ReflectionRouteSample staleSSR;
    staleSSR.iblValid = true;
    staleSSR.iblRadiance = RVX::Vec3(0.1f);
    staleSSR.ssrConfidence = 0.5f;
    staleSSR.ssrHitValid = false;
    EXPECT_FALSE(RVX::ResolveReflectionRoute(staleSSR).Succeeded());

    RVX::ReflectionRouteSample invalidConfidence = staleSSR;
    invalidConfidence.ssrHitValid = true;
    invalidConfidence.ssrConfidence =
        std::numeric_limits<float>::quiet_NaN();
    EXPECT_FALSE(RVX::ResolveReflectionRoute(invalidConfidence).Succeeded());

    RVX::ReflectionRouteSample blackIBL;
    blackIBL.iblValid = true;
    const RVX::ReflectionRouteResolution deliberateBlack =
        RVX::ResolveReflectionRoute(blackIBL);
    EXPECT_TRUE(deliberateBlack.Succeeded());
    EXPECT_EQ(RVX::ReflectionResolutionRoute::IBL, deliberateBlack.route);
    EXPECT_EQ(RVX::Vec3(0.0f), deliberateBlack.radiance);
}
