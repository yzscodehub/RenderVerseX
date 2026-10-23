#include "Render/Lighting/LocalShadowAtlas.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <vector>

namespace
{
    struct ShadowPlans
    {
        RVX::LightingFramePlan lighting{};
        RVX::LocalShadowFramePlan localShadow{};
    };

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
            static_cast<float>(lightId) * 0.1f, 2.0f, -3.0f);
        light.direction = type == RVX::RenderLightType::Spot
            ? RVX::Vec3(0.0f, -1.0f, -1.0f)
            : RVX::Vec3(0.0f);
        light.color = RVX::Vec3(1.0f, 0.75f, 0.5f);
        light.normalizedIntensity = 12.0f;
        light.range = type == RVX::RenderLightType::Point ? 12.0f : 20.0f;
        light.innerConeRadians = type == RVX::RenderLightType::Spot
            ? 0.2f : 0.0f;
        light.outerConeRadians = type == RVX::RenderLightType::Spot
            ? 0.4f : 0.0f;
        light.castsShadows = castsShadows;
        return light;
    }

    RVX::LightingFramePlan BuildLightingPlan(
        std::vector<RVX::LightingLightCandidate> candidates)
    {
        RVX::LightingFramePlanRequest request;
        request.frameIdentity = {
            .frameSequence = 211,
            .sceneRevision = 223,
            .surfaceGeneration = 227,
            .configurationRevision = 229};
        request.viewIdentity = {
            .sceneRuntimeId = 233,
            .cameraEntityPacked = 239,
            .subViewIndex = 0,
            .role = RVX::ViewRole::Main};
        request.sceneLightStateHash = 241;
        request.lightingConfigurationRevision = 251;
        request.qualityRevision = 257;
        request.quality = RVX::RenderQualityTier::High;
        request.pointLightCapacity = 16;
        request.spotLightCapacity = 16;
        request.allowLocalLightTruncation = false;
        request.ibl.normalizedIntensity = 0.0f;
        request.lightCandidates = std::move(candidates);

        const RVX::LightingFramePlanBuildResult result =
            RVX::LightingFramePlanBuilder{}.Build(request);
        EXPECT_TRUE(result.Succeeded()) << result.diagnostic;
        EXPECT_TRUE(result.plan.IsValid());
        return result.plan;
    }

    RVX::LocalShadowFramePlan BuildLocalShadowPlan(
        const RVX::LightingFramePlan& lightingPlan,
        RVX::uint32 pageResolution = 256,
        RVX::uint32 pageCapacity = 16,
        RVX::uint32 pointCapacity = 1,
        RVX::uint32 spotCapacity = 1,
        RVX::uint64 atlasGeneration = 17)
    {
        RVX::LocalShadowFramePlanRequest request;
        request.atlas = {
            .pageResolution = pageResolution,
            .pageCapacity = pageCapacity,
            .pointLightCapacity = pointCapacity,
            .spotLightCapacity = spotCapacity,
            .overflowPolicy = RVX::LocalShadowOverflowPolicy::Fail};
        request.atlasGeneration = atlasGeneration;
        const RVX::LocalShadowFramePlanBuildResult result =
            RVX::LocalShadowFramePlanBuilder{}.Build(lightingPlan, request);
        EXPECT_TRUE(result.Succeeded()) << result.diagnostic;
        EXPECT_TRUE(result.plan.IsValid());
        return result.plan;
    }

    ShadowPlans BuildRepresentativePlans()
    {
        ShadowPlans plans;
        plans.lighting = BuildLightingPlan({
            MakeLocalLight(40, RVX::RenderLightType::Point, 9, true),
            MakeLocalLight(20, RVX::RenderLightType::Spot, 10, true),
            MakeLocalLight(10, RVX::RenderLightType::Spot, 20, false),
        });
        plans.localShadow = BuildLocalShadowPlan(plans.lighting);
        return plans;
    }

    RVX::LocalShadowAtlasPlanRequest MakeAtlasRequest(
        RVX::uint64 atlasGeneration = 17)
    {
        RVX::LocalShadowAtlasPlanRequest request;
        request.atlasGeneration = atlasGeneration;
        request.nearPlane = 0.1f;
        return request;
    }

    float EvaluateDeviceDepth(
        const RVX::LocalShadowAtlasPage& page,
        float distance)
    {
        const RVX::Vec4 clip = page.viewProjection * RVX::Vec4(
            page.lightPosition + page.forward * distance, 1.0f);
        return clip.z / clip.w;
    }
} // namespace

TEST(LocalShadowAtlasValidation,
     PublishesFullCapacityD32Texture2DArrayAndExactLayerViews)
{
    const ShadowPlans plans = BuildRepresentativePlans();
    const RVX::LocalShadowAtlasPlanBuildResult result =
        RVX::LocalShadowAtlasPlanBuilder{}.Build(
            plans.lighting, plans.localShadow, MakeAtlasRequest());
    ASSERT_TRUE(result.Succeeded()) << result.diagnostic;
    ASSERT_TRUE(result.plan.IsValid());

    const RVX::LocalShadowAtlasTextureContract& texture =
        result.plan.GetTextureContract();
    ASSERT_TRUE(texture.textureRequired);
    ASSERT_NE(nullptr, texture.GetTextureDesc());
    const RVX::RHITextureDesc& descriptor = *texture.GetTextureDesc();
    EXPECT_EQ(256U, descriptor.width);
    EXPECT_EQ(256U, descriptor.height);
    EXPECT_EQ(1U, descriptor.depth);
    EXPECT_EQ(1U, descriptor.mipLevels);
    EXPECT_EQ(16U, descriptor.arraySize);
    EXPECT_EQ(RVX::RHITextureDimension::Texture2D, descriptor.dimension);
    EXPECT_EQ(RVX::RHIFormat::D32_FLOAT, descriptor.format);
    EXPECT_EQ(RVX::RHISampleCount::Count1, descriptor.sampleCount);
    EXPECT_TRUE(RVX::HasFlag(
        descriptor.usage, RVX::RHITextureUsage::DepthStencil));
    EXPECT_TRUE(RVX::HasFlag(
        descriptor.usage, RVX::RHITextureUsage::ShaderResource));
    EXPECT_EQ(RVX::RHIOptimizedClearValueType::DepthStencil,
              descriptor.optimizedClearValue.type);
    EXPECT_FLOAT_EQ(RVX::RVX_LOCAL_SHADOW_CLEAR_DEPTH,
                    descriptor.optimizedClearValue.depthStencil.depth);
    EXPECT_EQ(RVX::RHICompareOp::LessEqual,
              RVX::RVX_LOCAL_SHADOW_DEPTH_COMPARE);
    EXPECT_EQ(16U, RVX::GetTexturePhysicalLayerCount(descriptor));
    EXPECT_EQ(4194304U, texture.estimatedAllocationBytes);

    const auto depthView = texture.MakeDepthViewDesc(15);
    ASSERT_TRUE(depthView.has_value());
    EXPECT_EQ(RVX::RHITextureViewType::DepthStencil, depthView->type);
    EXPECT_EQ(15U, depthView->subresourceRange.baseArrayLayer);
    EXPECT_EQ(1U, depthView->subresourceRange.arrayLayerCount);
    EXPECT_EQ(RVX::RHITextureAspect::Depth,
              depthView->subresourceRange.aspect);
    EXPECT_TRUE(RVX::IsTextureViewTypeCompatible(
        descriptor.usage, descriptor.format, *depthView));
    EXPECT_FALSE(texture.MakeDepthViewDesc(16).has_value());

    const auto shaderView = texture.MakeShaderResourceViewDesc();
    ASSERT_TRUE(shaderView.has_value());
    EXPECT_EQ(RVX::RHITextureViewType::ShaderResource, shaderView->type);
    EXPECT_EQ(0U, shaderView->subresourceRange.baseArrayLayer);
    EXPECT_EQ(16U, shaderView->subresourceRange.arrayLayerCount);

    const RVX::LocalShadowAtlasExecutionEvidence& evidence =
        result.plan.GetExecutionEvidence();
    EXPECT_EQ(2U, evidence.allocationCount);
    EXPECT_EQ(7U, evidence.pageCount);
    EXPECT_EQ(16U, evidence.pageCapacity);
    EXPECT_TRUE(evidence.cpuPlanReady);
    EXPECT_FALSE(evidence.gpuResourceCreated);
    EXPECT_FALSE(evidence.gpuCommandsRecorded);
    EXPECT_FALSE(evidence.gpuExecuted);
}

TEST(LocalShadowAtlasValidation,
     PointPagesUseFixedSixFaceOrderAndRightHandedZeroToOneDepth)
{
    const ShadowPlans plans = BuildRepresentativePlans();
    const RVX::LocalShadowAtlasPlanBuildResult result =
        RVX::LocalShadowAtlasPlanBuilder{}.Build(
            plans.lighting, plans.localShadow, MakeAtlasRequest());
    ASSERT_TRUE(result.Succeeded()) << result.diagnostic;

    const RVX::LocalShadowAllocation* const pointAllocation =
        plans.localShadow.FindAllocation(40);
    ASSERT_NE(nullptr, pointAllocation);
    ASSERT_EQ(1U, pointAllocation->firstPage);
    const std::array<RVX::Vec3, 6> expectedForward = {
        RVX::Vec3(1.0f, 0.0f, 0.0f),
        RVX::Vec3(-1.0f, 0.0f, 0.0f),
        RVX::Vec3(0.0f, 1.0f, 0.0f),
        RVX::Vec3(0.0f, -1.0f, 0.0f),
        RVX::Vec3(0.0f, 0.0f, 1.0f),
        RVX::Vec3(0.0f, 0.0f, -1.0f),
    };

    for (RVX::uint32 face = 0;
         face < RVX::RVX_LOCAL_SHADOW_POINT_PAGE_COUNT;
         ++face)
    {
        const RVX::LocalShadowAtlasPage* const page =
            result.plan.ResolvePage(
                pointAllocation->handle, face,
                result.plan.GetIdentityHash(),
                result.plan.GetAtlasGeneration());
        ASSERT_NE(nullptr, page);
        EXPECT_EQ(pointAllocation->firstPage + face, page->pageIndex);
        EXPECT_EQ(static_cast<RVX::LocalShadowPageProjection>(face),
                  page->projection);
        EXPECT_NEAR(0.0f,
                    RVX::length(page->forward - expectedForward[face]),
                    1.0e-6f);
        EXPECT_NEAR(0.0f,
                    EvaluateDeviceDepth(*page, page->nearPlane),
                    2.0e-4f);
        EXPECT_NEAR(1.0f,
                    EvaluateDeviceDepth(*page, page->farPlane),
                    2.0e-4f);

        const RVX::Vec4 origin =
            page->view * RVX::Vec4(page->lightPosition, 1.0f);
        const RVX::Vec4 forward = page->view * RVX::Vec4(
            page->lightPosition + page->forward, 1.0f);
        EXPECT_NEAR(0.0f, RVX::length(RVX::Vec3(origin)), 1.0e-4f);
        EXPECT_LT(forward.z, 0.0f);
    }
}

TEST(LocalShadowAtlasValidation,
     SpotPageUsesCanonicalBasisConeAndTheSameDepthContract)
{
    const ShadowPlans plans = BuildRepresentativePlans();
    const RVX::LocalShadowAtlasPlanBuildResult result =
        RVX::LocalShadowAtlasPlanBuilder{}.Build(
            plans.lighting, plans.localShadow, MakeAtlasRequest());
    ASSERT_TRUE(result.Succeeded()) << result.diagnostic;

    const RVX::LocalShadowAllocation* const spotAllocation =
        plans.localShadow.FindAllocation(20);
    ASSERT_NE(nullptr, spotAllocation);
    const RVX::LocalShadowAtlasPage* const page = result.plan.ResolvePage(
        spotAllocation->handle, 0, result.plan.GetIdentityHash(),
        result.plan.GetAtlasGeneration());
    ASSERT_NE(nullptr, page);
    EXPECT_EQ(RVX::LocalShadowPageProjection::Spot, page->projection);
    EXPECT_NEAR(0.8f, page->fieldOfViewRadians, 1.0e-6f);
    EXPECT_NEAR(1.0f, RVX::length(page->forward), 1.0e-5f);
    EXPECT_NEAR(1.0f, RVX::length(page->up), 1.0e-5f);
    EXPECT_NEAR(0.0f, RVX::dot(page->forward, page->up), 1.0e-5f);
    EXPECT_NEAR(0.0f,
                EvaluateDeviceDepth(*page, page->nearPlane), 2.0e-4f);
    EXPECT_NEAR(1.0f,
                EvaluateDeviceDepth(*page, page->farPlane), 2.0e-4f);
    EXPECT_EQ(RVX::LocalShadowCoordinateSystem::RightHandedYUp,
              result.plan.GetCoordinateSystem());
    EXPECT_EQ(RVX::LocalShadowDepthConvention::ForwardZeroToOne,
              result.plan.GetDepthConvention());
}

TEST(LocalShadowAtlasValidation,
     ReorderedSourceCandidatesProduceTheSameAtlasAndMatrixIdentity)
{
    std::vector<RVX::LightingLightCandidate> candidates = {
        MakeLocalLight(40, RVX::RenderLightType::Point, 9, true),
        MakeLocalLight(20, RVX::RenderLightType::Spot, 10, true),
        MakeLocalLight(10, RVX::RenderLightType::Spot, 20, false),
    };
    const RVX::LightingFramePlan firstLighting =
        BuildLightingPlan(candidates);
    std::reverse(candidates.begin(), candidates.end());
    const RVX::LightingFramePlan reorderedLighting =
        BuildLightingPlan(candidates);
    const RVX::LocalShadowFramePlan firstLocal =
        BuildLocalShadowPlan(firstLighting);
    const RVX::LocalShadowFramePlan reorderedLocal =
        BuildLocalShadowPlan(reorderedLighting);
    ASSERT_EQ(firstLighting.GetIdentityHash(),
              reorderedLighting.GetIdentityHash());
    ASSERT_EQ(firstLocal.GetIdentityHash(), reorderedLocal.GetIdentityHash());

    const RVX::LocalShadowAtlasPlanBuildResult first =
        RVX::LocalShadowAtlasPlanBuilder{}.Build(
            firstLighting, firstLocal, MakeAtlasRequest());
    const RVX::LocalShadowAtlasPlanBuildResult reordered =
        RVX::LocalShadowAtlasPlanBuilder{}.Build(
            reorderedLighting, reorderedLocal, MakeAtlasRequest());
    ASSERT_TRUE(first.Succeeded()) << first.diagnostic;
    ASSERT_TRUE(reordered.Succeeded()) << reordered.diagnostic;
    EXPECT_EQ(first.plan.GetIdentityHash(), reordered.plan.GetIdentityHash());

    RVX::LocalShadowAtlasPlanRequest pinned = MakeAtlasRequest();
    pinned.expectedIdentityHash = first.plan.GetIdentityHash();
    EXPECT_TRUE(RVX::LocalShadowAtlasPlanBuilder{}
                    .Build(firstLighting, firstLocal, pinned)
                    .Succeeded());
    pinned.nearPlane = 0.2f;
    EXPECT_EQ(RVX::LocalShadowAtlasPlanCode::IdentityMismatch,
              RVX::LocalShadowAtlasPlanBuilder{}
                  .Build(firstLighting, firstLocal, pinned)
                  .code);
}

TEST(LocalShadowAtlasValidation,
     GenerationPlanIdentityAndPageOffsetRejectStaleResolution)
{
    const ShadowPlans plans = BuildRepresentativePlans();
    RVX::LocalShadowAtlasPlanRequest request = MakeAtlasRequest();
    const RVX::LocalShadowAtlasPlanBuildResult result =
        RVX::LocalShadowAtlasPlanBuilder{}.Build(
            plans.lighting, plans.localShadow, request);
    ASSERT_TRUE(result.Succeeded()) << result.diagnostic;

    request.atlasGeneration++;
    EXPECT_EQ(RVX::LocalShadowAtlasPlanCode::AtlasGenerationMismatch,
              RVX::LocalShadowAtlasPlanBuilder{}
                  .Build(plans.lighting, plans.localShadow, request)
                  .code);
    request = MakeAtlasRequest();
    request.expectedLocalShadowPlanIdentity =
        plans.localShadow.GetIdentityHash() + 1;
    EXPECT_EQ(
        RVX::LocalShadowAtlasPlanCode::LocalShadowPlanIdentityMismatch,
        RVX::LocalShadowAtlasPlanBuilder{}
            .Build(plans.lighting, plans.localShadow, request)
            .code);

    const RVX::LocalShadowAllocation* const pointAllocation =
        plans.localShadow.FindAllocation(40);
    ASSERT_NE(nullptr, pointAllocation);
    EXPECT_EQ(nullptr,
              result.plan.ResolvePage(
                  pointAllocation->handle, 0,
                  result.plan.GetIdentityHash() + 1,
                  result.plan.GetAtlasGeneration()));
    EXPECT_EQ(nullptr,
              result.plan.ResolvePage(
                  pointAllocation->handle, 0,
                  result.plan.GetIdentityHash(),
                  result.plan.GetAtlasGeneration() + 1));
    EXPECT_EQ(nullptr,
              result.plan.ResolvePage(
                  pointAllocation->handle,
                  RVX::RVX_LOCAL_SHADOW_POINT_PAGE_COUNT,
                  result.plan.GetIdentityHash(),
                  result.plan.GetAtlasGeneration()));
}

TEST(LocalShadowAtlasValidation,
     RejectsInvalidProjectionInputsUnsupportedConeAndDimensionOverflow)
{
    const ShadowPlans plans = BuildRepresentativePlans();
    RVX::LocalShadowAtlasPlanRequest request = MakeAtlasRequest();
    request.schemaVersion++;
    EXPECT_EQ(RVX::LocalShadowAtlasPlanCode::InvalidSchema,
              RVX::LocalShadowAtlasPlanBuilder{}
                  .Build(plans.lighting, plans.localShadow, request)
                  .code);

    request = MakeAtlasRequest();
    request.nearPlane = 0.0f;
    EXPECT_EQ(
        RVX::LocalShadowAtlasPlanCode::InvalidProjectionConfiguration,
        RVX::LocalShadowAtlasPlanBuilder{}
            .Build(plans.lighting, plans.localShadow, request)
            .code);
    request.nearPlane = 21.0f;
    EXPECT_EQ(RVX::LocalShadowAtlasPlanCode::InvalidClipRange,
              RVX::LocalShadowAtlasPlanBuilder{}
                  .Build(plans.lighting, plans.localShadow, request)
                  .code);

    RVX::LightingLightCandidate wideSpot =
        MakeLocalLight(20, RVX::RenderLightType::Spot, 10, true);
    wideSpot.innerConeRadians = 1.0f;
    wideSpot.outerConeRadians = 2.0f;
    const RVX::LightingFramePlan wideLighting =
        BuildLightingPlan({wideSpot});
    const RVX::LocalShadowFramePlan wideLocal =
        BuildLocalShadowPlan(wideLighting, 256, 1, 1, 1);
    EXPECT_EQ(RVX::LocalShadowAtlasPlanCode::UnsupportedSpotCone,
              RVX::LocalShadowAtlasPlanBuilder{}
                  .Build(wideLighting, wideLocal, MakeAtlasRequest())
                  .code);

    const RVX::LocalShadowFramePlan hugeLocal = BuildLocalShadowPlan(
        plans.lighting,
        std::numeric_limits<RVX::uint32>::max(),
        std::numeric_limits<RVX::uint32>::max(),
        1,
        1);
    EXPECT_EQ(RVX::LocalShadowAtlasPlanCode::ArithmeticOverflow,
              RVX::LocalShadowAtlasPlanBuilder{}
                  .Build(plans.lighting, hugeLocal, MakeAtlasRequest())
                  .code);
}

TEST(LocalShadowAtlasValidation,
     ZeroWorkPlanHasNoTextureAndNeverClaimsGpuExecution)
{
    const RVX::LightingFramePlan lighting = BuildLightingPlan({
        MakeLocalLight(1, RVX::RenderLightType::Point, 1, false),
        MakeLocalLight(2, RVX::RenderLightType::Spot, 1, false),
    });
    const RVX::LocalShadowFramePlan local =
        BuildLocalShadowPlan(lighting, 256, 0, 0, 0);
    const RVX::LocalShadowAtlasPlanBuildResult result =
        RVX::LocalShadowAtlasPlanBuilder{}.Build(
            lighting, local, MakeAtlasRequest());
    ASSERT_TRUE(result.Succeeded()) << result.diagnostic;
    EXPECT_TRUE(result.plan.IsValid());
    EXPECT_TRUE(result.plan.GetPages().empty());
    EXPECT_FALSE(result.plan.GetTextureContract().textureRequired);
    EXPECT_EQ(nullptr, result.plan.GetTextureContract().GetTextureDesc());
    EXPECT_FALSE(result.plan.GetTextureContract()
                     .MakeShaderResourceViewDesc()
                     .has_value());

    const RVX::LocalShadowAtlasExecutionEvidence& evidence =
        result.plan.GetExecutionEvidence();
    EXPECT_TRUE(evidence.cpuPlanReady);
    EXPECT_FALSE(evidence.gpuResourceCreated);
    EXPECT_FALSE(evidence.gpuCommandsRecorded);
    EXPECT_FALSE(evidence.gpuExecuted);
}
