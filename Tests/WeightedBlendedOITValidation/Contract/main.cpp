#include "Render/Translucency/TranslucencyFramePlan.h"

#include <array>

#include <gtest/gtest.h>

namespace
{
    RVX::WeightedBlendedOITCapabilityReadiness MakeReadyCapability()
    {
        return {
            .requested = true,
            .compiled = true,
            .hardwareSupported = true,
            .independentBlendSupported = true,
            .accumulationFormatBlendSupported = true,
            .revealageFormatBlendSupported = true,
            .implementationSupported = true,
            .pipelineReady = true,
            .inputsReady = true,
        };
    }

    RVX::TranslucencyFramePlanRequest MakeMixedRequest()
    {
        RVX::TranslucencyFramePlanRequest request;
        request.frameIdentity = {17u, 19u, 23u, 29u};
        request.viewIdentity = {31u, 37u, 0u, RVX::ViewRole::Main};
        request.profileHash = RVX::ComputeRenderPipelineProfileHash(
            RVX::RenderPipelineRecipe::DeferredQuality,
            RVX::RenderQualityTier::High);
        request.meshSourceIdentityHash = 41u;
        request.particleSourceIdentityHash = 43u;
        request.particleItemCount = 2u;
        request.weightedBlendedRequirement =
            RVX::TranslucencyCapabilityRequirement::Required;
        request.weightedBlendedReadiness = MakeReadyCapability();
        request.meshCandidates = {{
            .sourceOrdinal = 0u,
            .sourceIdentityHash = 47u,
            .shadingModel = RVX::MaterialShadingModel::StandardLit,
            .blendMode = RVX::MaterialBlendMode::AlphaBlend,
            .surfacePathEligibility =
                RVX::SurfacePathEligibility::ForwardTranslucency,
            .materialPolicyValid = true,
        }};
        request.particleCandidates = {{
            .sourceOrdinal = 0u,
            .sourceIdentityHash = 53u,
            .firstItem = 0u,
            .itemCount = 2u,
            .blendMode = RVX::ParticleRenderSnapshotBlendMode::Premultiplied,
        }};
        return request;
    }
} // namespace

TEST(WeightedBlendedOITContractValidation,
     RequiredMixedMeshParticlePlanRoutesBothContributorsExactlyOnce)
{
    const RVX::TranslucencyFramePlanBuildResult result =
        RVX::TranslucencyFramePlanBuilder{}.Build(MakeMixedRequest());

    ASSERT_TRUE(result.Succeeded()) << result.diagnostic;
    ASSERT_TRUE(result.plan.IsExecutable());
    EXPECT_EQ(RVX::WeightedBlendedOITCapabilityResolution::Enabled,
              result.plan.GetWeightedBlendedResolution());
    EXPECT_EQ(1u, result.plan.GetSummary().meshCandidateCount);
    EXPECT_EQ(1u, result.plan.GetSummary().particleBatchCandidateCount);
    EXPECT_EQ(2u, result.plan.GetSummary().particleItemCount);
    EXPECT_EQ(2u, result.plan.GetSummary().GetEntryCount(
                      RVX::TranslucencyRoute::WeightedBlended));
    EXPECT_EQ(3u, result.plan.GetSummary().GetItemCount(
                      RVX::TranslucencyRoute::WeightedBlended));
    EXPECT_TRUE(result.plan.GetSummary().IsExactlyOnce());
}

TEST(WeightedBlendedOITContractValidation,
     RequiredMissingIndependentBlendFailsClosedWithoutFallbackRoute)
{
    RVX::TranslucencyFramePlanRequest request = MakeMixedRequest();
    request.weightedBlendedReadiness.independentBlendSupported = false;

    const RVX::TranslucencyFramePlanBuildResult result =
        RVX::TranslucencyFramePlanBuilder{}.Build(request);

    EXPECT_FALSE(result.Succeeded());
    EXPECT_TRUE(result.plan.HasRequiredCapabilityFailure());
    EXPECT_EQ(RVX::TranslucencyFramePlanCode::RequiredCapabilityUnavailable,
              result.code);
    EXPECT_EQ(0u, result.plan.GetSummary().GetItemCount(
                      RVX::TranslucencyRoute::SortedRefraction));
    EXPECT_EQ(3u, result.plan.GetSummary().GetItemCount(
                      RVX::TranslucencyRoute::Unsupported));
}
