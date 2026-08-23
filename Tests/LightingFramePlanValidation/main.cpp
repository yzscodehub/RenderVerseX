#include "Render/Lighting/LightingFramePlan.h"
#include "Render/Lighting/PhotometricLighting.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <limits>
#include <vector>

namespace
{
    RVX::LightingLightCandidate MakeLight(
        RVX::uint64 id,
        RVX::RenderLightType type,
        RVX::uint32 priority)
    {
        RVX::LightingLightCandidate candidate;
        candidate.lightId = id;
        candidate.type = type;
        candidate.priority = priority;
        candidate.position = RVX::Vec3(static_cast<float>(id), 1.0f, -2.0f);
        candidate.direction = RVX::Vec3(0.0f, -1.0f, 0.0f);
        candidate.color = RVX::Vec3(1.0f, 0.5f, 0.25f);
        candidate.normalizedIntensity = 2.0f;
        candidate.range = type == RVX::RenderLightType::Directional ? 0.0f : 12.0f;
        candidate.innerConeRadians = 0.25f;
        candidate.outerConeRadians = 0.5f;
        candidate.castsShadows = type == RVX::RenderLightType::Directional;
        return candidate;
    }

    RVX::LightingFramePlanRequest MakeRequest()
    {
        RVX::LightingFramePlanRequest request;
        request.frameIdentity = {
            .frameSequence = 11,
            .sceneRevision = 19,
            .surfaceGeneration = 23,
            .configurationRevision = 29};
        request.viewIdentity = {
            .sceneRuntimeId = 31,
            .cameraEntityPacked = 37,
            .subViewIndex = 0,
            .role = RVX::ViewRole::Main};
        request.sceneLightStateHash = 41;
        request.lightingConfigurationRevision = 43;
        request.qualityRevision = 47;
        request.quality = RVX::RenderQualityTier::High;
        request.pointLightCapacity = 4;
        request.spotLightCapacity = 4;
        request.csm = {
            .requested = true,
            .inputsReady = true,
            .cascadeCount = 3,
            .configurationRevision = 53};
        request.ibl = {
            .requested = true,
            .inputsReady = true,
            .resourceRevision = 59,
            .prefilteredMipLevels = 6,
            .normalizedIntensity = 1.25f};
        request.lightCandidates = {
            MakeLight(70, RVX::RenderLightType::Point, 1),
            MakeLight(50, RVX::RenderLightType::Directional, 1),
            MakeLight(30, RVX::RenderLightType::Point, 8),
            MakeLight(10, RVX::RenderLightType::Directional, 8),
            MakeLight(20, RVX::RenderLightType::Spot, 8)};
        return request;
    }
} // namespace

TEST(LightingFramePlanValidation,
     CanonicalizesCandidateOrderAndSelectsStablePrimaryAndLocals)
{
    RVX::LightingFramePlanBuilder builder;
    RVX::LightingFramePlanRequest request = MakeRequest();
    const RVX::LightingFramePlanBuildResult first = builder.Build(request);
    ASSERT_TRUE(first.Succeeded()) << first.diagnostic;
    ASSERT_TRUE(first.plan.IsValid());

    std::reverse(request.lightCandidates.begin(), request.lightCandidates.end());
    const RVX::LightingFramePlanBuildResult reordered = builder.Build(request);
    ASSERT_TRUE(reordered.Succeeded()) << reordered.diagnostic;
    EXPECT_EQ(first.plan.GetIdentityHash(), reordered.plan.GetIdentityHash());

    const RVX::LightingLightCandidate* const primary =
        first.plan.GetPrimaryDirectionalLight();
    ASSERT_NE(nullptr, primary);
    EXPECT_EQ(10U, primary->lightId);
    EXPECT_EQ(8U, primary->priority);

    const std::vector<RVX::LightingLightCandidate>& locals =
        first.plan.GetLocalLights();
    ASSERT_EQ(3U, locals.size());
    EXPECT_EQ(20U, locals[0].lightId);
    EXPECT_EQ(30U, locals[1].lightId);
    EXPECT_EQ(70U, locals[2].lightId);
    EXPECT_EQ(3U, first.plan.GetCapacityEvidence().requestedLocalLightCount);
    EXPECT_FALSE(first.plan.GetCapacityEvidence().overflowed);
}

TEST(LightingFramePlanValidation,
     BlackbodyColorTemperatureIsFiniteLinearAndRejectsInvalidInput)
{
    const RVX::Vec3 warm = RVX::EvaluateBlackbodyColorTemperature(2000.0f);
    const RVX::Vec3 daylight =
        RVX::EvaluateBlackbodyColorTemperature(6500.0f);
    const RVX::Vec3 cool = RVX::EvaluateBlackbodyColorTemperature(12000.0f);

    EXPECT_GT(warm.x, warm.y);
    EXPECT_GT(warm.y, warm.z);
    EXPECT_GT(daylight.x, 0.9f);
    EXPECT_GT(daylight.y, 0.9f);
    EXPECT_GT(daylight.z, 0.85f);
    EXPECT_GT(cool.z, cool.x);
    EXPECT_EQ(RVX::Vec3(0.0f),
              RVX::EvaluateBlackbodyColorTemperature(999.0f));
    EXPECT_EQ(RVX::Vec3(0.0f),
              RVX::EvaluateBlackbodyColorTemperature(
                  std::numeric_limits<float>::infinity()));
}

TEST(LightingFramePlanValidation,
     CandelaReferenceUsesExactInverseSquareBeforeFiniteRangeWindow)
{
    const float atOneMeter = RVX::EvaluatePhotometricInverseSquare(1.0f);
    const float atTwoMeters = RVX::EvaluatePhotometricInverseSquare(2.0f);
    const float atFourMeters = RVX::EvaluatePhotometricInverseSquare(4.0f);

    EXPECT_FLOAT_EQ(1.0f, atOneMeter);
    EXPECT_FLOAT_EQ(atOneMeter / 4.0f, atTwoMeters);
    EXPECT_FLOAT_EQ(atOneMeter / 16.0f, atFourMeters);
    EXPECT_FLOAT_EQ(0.0f, RVX::EvaluatePhotometricInverseSquare(0.0f));
    // Positive near-zero distances retain the shared denominator clamp.  The
    // GPU rejects only exactly zero before normalizing its light direction.
    EXPECT_FLOAT_EQ(
        1.0f / RVX::RVX_PHOTOMETRIC_MIN_DISTANCE_SQUARED,
        RVX::EvaluatePhotometricInverseSquare(1.0e-5f));
    EXPECT_FLOAT_EQ(
        RVX::EvaluatePhotometricInverseSquare(1.0e-5f),
        RVX::EvaluatePhotometricInverseSquare(1.0e-4f));
    EXPECT_FLOAT_EQ(
        0.0f,
        RVX::EvaluatePhotometricInverseSquare(
            std::numeric_limits<float>::infinity()));

    EXPECT_GT(RVX::EvaluateFiniteLightRangeWindow(1.0f, 10.0f), 0.0f);
    EXPECT_FLOAT_EQ(0.0f, RVX::EvaluateFiniteLightRangeWindow(10.0f, 10.0f));
    EXPECT_FLOAT_EQ(
        RVX::EvaluatePhotometricInverseSquare(2.0f) *
            RVX::EvaluateFiniteLightRangeWindow(2.0f, 10.0f),
        RVX::EvaluatePhotometricLocalLightAttenuation(2.0f, 10.0f));
}

TEST(LightingFramePlanValidation, PinnedIdentityRejectsAnyChangedInput)
{
    RVX::LightingFramePlanBuilder builder;
    const RVX::LightingFramePlanBuildResult baseline = builder.Build(MakeRequest());
    ASSERT_TRUE(baseline.Succeeded()) << baseline.diagnostic;

    RVX::LightingFramePlanRequest changed = MakeRequest();
    changed.lightCandidates[0].normalizedIntensity = 3.0f;
    const RVX::LightingFramePlanBuildResult unpinned = builder.Build(changed);
    ASSERT_TRUE(unpinned.Succeeded()) << unpinned.diagnostic;
    EXPECT_NE(baseline.plan.GetIdentityHash(), unpinned.plan.GetIdentityHash());

    changed.expectedIdentityHash = baseline.plan.GetIdentityHash();
    const RVX::LightingFramePlanBuildResult pinned = builder.Build(changed);
    EXPECT_EQ(RVX::LightingFramePlanCode::IdentityMismatch, pinned.code);
    EXPECT_FALSE(pinned.Succeeded());
    EXPECT_FALSE(pinned.plan.IsValid());
    EXPECT_FALSE(pinned.diagnostic.empty());
}

TEST(LightingFramePlanValidation,
     IdentityCanonicalizesDirectionScaleSignedZeroAndIrrelevantFields)
{
    RVX::LightingFramePlanBuilder builder;
    RVX::LightingFramePlanRequest baselineRequest = MakeRequest();
    baselineRequest.lightCandidates[1].direction =
        RVX::Vec3(-0.0f, -2.0f, 0.0f);
    baselineRequest.lightCandidates[1].position =
        RVX::Vec3(-0.0f, 123.0f, -456.0f);
    baselineRequest.lightCandidates[0].direction =
        RVX::Vec3(99.0f, 98.0f, 97.0f);
    baselineRequest.lightCandidates[0].innerConeRadians = 0.1f;
    baselineRequest.lightCandidates[0].outerConeRadians = 0.2f;
    const RVX::LightingFramePlanBuildResult baseline =
        builder.Build(baselineRequest);
    ASSERT_TRUE(baseline.Succeeded()) << baseline.diagnostic;

    RVX::LightingFramePlanRequest equivalent = baselineRequest;
    equivalent.lightCandidates[1].direction =
        RVX::Vec3(0.0f, -20.0f, -0.0f);
    equivalent.lightCandidates[1].position =
        RVX::Vec3(999.0f, -999.0f, 1.0f);
    equivalent.lightCandidates[0].direction = RVX::Vec3(0.0f);
    equivalent.lightCandidates[0].innerConeRadians = 0.0f;
    equivalent.lightCandidates[0].outerConeRadians = 0.0f;
    const RVX::LightingFramePlanBuildResult canonical =
        builder.Build(equivalent);
    ASSERT_TRUE(canonical.Succeeded()) << canonical.diagnostic;
    EXPECT_EQ(baseline.plan.GetIdentityHash(),
              canonical.plan.GetIdentityHash());
}

TEST(LightingFramePlanValidation,
     PreservesUnavailableRequestsAndRejectsInconsistentReadiness)
{
    RVX::LightingFramePlanBuilder builder;

    RVX::LightingFramePlanRequest unavailable = MakeRequest();
    unavailable.csm.inputsReady = false;
    unavailable.ibl.inputsReady = false;
    unavailable.ibl.resourceRevision = 0;
    unavailable.ibl.prefilteredMipLevels = 0;
    const RVX::LightingFramePlanBuildResult preserved =
        builder.Build(unavailable);
    ASSERT_TRUE(preserved.Succeeded()) << preserved.diagnostic;
    EXPECT_TRUE(preserved.plan.GetCSMRequest().requested);
    EXPECT_FALSE(preserved.plan.GetCSMRequest().inputsReady);
    EXPECT_TRUE(preserved.plan.GetIBLRequest().requested);
    EXPECT_FALSE(preserved.plan.GetIBLRequest().inputsReady);

    RVX::LightingFramePlanRequest unsolicitedCSM = MakeRequest();
    unsolicitedCSM.csm.requested = false;
    EXPECT_EQ(RVX::LightingFramePlanCode::InvalidCSMRequest,
              builder.Build(unsolicitedCSM).code);

    RVX::LightingFramePlanRequest invalidIBL = MakeRequest();
    invalidIBL.ibl.resourceRevision = 0;
    EXPECT_EQ(RVX::LightingFramePlanCode::InvalidIBLRequest,
              builder.Build(invalidIBL).code);

    RVX::LightingFramePlanRequest unsolicitedIBL = MakeRequest();
    unsolicitedIBL.ibl.requested = false;
    EXPECT_EQ(RVX::LightingFramePlanCode::InvalidIBLRequest,
              builder.Build(unsolicitedIBL).code);

    RVX::LightingFramePlanRequest duplicate = MakeRequest();
    duplicate.lightCandidates.push_back(duplicate.lightCandidates.front());
    EXPECT_EQ(RVX::LightingFramePlanCode::DuplicateLightCandidate,
              builder.Build(duplicate).code);
}

TEST(LightingFramePlanValidation,
     ValidatesThenCanonicalizesDisabledAndUnavailableLightingRequests)
{
    RVX::LightingFramePlanBuilder builder;

    RVX::LightingFramePlanRequest disabled = MakeRequest();
    disabled.csm = {
        .requested = false,
        .inputsReady = false,
        .cascadeCount = 3,
        .configurationRevision = 97};
    disabled.ibl = {
        .requested = false,
        .inputsReady = false,
        .resourceRevision = 101,
        .prefilteredMipLevels = 5,
        .normalizedIntensity = -0.0f};
    const RVX::LightingFramePlanBuildResult disabledResult =
        builder.Build(disabled);
    ASSERT_TRUE(disabledResult.Succeeded()) << disabledResult.diagnostic;
    EXPECT_FALSE(disabledResult.plan.GetCSMRequest().requested);
    EXPECT_FALSE(disabledResult.plan.GetCSMRequest().inputsReady);
    EXPECT_EQ(0U, disabledResult.plan.GetCSMRequest().cascadeCount);
    EXPECT_EQ(0U, disabledResult.plan.GetCSMRequest().configurationRevision);
    EXPECT_FALSE(disabledResult.plan.GetIBLRequest().requested);
    EXPECT_FALSE(disabledResult.plan.GetIBLRequest().inputsReady);
    EXPECT_EQ(0U, disabledResult.plan.GetIBLRequest().resourceRevision);
    EXPECT_EQ(0U, disabledResult.plan.GetIBLRequest().prefilteredMipLevels);
    EXPECT_FLOAT_EQ(0.0f,
                    disabledResult.plan.GetIBLRequest().normalizedIntensity);

    RVX::LightingFramePlanRequest disabledCanonical = disabled;
    disabledCanonical.csm = {};
    disabledCanonical.ibl = {};
    disabledCanonical.ibl.normalizedIntensity = 0.0f;
    const RVX::LightingFramePlanBuildResult disabledCanonicalResult =
        builder.Build(disabledCanonical);
    ASSERT_TRUE(disabledCanonicalResult.Succeeded())
        << disabledCanonicalResult.diagnostic;
    EXPECT_EQ(disabledResult.plan.GetIdentityHash(),
              disabledCanonicalResult.plan.GetIdentityHash());

    RVX::LightingFramePlanRequest disabledNaN = disabled;
    disabledNaN.ibl.normalizedIntensity =
        std::numeric_limits<float>::quiet_NaN();
    EXPECT_EQ(RVX::LightingFramePlanCode::InvalidIBLRequest,
              builder.Build(disabledNaN).code);

    RVX::LightingFramePlanRequest disabledInvalidCSM = disabled;
    disabledInvalidCSM.csm.cascadeCount =
        RVX::RVX_MAX_DIRECTIONAL_SHADOW_CASCADES + 1U;
    EXPECT_EQ(RVX::LightingFramePlanCode::InvalidCSMRequest,
              builder.Build(disabledInvalidCSM).code);

    RVX::LightingFramePlanRequest unavailable = MakeRequest();
    unavailable.csm.inputsReady = false;
    unavailable.ibl.inputsReady = false;
    unavailable.ibl.normalizedIntensity = -0.0f;
    const RVX::LightingFramePlanBuildResult unavailableResult =
        builder.Build(unavailable);
    ASSERT_TRUE(unavailableResult.Succeeded()) << unavailableResult.diagnostic;
    EXPECT_TRUE(unavailableResult.plan.GetCSMRequest().requested);
    EXPECT_FALSE(unavailableResult.plan.GetCSMRequest().inputsReady);
    EXPECT_EQ(3U, unavailableResult.plan.GetCSMRequest().cascadeCount);
    EXPECT_EQ(53U,
              unavailableResult.plan.GetCSMRequest().configurationRevision);
    EXPECT_TRUE(unavailableResult.plan.GetIBLRequest().requested);
    EXPECT_FALSE(unavailableResult.plan.GetIBLRequest().inputsReady);
    EXPECT_EQ(0U, unavailableResult.plan.GetIBLRequest().resourceRevision);
    EXPECT_EQ(0U,
              unavailableResult.plan.GetIBLRequest().prefilteredMipLevels);
    EXPECT_FLOAT_EQ(0.0f,
                    unavailableResult.plan.GetIBLRequest().normalizedIntensity);

    RVX::LightingFramePlanRequest unavailableEquivalent = unavailable;
    unavailableEquivalent.ibl.resourceRevision = 211;
    unavailableEquivalent.ibl.prefilteredMipLevels = 9;
    unavailableEquivalent.ibl.normalizedIntensity = 0.0f;
    const RVX::LightingFramePlanBuildResult unavailableEquivalentResult =
        builder.Build(unavailableEquivalent);
    ASSERT_TRUE(unavailableEquivalentResult.Succeeded())
        << unavailableEquivalentResult.diagnostic;
    EXPECT_EQ(unavailableResult.plan.GetIdentityHash(),
              unavailableEquivalentResult.plan.GetIdentityHash());
}

TEST(LightingFramePlanValidation, RejectsDegenerateLocalLightGeometry)
{
    RVX::LightingFramePlanBuilder builder;

    RVX::LightingFramePlanRequest zeroRangePoint = MakeRequest();
    zeroRangePoint.lightCandidates[0].range = 0.0f;
    EXPECT_EQ(RVX::LightingFramePlanCode::InvalidLightCandidate,
              builder.Build(zeroRangePoint).code);

    RVX::LightingFramePlanRequest collapsedSpotCone = MakeRequest();
    collapsedSpotCone.lightCandidates.back().innerConeRadians =
        collapsedSpotCone.lightCandidates.back().outerConeRadians;
    EXPECT_EQ(RVX::LightingFramePlanCode::InvalidLightCandidate,
              builder.Build(collapsedSpotCone).code);

    RVX::LightingFramePlanRequest zeroDirection = MakeRequest();
    zeroDirection.lightCandidates[1].direction = RVX::Vec3(0.0f);
    EXPECT_EQ(RVX::LightingFramePlanCode::InvalidLightCandidate,
              builder.Build(zeroDirection).code);
}

TEST(LightingFramePlanValidation,
     ReportsOverflowAndRejectsUndeclaredLocalLightTruncation)
{
    RVX::LightingFramePlanBuilder builder;
    RVX::LightingFramePlanRequest request = MakeRequest();
    request.pointLightCapacity = 1;
    request.spotLightCapacity = 1;

    const RVX::LightingFramePlanBuildResult rejected = builder.Build(request);
    EXPECT_EQ(RVX::LightingFramePlanCode::CapacityExceeded, rejected.code);
    EXPECT_EQ(3U, rejected.capacityEvidence.requestedLocalLightCount);
    EXPECT_EQ(2U, rejected.capacityEvidence.selectedLocalLightCount);
    EXPECT_EQ(1U, rejected.capacityEvidence.overflowLocalLightCount);
    EXPECT_EQ(1U, rejected.capacityEvidence.overflowPointLightCount);
    EXPECT_EQ(0U, rejected.capacityEvidence.overflowSpotLightCount);
    EXPECT_TRUE(rejected.capacityEvidence.overflowed);
    EXPECT_FALSE(rejected.capacityEvidence.truncationAllowed);

    request.allowLocalLightTruncation = true;
    const RVX::LightingFramePlanBuildResult accepted = builder.Build(request);
    ASSERT_TRUE(accepted.Succeeded()) << accepted.diagnostic;
    ASSERT_TRUE(accepted.plan.IsValid());
    EXPECT_TRUE(accepted.plan.GetCapacityEvidence().overflowed);
    ASSERT_EQ(2U, accepted.plan.GetLocalLights().size());
    EXPECT_EQ(20U, accepted.plan.GetLocalLights()[0].lightId);
    EXPECT_EQ(30U, accepted.plan.GetLocalLights()[1].lightId);
}
