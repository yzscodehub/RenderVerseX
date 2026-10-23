#include "Render/Lighting/LightingFramePlan.h"
#include "Render/Lighting/FroxelVolumetricLighting.h"
#include "Render/Lighting/VolumetricLightingFramePlan.h"
#include "RenderContracts/RenderFrameValidation.h"

#include <gtest/gtest.h>

#include <limits>

namespace RVX
{
namespace
{
    [[nodiscard]] RenderFrameIdentity MakeFrameIdentity(uint64 sequence = 8)
    {
        return {
            .frameSequence = sequence,
            .sceneRevision = 17,
            .surfaceGeneration = 23,
            .configurationRevision = 29,
        };
    }

    [[nodiscard]] RenderViewIdentity MakeViewIdentity()
    {
        return {
            .sceneRuntimeId = 31,
            .cameraEntityPacked = 0x0000000B00000007ULL,
            .subViewIndex = 0,
            .role = ViewRole::Main,
        };
    }

    [[nodiscard]] LightingFramePlan MakeLightingPlan(
        RenderQualityTier quality = RenderQualityTier::High,
        uint64 sequence = 8)
    {
        LightingFramePlanRequest request;
        request.frameIdentity = MakeFrameIdentity(sequence);
        request.viewIdentity = MakeViewIdentity();
        request.sceneLightStateHash = 37;
        request.lightingConfigurationRevision = 41;
        request.qualityRevision = 43;
        request.quality = quality;
        request.pointLightCapacity = 4;
        request.spotLightCapacity = 4;
        request.lightCandidates = {
            {.lightId = 47,
             .type = RenderLightType::Directional,
             .priority = 2,
             .direction = {0.0f, -1.0f, 0.0f},
             .color = {1.0f, 0.95f, 0.90f},
             .normalizedIntensity = 120000.0f,
             .castsShadows = true},
            {.lightId = 53,
             .type = RenderLightType::Point,
             .priority = 1,
             .position = {1.0f, 2.0f, 3.0f},
             .normalizedIntensity = 10.0f,
             .range = 12.0f,
             .castsShadows = true},
        };
        const LightingFramePlanBuildResult built =
            LightingFramePlanBuilder{}.Build(request);
        EXPECT_TRUE(built.Succeeded()) << built.diagnostic;
        return built.plan;
    }

    [[nodiscard]] RenderViewHistoryIdentity MakeCompatibleHistoryIdentity(
        uint64 sequence = 7)
    {
        RenderViewHistoryIdentity history;
        history.previousFrame = MakeFrameIdentity(sequence);
        history.view = MakeViewIdentity();
        history.extent = {1920, 1080};
        history.familyId = 59;
        history.worldRevision = 61;
        history.temporalEpoch = 67;
        history.formatRevision = 71;
        history.surfaceGeneration = history.previousFrame.surfaceGeneration;
        history.cameraCutRevision = 73;
        return history;
    }

    [[nodiscard]] VolumetricLightingFramePlanRequest MakeRequest(
        RenderQualityTier quality = RenderQualityTier::High)
    {
        VolumetricLightingFramePlanRequest request;
        request.frameIdentity = MakeFrameIdentity();
        request.viewIdentity = MakeViewIdentity();
        request.outputExtent = {1920, 1080};
        request.quality = quality;
        request.settings.enabled = true;
        request.settings.density = 0.025f;
        request.settings.enableDirectionalShadowing = false;
        request.settings.enableLocalShadowing = false;
        request.settings.enableTemporalReprojection = false;
        request.historyResetReasons = RenderHistoryResetReason::None;
        return request;
    }

    TEST(FroxelVolumetricLightingValidation,
         QualityTiersResolveStableBoundedGridDimensions)
    {
        struct ExpectedGrid
        {
            RenderQualityTier quality;
            uint32 tileSize;
            uint32 depthSlices;
            uint64 froxelCount;
        };
        constexpr ExpectedGrid expected[] = {
            {RenderQualityTier::Low, 16, 32, 261120},
            {RenderQualityTier::Medium, 12, 48, 691200},
            {RenderQualityTier::High, 8, 64, 2073600},
            {RenderQualityTier::Ultra, 8, 96, 3110400},
        };

        for (const ExpectedGrid& item : expected)
        {
            const FroxelGridResolution resolved = ResolveFroxelGridMetadata(
                {1920, 1080}, item.quality);
            ASSERT_TRUE(resolved.Succeeded()) <<
                GetFroxelGridResolutionCodeName(resolved.code);
            EXPECT_EQ(resolved.metadata.tileSizePixels, item.tileSize);
            EXPECT_EQ(resolved.metadata.froxelWidth,
                      1920U / item.tileSize +
                          (1920U % item.tileSize == 0U ? 0U : 1U));
            EXPECT_EQ(resolved.metadata.froxelHeight,
                      1080U / item.tileSize +
                          (1080U % item.tileSize == 0U ? 0U : 1U));
            EXPECT_EQ(resolved.metadata.depthSliceCount, item.depthSlices);
            EXPECT_EQ(resolved.metadata.froxelCount, item.froxelCount);
            EXPECT_TRUE(resolved.metadata.IsValid());
        }
    }

    TEST(FroxelVolumetricLightingValidation,
         GridRejectsInvalidQualityAndBoundedCapacityOverflow)
    {
        const FroxelGridResolution invalidQuality = ResolveFroxelGridMetadata(
            {320, 180}, static_cast<RenderQualityTier>(255));
        EXPECT_EQ(invalidQuality.code, FroxelGridResolutionCode::InvalidQuality);

        const FroxelGridResolution capacityExceeded = ResolveFroxelGridMetadata(
            {4096, 4096}, RenderQualityTier::Ultra);
        EXPECT_EQ(capacityExceeded.code,
                  FroxelGridResolutionCode::CapacityExceeded);

        const FroxelGridResolution minimum = ResolveFroxelGridMetadata(
            {1, 1}, RenderQualityTier::Low);
        ASSERT_TRUE(minimum.Succeeded());
        EXPECT_EQ(minimum.metadata.froxelWidth, 1U);
        EXPECT_EQ(minimum.metadata.froxelHeight, 1U);
        EXPECT_EQ(minimum.metadata.depthSliceCount, 32U);
        EXPECT_EQ(minimum.metadata.froxelCount, 32U);
    }

    TEST(FroxelVolumetricLightingValidation,
         FrameSettingsRejectNonFiniteAndNegativeVolumetricValues)
    {
        RenderFrameSettings settings;
        EXPECT_TRUE(IsValidRenderFrameSettings(settings));

        settings.volumetricLighting.density =
            std::numeric_limits<float32>::quiet_NaN();
        EXPECT_FALSE(IsValidRenderFrameSettings(settings));

        settings.volumetricLighting = {};
        settings.volumetricLighting.anisotropy = -1.0f;
        EXPECT_FALSE(IsValidRenderFrameSettings(settings));

        settings.volumetricLighting = {};
        settings.volumetricLighting.heightFogFalloffPerMeter = -0.01f;
        EXPECT_FALSE(IsValidRenderFrameSettings(settings));
    }

    TEST(FroxelVolumetricLightingValidation,
         ZeroDensityBuildsAnExactIdentityWithoutShadowOrHistoryInputs)
    {
        const LightingFramePlan lightingPlan = MakeLightingPlan();
        VolumetricLightingFramePlanRequest request = MakeRequest();
        request.settings.density = 0.0f;
        request.settings.enableTemporalReprojection = true;
        request.settings.enableDirectionalShadowing = true;
        request.settings.enableLocalShadowing = true;
        request.historyResetReasons =
            RenderHistoryResetReason::HistoryUnavailable;

        const VolumetricLightingFramePlanBuildResult built =
            VolumetricLightingFramePlanBuilder{}.Build(lightingPlan, request);
        ASSERT_TRUE(built.Succeeded()) << built.diagnostic;
        EXPECT_EQ(built.code, VolumetricLightingFramePlanCode::Identity);
        EXPECT_TRUE(built.plan.IsIdentityOutput());
        EXPECT_FALSE(built.plan.UsesTemporalHistory());
        EXPECT_EQ(built.plan.GetHistoryResetReasons(),
                  RenderHistoryResetReason::None);
        EXPECT_EQ(built.plan.GetInputReadiness(),
                  VolumetricLightingInputReadiness{});
        EXPECT_TRUE(built.plan.IsValid());
    }

    TEST(FroxelVolumetricLightingValidation,
         ReverseZDeviceDepthUsesTheSameNearToFarFroxelAxis)
    {
        // The froxel field is always front-to-back: near maps to 0 and far
        // maps to 1, independently of whether the RHI uses reverse-Z.
        EXPECT_FLOAT_EQ(0.0f, CanonicalizeFroxelDeviceDepth(0.0f, false));
        EXPECT_FLOAT_EQ(1.0f, CanonicalizeFroxelDeviceDepth(1.0f, false));
        EXPECT_FLOAT_EQ(0.0f, CanonicalizeFroxelDeviceDepth(1.0f, true));
        EXPECT_FLOAT_EQ(1.0f, CanonicalizeFroxelDeviceDepth(0.0f, true));
        EXPECT_FLOAT_EQ(0.0f, CanonicalizeFroxelDeviceDepth(-1.0f, false));
        EXPECT_FLOAT_EQ(0.0f, CanonicalizeFroxelDeviceDepth(2.0f, true));
    }

    TEST(FroxelVolumetricLightingValidation,
         IdentityFramesThenFirstEnabledFrameInitializeBeforeTemporalReuse)
    {
        // The global frame history can exist while this feature was disabled.
        // Its first enabled frame must carry a reset rather than claiming that
        // global source as a valid froxel Texture3D; the following frame may
        // then reuse the newly committed feature-specific source.
        FroxelVolumetricLighting froxelLighting;
        for (const uint64 identitySequence : {8ULL, 9ULL})
        {
            VolumetricLightingFramePlanRequest identity = MakeRequest();
            identity.frameIdentity = MakeFrameIdentity(identitySequence);
            identity.settings.density = 0.0f;
            identity.settings.enableTemporalReprojection = true;
            identity.historyIdentity =
                MakeCompatibleHistoryIdentity(identitySequence - 1U);
            identity.historyResetReasons = RenderHistoryResetReason::None;
            const VolumetricLightingFramePlanBuildResult built =
                VolumetricLightingFramePlanBuilder{}.Build(
                    MakeLightingPlan(RenderQualityTier::High, identitySequence),
                    identity);
            ASSERT_TRUE(built.Succeeded()) << built.diagnostic;
            EXPECT_TRUE(built.plan.IsIdentityOutput());
            EXPECT_FALSE(built.plan.UsesTemporalHistory());
        }
        EXPECT_FALSE(froxelLighting.HasCompatibleCommittedHistory(
            MakeCompatibleHistoryIdentity(9)));

        VolumetricLightingFramePlanRequest firstEnabled = MakeRequest();
        firstEnabled.frameIdentity = MakeFrameIdentity(10);
        // A coordinator could offer frame 9 here, but no froxel history was
        // committed during the identity frames, so adapter policy supplies
        // HistoryUnavailable.
        firstEnabled.historyIdentity = MakeCompatibleHistoryIdentity(9);
        firstEnabled.settings.enableTemporalReprojection = true;
        firstEnabled.historyResetReasons =
            RenderHistoryResetReason::HistoryUnavailable;
        const VolumetricLightingFramePlanBuildResult initialized =
            VolumetricLightingFramePlanBuilder{}.Build(
                MakeLightingPlan(RenderQualityTier::High, 10), firstEnabled);
        ASSERT_TRUE(initialized.Succeeded()) << initialized.diagnostic;
        EXPECT_FALSE(initialized.plan.UsesTemporalHistory());
        EXPECT_FALSE(initialized.plan.GetHistoryIdentity().IsValid());
        EXPECT_TRUE(HasRenderHistoryResetReason(
            initialized.plan.GetHistoryResetReasons(),
            RenderHistoryResetReason::HistoryUnavailable));

        VolumetricLightingFramePlanRequest reused = MakeRequest();
        reused.frameIdentity = MakeFrameIdentity(11);
        reused.historyIdentity = MakeCompatibleHistoryIdentity(10);
        reused.settings.enableTemporalReprojection = true;
        reused.historyResetReasons = RenderHistoryResetReason::None;
        const VolumetricLightingFramePlanBuildResult reuse =
            VolumetricLightingFramePlanBuilder{}.Build(
                MakeLightingPlan(RenderQualityTier::High, 11), reused);
        ASSERT_TRUE(reuse.Succeeded()) << reuse.diagnostic;
        EXPECT_TRUE(reuse.plan.UsesTemporalHistory());
    }

    TEST(FroxelVolumetricLightingValidation,
         PlanHashIsDeterministicAndRejectsStalePinnedIdentities)
    {
        const LightingFramePlan lightingPlan = MakeLightingPlan();
        VolumetricLightingFramePlanRequest request = MakeRequest();
        const VolumetricLightingFramePlanBuildResult first =
            VolumetricLightingFramePlanBuilder{}.Build(lightingPlan, request);
        const VolumetricLightingFramePlanBuildResult second =
            VolumetricLightingFramePlanBuilder{}.Build(lightingPlan, request);
        ASSERT_TRUE(first.Succeeded()) << first.diagnostic;
        ASSERT_TRUE(second.Succeeded()) << second.diagnostic;
        EXPECT_EQ(first.plan.GetIdentityHash(), second.plan.GetIdentityHash());
        EXPECT_NE(first.plan.GetSettingsHash(), 0U);
        EXPECT_NE(first.plan.GetLightingPlanIdentity(), 0U);

        request.expectedLightingPlanIdentity =
            lightingPlan.GetIdentityHash() + 1U;
        const VolumetricLightingFramePlanBuildResult staleLighting =
            VolumetricLightingFramePlanBuilder{}.Build(lightingPlan, request);
        EXPECT_EQ(staleLighting.code,
                  VolumetricLightingFramePlanCode::LightingPlanIdentityMismatch);

        request.expectedLightingPlanIdentity = lightingPlan.GetIdentityHash();
        request.expectedIdentityHash = first.plan.GetIdentityHash() + 1U;
        const VolumetricLightingFramePlanBuildResult stalePlan =
            VolumetricLightingFramePlanBuilder{}.Build(lightingPlan, request);
        EXPECT_EQ(stalePlan.code,
                  VolumetricLightingFramePlanCode::IdentityMismatch);
    }

    TEST(FroxelVolumetricLightingValidation,
         TemporalPlanUsesOnlyCompatibleHistoryAndRecordsResetWithoutSampling)
    {
        const LightingFramePlan lightingPlan = MakeLightingPlan();
        VolumetricLightingFramePlanRequest request = MakeRequest();
        request.settings.enableTemporalReprojection = true;
        request.historyIdentity = MakeCompatibleHistoryIdentity();
        request.historyResetReasons = RenderHistoryResetReason::None;

        const VolumetricLightingFramePlanBuildResult reusable =
            VolumetricLightingFramePlanBuilder{}.Build(lightingPlan, request);
        ASSERT_TRUE(reusable.Succeeded()) << reusable.diagnostic;
        EXPECT_TRUE(reusable.plan.UsesTemporalHistory());
        EXPECT_EQ(reusable.plan.GetHistoryResetReasons(),
                  RenderHistoryResetReason::None);

        request.historyIdentity.previousFrame.frameSequence =
            request.frameIdentity.frameSequence;
        const VolumetricLightingFramePlanBuildResult stale =
            VolumetricLightingFramePlanBuilder{}.Build(lightingPlan, request);
        EXPECT_EQ(stale.code,
                  VolumetricLightingFramePlanCode::HistoryIdentityIncompatible);

        request.historyIdentity = {};
        request.historyResetReasons = RenderHistoryResetReason::FirstFrame |
            RenderHistoryResetReason::HistoryUnavailable;
        const VolumetricLightingFramePlanBuildResult reset =
            VolumetricLightingFramePlanBuilder{}.Build(lightingPlan, request);
        ASSERT_TRUE(reset.Succeeded()) << reset.diagnostic;
        EXPECT_FALSE(reset.plan.UsesTemporalHistory());
        EXPECT_TRUE(HasRenderHistoryResetReason(
            reset.plan.GetHistoryResetReasons(),
            RenderHistoryResetReason::FirstFrame));
        EXPECT_TRUE(HasRenderHistoryResetReason(
            reset.plan.GetHistoryResetReasons(),
            RenderHistoryResetReason::HistoryUnavailable));
        EXPECT_FALSE(reset.plan.GetHistoryIdentity().IsValid());
    }

    TEST(FroxelVolumetricLightingValidation,
         NonIdentityPlanFailsClosedWhenRequiredShadowEvidenceIsMissing)
    {
        const LightingFramePlan lightingPlan = MakeLightingPlan();
        VolumetricLightingFramePlanRequest request = MakeRequest();
        request.settings.enableDirectionalShadowing = true;
        request.settings.enableLocalShadowing = true;

        const VolumetricLightingFramePlanBuildResult missingDirectional =
            VolumetricLightingFramePlanBuilder{}.Build(lightingPlan, request);
        EXPECT_EQ(missingDirectional.code,
                  VolumetricLightingFramePlanCode::DirectionalShadowUnavailable);

        request.directionalShadowReady = true;
        request.directionalShadowIdentityHash = 79;
        const VolumetricLightingFramePlanBuildResult missingLocal =
            VolumetricLightingFramePlanBuilder{}.Build(lightingPlan, request);
        EXPECT_EQ(missingLocal.code,
                  VolumetricLightingFramePlanCode::LocalShadowUnavailable);

        request.localShadowReady = true;
        request.localShadowIdentityHash = 83;
        const VolumetricLightingFramePlanBuildResult ready =
            VolumetricLightingFramePlanBuilder{}.Build(lightingPlan, request);
        ASSERT_TRUE(ready.Succeeded()) << ready.diagnostic;
        EXPECT_TRUE(ready.plan.GetInputReadiness().directionalShadowRequired);
        EXPECT_TRUE(ready.plan.GetInputReadiness().localShadowRequired);
    }
} // namespace
} // namespace RVX
