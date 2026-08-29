#include "Render/Graph/RenderGraph.h"
#include "Render/Lighting/LightingFramePlan.h"
#include "Render/Lighting/LocalShadowAtlas.h"
#include "Render/Lighting/LocalShadowFramePlan.h"
#include "Render/Passes/LocalShadowPass.h"

#include <gtest/gtest.h>

#include <memory>
#include <vector>

namespace
{
    struct Plans
    {
        RVX::LightingFramePlan lighting{};
        RVX::LocalShadowFramePlan localShadow{};
        std::shared_ptr<const RVX::LocalShadowAtlasPlan> atlas{};
    };

    RVX::LightingLightCandidate MakeLight(
        RVX::uint64 id,
        RVX::RenderLightType type)
    {
        RVX::LightingLightCandidate light;
        light.lightId = id;
        light.type = type;
        light.priority = static_cast<RVX::uint32>(id);
        light.position = RVX::Vec3(static_cast<float>(id), 2.0f, -3.0f);
        light.direction = type == RVX::RenderLightType::Spot
            ? RVX::Vec3(0.0f, -1.0f, -1.0f)
            : RVX::Vec3(0.0f);
        light.color = RVX::Vec3(1.0f);
        light.normalizedIntensity = 8.0f;
        light.range = 20.0f;
        light.innerConeRadians = type == RVX::RenderLightType::Spot ? 0.2f : 0.0f;
        light.outerConeRadians = type == RVX::RenderLightType::Spot ? 0.4f : 0.0f;
        light.castsShadows = true;
        return light;
    }

    Plans MakePlans()
    {
        RVX::LightingFramePlanRequest lightingRequest;
        lightingRequest.frameIdentity = {
            .frameSequence = 101,
            .sceneRevision = 103,
            .surfaceGeneration = 107,
            .configurationRevision = 109};
        lightingRequest.viewIdentity = {
            .sceneRuntimeId = 113,
            .cameraEntityPacked = 127,
            .subViewIndex = 0,
            .role = RVX::ViewRole::Main};
        lightingRequest.sceneLightStateHash = 131;
        lightingRequest.lightingConfigurationRevision = 137;
        lightingRequest.qualityRevision = 139;
        lightingRequest.quality = RVX::RenderQualityTier::High;
        lightingRequest.pointLightCapacity = 4;
        lightingRequest.spotLightCapacity = 4;
        lightingRequest.lightCandidates = {
            MakeLight(17, RVX::RenderLightType::Spot),
            MakeLight(19, RVX::RenderLightType::Point)};
        const RVX::LightingFramePlanBuildResult lighting =
            RVX::LightingFramePlanBuilder{}.Build(lightingRequest);
        EXPECT_TRUE(lighting.Succeeded()) << lighting.diagnostic;

        RVX::LocalShadowFramePlanRequest localRequest;
        localRequest.atlas = {
            .pageResolution = 128,
            .pageCapacity = 8,
            .pointLightCapacity = 1,
            .spotLightCapacity = 1,
            .overflowPolicy = RVX::LocalShadowOverflowPolicy::Fail};
        localRequest.atlasGeneration = 149;
        const RVX::LocalShadowFramePlanBuildResult local =
            RVX::LocalShadowFramePlanBuilder{}.Build(lighting.plan, localRequest);
        EXPECT_TRUE(local.Succeeded()) << local.diagnostic;

        RVX::LocalShadowAtlasPlanRequest atlasRequest;
        atlasRequest.atlasGeneration = localRequest.atlasGeneration;
        atlasRequest.nearPlane = 0.1f;
        const RVX::LocalShadowAtlasPlanBuildResult atlas =
            RVX::LocalShadowAtlasPlanBuilder{}.Build(
                lighting.plan, local.plan, atlasRequest);
        EXPECT_TRUE(atlas.Succeeded()) << atlas.diagnostic;

        Plans result;
        result.lighting = lighting.plan;
        result.localShadow = local.plan;
        result.atlas = std::make_shared<RVX::LocalShadowAtlasPlan>(atlas.plan);
        return result;
    }
} // namespace

TEST(LocalShadowPassValidation,
     RegistersOneExactDepthLayerPassForEveryImmutablePointAndSpotPage)
{
    const Plans plans = MakePlans();
    ASSERT_NE(nullptr, plans.atlas);
    ASSERT_EQ(7U, plans.atlas->GetPages().size());

    RVX::RenderGraph graph;
    RVX::LocalShadowPass pass;
    RVX::LocalShadowPassRequest request;
    request.atlasPlan = plans.atlas;
    request.expectedAtlasPlanIdentity = plans.atlas->GetIdentityHash();
    request.expectedAtlasGeneration = plans.atlas->GetAtlasGeneration();
    const RVX::LocalShadowPassOutput output = pass.AddToGraph(graph, request);

    ASSERT_TRUE(output.IsValid()) << output.diagnostic;
    EXPECT_EQ(RVX::LocalShadowPassCode::Ready, output.code);
    EXPECT_EQ(plans.atlas->GetPages().size(), output.pageDepthLayers.size());
    EXPECT_EQ(plans.atlas->GetFrameIdentity(), output.frameIdentity);
    EXPECT_EQ(plans.atlas->GetViewIdentity(), output.viewIdentity);
    EXPECT_EQ(plans.lighting.GetIdentityHash(), output.lightingPlanIdentity);
    EXPECT_EQ(plans.atlas->GetIdentityHash(), output.atlasPlanIdentity);
    EXPECT_EQ(plans.atlas->GetAtlasGeneration(), output.atlasGeneration);
    EXPECT_EQ(7U, graph.GetRecordedPassCount());

    const RVX::RHITextureDesc* const descriptor =
        graph.GetTextureDesc(output.depthAtlas);
    ASSERT_NE(nullptr, descriptor);
    EXPECT_EQ(RVX::RHIFormat::D32_FLOAT, descriptor->format);
    EXPECT_EQ(8U, descriptor->arraySize);
    EXPECT_TRUE(RVX::HasFlag(
        descriptor->usage, RVX::RHITextureUsage::DepthStencil));
    EXPECT_TRUE(RVX::HasFlag(
        descriptor->usage, RVX::RHITextureUsage::ShaderResource));
    for (RVX::uint32 pageIndex = 0;
         pageIndex < static_cast<RVX::uint32>(output.pageDepthLayers.size());
         ++pageIndex)
    {
        const RVX::RGTextureHandle& layer = output.pageDepthLayers[pageIndex];
        EXPECT_TRUE(layer.IsValid());
        EXPECT_TRUE(layer.hasSubresourceRange);
        EXPECT_EQ(pageIndex, layer.subresourceRange.baseArrayLayer);
        EXPECT_EQ(1U, layer.subresourceRange.arrayLayerCount);
        EXPECT_EQ(RVX::RHITextureAspect::Depth, layer.subresourceRange.aspect);
    }

    const RVX::LocalShadowPassExecutionEvidence pending =
        pass.GetLastExecutionEvidence();
    EXPECT_TRUE(pending.cpuPlanReady);
    EXPECT_FALSE(pending.gpuResourceCreated);
    EXPECT_FALSE(pending.gpuCommandsRecorded);
    EXPECT_FALSE(pending.gpuExecuted);

    pass.FinalizeGraphExecution(false);
    const RVX::LocalShadowPassExecutionEvidence rejected =
        pass.GetLastExecutionEvidence();
    EXPECT_FALSE(rejected.gpuResourceCreated);
    EXPECT_FALSE(rejected.gpuCommandsRecorded);
    EXPECT_FALSE(rejected.gpuExecuted);
    EXPECT_FALSE(rejected.HasExactGpuExecution());
}

TEST(LocalShadowPassValidation,
     RejectsStaleIdentityOrGenerationWithoutMutatingTheGraph)
{
    const Plans plans = MakePlans();
    ASSERT_NE(nullptr, plans.atlas);

    RVX::RenderGraph graph;
    RVX::LocalShadowPass pass;
    RVX::LocalShadowPassRequest request;
    request.atlasPlan = plans.atlas;
    request.expectedAtlasPlanIdentity = plans.atlas->GetIdentityHash() + 1U;
    EXPECT_EQ(RVX::LocalShadowPassCode::StaleAtlasPlan,
              pass.AddToGraph(graph, request).code);
    EXPECT_EQ(0U, graph.GetRecordedPassCount());

    request.expectedAtlasPlanIdentity = plans.atlas->GetIdentityHash();
    request.expectedAtlasGeneration = plans.atlas->GetAtlasGeneration() + 1U;
    EXPECT_EQ(RVX::LocalShadowPassCode::StaleAtlasPlan,
              pass.AddToGraph(graph, request).code);
    EXPECT_EQ(0U, graph.GetRecordedPassCount());
}

TEST(LocalShadowPassValidation,
     CasterRecorderIsExplicitAndDoesNotImplyExecutionAtRegistrationTime)
{
    const Plans plans = MakePlans();
    RVX::RenderGraph graph;
    RVX::LocalShadowPass pass;
    RVX::LocalShadowPassRequest request;
    request.atlasPlan = plans.atlas;
    RVX::uint32 callbackCount = 0;
    request.recordCasters = [&callbackCount](
        RVX::RHICommandContext&,
        const RVX::LocalShadowAtlasPage&,
        RVX::uint32& outDrawCount,
        std::string&)
    {
        ++callbackCount;
        outDrawCount = 3;
        return true;
    };
    ASSERT_TRUE(pass.AddToGraph(graph, request).IsValid());
    EXPECT_EQ(0U, callbackCount)
        << "Caster recording must only occur inside RenderGraph execution";
    const RVX::LocalShadowPassExecutionEvidence evidence =
        pass.GetLastExecutionEvidence();
    EXPECT_FALSE(evidence.gpuExecuted);
    EXPECT_EQ(0U, evidence.casterDrawCount);
}
