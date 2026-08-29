#include "Render/PostProcess/SSR.h"
#include "Render/Reflection/ReflectionFramePlan.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <limits>
#include <vector>

namespace
{
    constexpr RVX::RenderFrameIdentity FRAME_IDENTITY{
        .frameSequence = 11,
        .sceneRevision = 13,
        .surfaceGeneration = 17,
        .configurationRevision = 19};
    constexpr RVX::RenderViewIdentity VIEW_IDENTITY{
        .sceneRuntimeId = 23,
        .cameraEntityPacked = 29,
        .subViewIndex = 0,
        .role = RVX::ViewRole::Main};
    constexpr RVX::RenderOutputExtent OUTPUT_EXTENT{
        .width = 640,
        .height = 360};

    void ExpectVec3Near(const RVX::Vec3& actual,
                        const RVX::Vec3& expected,
                        float tolerance = 1.0e-6f)
    {
        EXPECT_NEAR(actual.x, expected.x, tolerance);
        EXPECT_NEAR(actual.y, expected.y, tolerance);
        EXPECT_NEAR(actual.z, expected.z, tolerance);
    }

    struct ReferenceHiZLevel
    {
        RVX::uint32 width = 0;
        RVX::uint32 height = 0;
        std::vector<float> values;
    };

    std::vector<ReferenceHiZLevel> BuildReferenceMinPyramid(
        RVX::uint32 width,
        RVX::uint32 height,
        std::vector<float> base)
    {
        std::vector<ReferenceHiZLevel> levels;
        levels.push_back({width, height, std::move(base)});
        while (width > 1U || height > 1U)
        {
            const ReferenceHiZLevel& source = levels.back();
            const RVX::uint32 destinationWidth = std::max(1U, width / 2U);
            const RVX::uint32 destinationHeight = std::max(1U, height / 2U);
            ReferenceHiZLevel destination;
            destination.width = destinationWidth;
            destination.height = destinationHeight;
            destination.values.resize(
                static_cast<size_t>(destinationWidth) * destinationHeight,
                std::numeric_limits<float>::infinity());
            for (RVX::uint32 y = 0; y < destinationHeight; ++y)
            {
                const RVX::uint32 beginY = y * height / destinationHeight;
                const RVX::uint32 endY = std::max(
                    beginY + 1U, (y + 1U) * height / destinationHeight);
                for (RVX::uint32 x = 0; x < destinationWidth; ++x)
                {
                    const RVX::uint32 beginX =
                        x * width / destinationWidth;
                    const RVX::uint32 endX = std::max(
                        beginX + 1U,
                        (x + 1U) * width / destinationWidth);
                    float minimum = std::numeric_limits<float>::infinity();
                    for (RVX::uint32 sourceY = beginY;
                         sourceY < std::min(endY, height);
                         ++sourceY)
                    {
                        for (RVX::uint32 sourceX = beginX;
                             sourceX < std::min(endX, width);
                             ++sourceX)
                        {
                            minimum = std::min(
                                minimum,
                                source.values[
                                    static_cast<size_t>(sourceY) * width +
                                    sourceX]);
                        }
                    }
                    destination.values[
                        static_cast<size_t>(y) * destinationWidth + x] =
                        minimum;
                }
            }
            levels.push_back(std::move(destination));
            width = destinationWidth;
            height = destinationHeight;
        }
        return levels;
    }

    struct ReferenceHiZCellRange
    {
        RVX::uint32 mipPixel = 0;
        RVX::uint32 baseBegin = 0;
        RVX::uint32 baseEnd = 0;
    };

    ReferenceHiZCellRange GetReferenceHiZCellRange(
        RVX::uint32 baseExtent,
        RVX::uint32 mip,
        RVX::uint32 basePixel)
    {
        const RVX::uint32 mipExtent =
            std::max(1U, baseExtent >> mip);
        const RVX::uint32 mipPixel = std::min(
            basePixel >> mip, mipExtent - 1U);
        const RVX::uint32 begin = mipPixel << mip;
        const RVX::uint32 end = mipPixel + 1U < mipExtent
            ? (mipPixel + 1U) << mip
            : baseExtent;
        return {mipPixel, begin, end};
    }

    bool ReferenceHiZIntervalIsStrictlyFront(
        float entryDepth,
        float exitDepth,
        float cellMinimumDepth,
        float thickness)
    {
        return std::max(entryDepth, exitDepth) <
            cellMinimumDepth - thickness;
    }
} // namespace

TEST(HiZReflectionValidation,
     NPOTMinPyramidConsumesEverySourceTexelExactlyOncePerParent)
{
    std::vector<float> base(15U);
    constexpr float columns[5] = {3.0f, 4.0f, -5.0f, 6.0f, 7.0f};
    for (RVX::uint32 y = 0; y < 3U; ++y)
    {
        for (RVX::uint32 x = 0; x < 5U; ++x)
        {
            base[static_cast<size_t>(y) * 5U + x] = columns[x];
        }
    }
    const auto levels = BuildReferenceMinPyramid(5U, 3U, base);
    ASSERT_EQ(levels.size(), 3U);
    ASSERT_EQ(levels[1].width, 2U);
    ASSERT_EQ(levels[1].height, 1U);
    ASSERT_EQ(levels[1].values.size(), 2U);
    EXPECT_FLOAT_EQ(levels[1].values[0], 3.0f);
    EXPECT_FLOAT_EQ(levels[1].values[1], -5.0f);
    ASSERT_EQ(levels[2].values.size(), 1U);
    EXPECT_FLOAT_EQ(levels[2].values[0], -5.0f);
}

TEST(HiZReflectionValidation,
     NPOTHierarchyMatchesOriginalBaseCellRangesAtEveryMip)
{
    constexpr RVX::uint32 Width = 65U;
    constexpr RVX::uint32 Height = 49U;
    std::vector<float> base(static_cast<size_t>(Width) * Height);
    for (RVX::uint32 y = 0; y < Height; ++y)
    {
        for (RVX::uint32 x = 0; x < Width; ++x)
        {
            base[static_cast<size_t>(y) * Width + x] =
                static_cast<float>((x * 131U + y * 977U) % 1009U) -
                static_cast<float>((x + y) % 17U) * 0.25f;
        }
    }
    const auto levels = BuildReferenceMinPyramid(Width, Height, base);
    for (RVX::uint32 mip = 0;
         mip < static_cast<RVX::uint32>(levels.size());
         ++mip)
    {
        const ReferenceHiZLevel& level = levels[mip];
        for (RVX::uint32 y = 0; y < level.height; ++y)
        {
            const ReferenceHiZCellRange yRange =
                GetReferenceHiZCellRange(Height, mip, y << mip);
            for (RVX::uint32 x = 0; x < level.width; ++x)
            {
                const ReferenceHiZCellRange xRange =
                    GetReferenceHiZCellRange(Width, mip, x << mip);
                float expected = std::numeric_limits<float>::infinity();
                for (RVX::uint32 sourceY = yRange.baseBegin;
                     sourceY < yRange.baseEnd;
                     ++sourceY)
                {
                    for (RVX::uint32 sourceX = xRange.baseBegin;
                         sourceX < xRange.baseEnd;
                         ++sourceX)
                    {
                        expected = std::min(
                            expected,
                            base[static_cast<size_t>(sourceY) * Width +
                                 sourceX]);
                    }
                }
                EXPECT_FLOAT_EQ(
                    level.values[static_cast<size_t>(y) * level.width + x],
                    expected)
                    << "mip=" << mip << " cell=(" << x << "," << y
                    << ")";
            }
        }
    }
}

TEST(HiZReflectionValidation,
     NPOTTraversalUsesReductionCellBoundariesRatherThanUniformMipUVs)
{
    const ReferenceHiZCellRange fiveWide =
        GetReferenceHiZCellRange(5U, 1U, 2U);
    EXPECT_EQ(fiveWide.mipPixel, 1U);
    EXPECT_EQ(fiveWide.baseBegin, 2U);
    EXPECT_EQ(fiveWide.baseEnd, 5U);
    EXPECT_FLOAT_EQ(
        static_cast<float>(fiveWide.baseBegin) / 5.0f, 0.4f);
    EXPECT_FLOAT_EQ(
        static_cast<float>(fiveWide.baseEnd) / 5.0f, 1.0f);

    const ReferenceHiZCellRange elevenWide =
        GetReferenceHiZCellRange(11U, 2U, 4U);
    EXPECT_EQ(elevenWide.mipPixel, 1U);
    EXPECT_EQ(elevenWide.baseBegin, 4U);
    EXPECT_EQ(elevenWide.baseEnd, 11U);
    EXPECT_NE(static_cast<float>(elevenWide.baseBegin) / 11.0f, 0.5f);
}

TEST(HiZReflectionValidation,
     FirstMipZeroCellCrossingCreatesABracketAndCannotBeSkipped)
{
    constexpr float EntryDepth = 1.0f;
    constexpr float ExitDepth = 2.0f;
    constexpr float CellMinimumDepth = 1.5f;
    constexpr float Thickness = 0.05f;
    EXPECT_FALSE(ReferenceHiZIntervalIsStrictlyFront(
        EntryDepth, ExitDepth, CellMinimumDepth, Thickness));
    EXPECT_LT(EntryDepth, CellMinimumDepth - Thickness);
    EXPECT_GT(ExitDepth, CellMinimumDepth - Thickness);

    const ReferenceHiZCellRange thinOccluderCell =
        GetReferenceHiZCellRange(5U, 1U, 2U);
    EXPECT_EQ(thinOccluderCell.mipPixel, 1U);
    const std::vector<float> baseDepths =
        {100.0f, 100.0f, 1.0f, 100.0f, 100.0f};
    const float conservativeMinimum = *std::min_element(
        baseDepths.begin() + thinOccluderCell.baseBegin,
        baseDepths.begin() + thinOccluderCell.baseEnd);
    EXPECT_FLOAT_EQ(conservativeMinimum, 1.0f);
    EXPECT_FALSE(ReferenceHiZIntervalIsStrictlyFront(
        0.5f, 2.0f, conservativeMinimum, Thickness));
}

TEST(HiZReflectionValidation,
     DepthPyramidIdentityPinsFrameViewExtentConfigurationAndPipeline)
{
    const RVX::SSRConfig config;
    const RVX::uint64 configurationIdentity =
        RVX::ComputeSSRConfigurationIdentity(config);
    constexpr RVX::uint64 PipelineIdentity = 31;
    const RVX::uint64 baseline = RVX::ComputeSSRDepthPyramidIdentity(
        FRAME_IDENTITY,
        VIEW_IDENTITY,
        OUTPUT_EXTENT,
        configurationIdentity,
        PipelineIdentity);
    ASSERT_NE(baseline, 0U);
    EXPECT_EQ(baseline,
              RVX::ComputeSSRDepthPyramidIdentity(
                  FRAME_IDENTITY,
                  VIEW_IDENTITY,
                  OUTPUT_EXTENT,
                  configurationIdentity,
                  PipelineIdentity));

    RVX::RenderFrameIdentity laterFrame = FRAME_IDENTITY;
    ++laterFrame.frameSequence;
    EXPECT_NE(baseline,
              RVX::ComputeSSRDepthPyramidIdentity(
                  laterFrame,
                  VIEW_IDENTITY,
                  OUTPUT_EXTENT,
                  configurationIdentity,
                  PipelineIdentity));

    RVX::RenderViewIdentity reflectionView = VIEW_IDENTITY;
    reflectionView.role = RVX::ViewRole::Reflection;
    EXPECT_NE(baseline,
              RVX::ComputeSSRDepthPyramidIdentity(
                  FRAME_IDENTITY,
                  reflectionView,
                  OUTPUT_EXTENT,
                  configurationIdentity,
                  PipelineIdentity));

    RVX::RenderOutputExtent resized = OUTPUT_EXTENT;
    resized.width = 1280;
    EXPECT_NE(baseline,
              RVX::ComputeSSRDepthPyramidIdentity(
                  FRAME_IDENTITY,
                  VIEW_IDENTITY,
                  resized,
                  configurationIdentity,
                  PipelineIdentity));
    EXPECT_NE(baseline,
              RVX::ComputeSSRDepthPyramidIdentity(
                  FRAME_IDENTITY,
                  VIEW_IDENTITY,
                  OUTPUT_EXTENT,
                  configurationIdentity + 1U,
                  PipelineIdentity));
    EXPECT_NE(baseline,
              RVX::ComputeSSRDepthPyramidIdentity(
                  FRAME_IDENTITY,
                  VIEW_IDENTITY,
                  OUTPUT_EXTENT,
                  configurationIdentity,
                  PipelineIdentity + 1U));
}

TEST(HiZReflectionValidation, DepthPyramidIdentityRejectsIncompleteInputs)
{
    const RVX::uint64 configurationIdentity =
        RVX::ComputeSSRConfigurationIdentity({});
    constexpr RVX::uint64 PipelineIdentity = 37;

    EXPECT_EQ(0U,
              RVX::ComputeSSRDepthPyramidIdentity(
                  {},
                  VIEW_IDENTITY,
                  OUTPUT_EXTENT,
                  configurationIdentity,
                  PipelineIdentity));
    EXPECT_EQ(0U,
              RVX::ComputeSSRDepthPyramidIdentity(
                  FRAME_IDENTITY,
                  {},
                  OUTPUT_EXTENT,
                  configurationIdentity,
                  PipelineIdentity));
    EXPECT_EQ(0U,
              RVX::ComputeSSRDepthPyramidIdentity(
                  FRAME_IDENTITY,
                  VIEW_IDENTITY,
                  {},
                  configurationIdentity,
                  PipelineIdentity));
    EXPECT_EQ(0U,
              RVX::ComputeSSRDepthPyramidIdentity(
                  FRAME_IDENTITY,
                  VIEW_IDENTITY,
                  OUTPUT_EXTENT,
                  0,
                  PipelineIdentity));
    EXPECT_EQ(0U,
              RVX::ComputeSSRDepthPyramidIdentity(
                  FRAME_IDENTITY,
                  VIEW_IDENTITY,
                  OUTPUT_EXTENT,
                  configurationIdentity,
                  0));
}

TEST(HiZReflectionValidation,
     SSRConfidenceConsumesFirstThenProbeAndIBLConsumeTheResidual)
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
    ExpectVec3Near(result.radiance, RVX::Vec3(0.25f, 0.375f, 0.375f));
}

TEST(HiZReflectionValidation,
     ScreenSpaceMissFallsThroughProbeThenIBLWithoutBlackOutput)
{
    RVX::ReflectionRouteSample sample;
    sample.probeRadiance = RVX::Vec3(0.8f, 0.4f, 0.2f);
    sample.probeConfidence = 0.75f;
    sample.probeValid = true;
    sample.iblRadiance = RVX::Vec3(0.1f, 0.2f, 0.4f);
    sample.iblValid = true;

    const RVX::ReflectionRouteResolution probeFallback =
        RVX::ResolveReflectionRoute(sample);
    ASSERT_TRUE(probeFallback.Succeeded()) << probeFallback.diagnostic;
    EXPECT_EQ(RVX::ReflectionResolutionRoute::ProbeAndIBL,
              probeFallback.route);
    EXPECT_FLOAT_EQ(0.0f, probeFallback.ssrWeight);
    EXPECT_FLOAT_EQ(0.75f, probeFallback.probeWeight);
    EXPECT_FLOAT_EQ(0.25f, probeFallback.iblWeight);
    ExpectVec3Near(probeFallback.radiance,
                   RVX::Vec3(0.625f, 0.35f, 0.25f));

    sample.probeRadiance = RVX::Vec3(0.0f);
    sample.probeConfidence = 0.0f;
    sample.probeValid = false;
    const RVX::ReflectionRouteResolution iblFallback =
        RVX::ResolveReflectionRoute(sample);
    ASSERT_TRUE(iblFallback.Succeeded()) << iblFallback.diagnostic;
    EXPECT_EQ(RVX::ReflectionResolutionRoute::IBL, iblFallback.route);
    EXPECT_FLOAT_EQ(1.0f, iblFallback.iblWeight);
    ExpectVec3Near(iblFallback.radiance, sample.iblRadiance);
}

TEST(HiZReflectionValidation,
     ResolveRejectsMissingIBLAndConfidenceWithoutMatchingValidity)
{
    RVX::ReflectionRouteSample missingIBL;
    EXPECT_FALSE(RVX::ResolveReflectionRoute(missingIBL).Succeeded());

    RVX::ReflectionRouteSample staleSSR;
    staleSSR.ssrConfidence = 0.5f;
    staleSSR.ssrHitValid = false;
    staleSSR.iblRadiance = RVX::Vec3(0.1f);
    staleSSR.iblValid = true;
    EXPECT_FALSE(RVX::ResolveReflectionRoute(staleSSR).Succeeded());

    RVX::ReflectionRouteSample staleProbe;
    staleProbe.probeConfidence = 0.5f;
    staleProbe.probeValid = false;
    staleProbe.iblRadiance = RVX::Vec3(0.1f);
    staleProbe.iblValid = true;
    EXPECT_FALSE(RVX::ResolveReflectionRoute(staleProbe).Succeeded());
}

TEST(HiZReflectionValidation,
     MaterializedGlobalFallbackPreservesTheExactPreExposedScene)
{
    RVX::MaterializedReflectionRouteSample sample;
    sample.sceneColor = RVX::Vec3(10.0f, 8.0f, 6.0f);
    sample.baseIndirectSpecular = RVX::Vec3(2.0f, 1.0f, 0.5f);
    sample.specularResponse = RVX::Vec3(0.5f, 0.25f, 1.0f);
    sample.producerPreExposure = 8.0f;
    sample.surfaceInputsValid = true;

    const RVX::MaterializedReflectionRouteResolution result =
        RVX::ResolveMaterializedReflectionRoute(sample);
    ASSERT_TRUE(result.Succeeded()) << result.diagnostic;
    EXPECT_EQ(result.route, RVX::ReflectionResolutionRoute::IBL);
    ExpectVec3Near(result.routedIndirectSpecular,
                   sample.baseIndirectSpecular);
    ExpectVec3Near(result.resolvedSceneColor, sample.sceneColor);
}

TEST(HiZReflectionValidation,
     MaterializedProbeAndSSRUseReceiverResponseAndProducerExposureOnce)
{
    RVX::MaterializedReflectionRouteSample sample;
    sample.sceneColor = RVX::Vec3(10.0f, 8.0f, 6.0f);
    sample.baseIndirectSpecular = RVX::Vec3(2.0f, 1.0f, 0.5f);
    sample.specularResponse = RVX::Vec3(0.5f, 0.25f, 1.0f);
    sample.producerPreExposure = 2.0f;
    sample.surfaceInputsValid = true;
    sample.localProbeRadiance = RVX::Vec3(4.0f, 4.0f, 1.0f);
    sample.localProbeWeight = 0.5f;
    sample.localProbeValid = true;
    sample.ssrSceneRadiance = RVX::Vec3(6.0f, 2.0f, 1.0f);
    sample.ssrConfidence = 0.5f;
    sample.ssrHitValid = true;

    const RVX::MaterializedReflectionRouteResolution result =
        RVX::ResolveMaterializedReflectionRoute(sample);
    ASSERT_TRUE(result.Succeeded()) << result.diagnostic;
    EXPECT_EQ(result.route,
              RVX::ReflectionResolutionRoute::SSRProbeAndIBL);
    ExpectVec3Near(result.routedIndirectSpecular,
                   RVX::Vec3(3.0f, 1.0f, 1.125f));
    ExpectVec3Near(result.resolvedSceneColor,
                   RVX::Vec3(11.0f, 8.0f, 6.625f));

    sample.producerPreExposure = 4.0f;
    const RVX::MaterializedReflectionRouteResolution brighterProbe =
        RVX::ResolveMaterializedReflectionRoute(sample);
    ASSERT_TRUE(brighterProbe.Succeeded()) << brighterProbe.diagnostic;
    EXPECT_GT(brighterProbe.routedIndirectSpecular.x,
              result.routedIndirectSpecular.x);
    EXPECT_GT(brighterProbe.routedIndirectSpecular.y,
              result.routedIndirectSpecular.y);
}

TEST(HiZReflectionValidation,
     InvalidOrUnlitReceiverCannotMutateTheSceneOrConsumeRoutes)
{
    RVX::MaterializedReflectionRouteSample unlit;
    unlit.sceneColor = RVX::Vec3(3.0f, 2.0f, 1.0f);
    unlit.baseIndirectSpecular = RVX::Vec3(0.0f);
    unlit.specularResponse = RVX::Vec3(0.0f);
    unlit.surfaceInputsValid = false;
    const RVX::MaterializedReflectionRouteResolution preserved =
        RVX::ResolveMaterializedReflectionRoute(unlit);
    ASSERT_TRUE(preserved.Succeeded()) << preserved.diagnostic;
    ExpectVec3Near(preserved.resolvedSceneColor, unlit.sceneColor);

    unlit.ssrConfidence = 0.5f;
    unlit.ssrHitValid = true;
    EXPECT_FALSE(
        RVX::ResolveMaterializedReflectionRoute(unlit).Succeeded());

    RVX::MaterializedReflectionRouteSample invalid;
    invalid.sceneColor = RVX::Vec3(1.0f);
    invalid.baseIndirectSpecular = RVX::Vec3(0.5f);
    invalid.specularResponse = RVX::Vec3(
        std::numeric_limits<float>::quiet_NaN());
    invalid.surfaceInputsValid = true;
    EXPECT_FALSE(
        RVX::ResolveMaterializedReflectionRoute(invalid).Succeeded());
}

TEST(HiZReflectionValidation,
     QualificationRouteStatsBindRadianceSchemaToObservedFallbackClasses)
{
    RVX::SSRQualificationProbeStats stats;
    stats.requested = true;
    stats.traceDrawRecorded = true;
    stats.resolveDrawRecorded = true;
    stats.copyRecorded = true;
    stats.submissionAccepted = true;
    stats.presented = true;
    stats.gpuCompleted = true;
    stats.ready = true;
    stats.resolutionAware = true;
    stats.routeMaskRadianceSchemaVersion =
        RVX::RVX_SSR_QUALIFICATION_ROUTE_MASK_RADIANCE_SCHEMA_VERSION;
    stats.localProbeRadianceQualified = true;
    stats.globalIBLRadianceQualified = false;
    stats.identity.frameIdentity = FRAME_IDENTITY;
    stats.identity.viewIdentity = VIEW_IDENTITY;
    stats.identity.extent = OUTPUT_EXTENT;
    stats.identity.reflectionPlanIdentity = 31u;
    stats.identity.lightingPlanIdentity = 37u;
    stats.identity.depthPyramidIdentity = 41u;
    stats.identity.configurationIdentity = 43u;
    stats.identity.reflectionSurfaceIdentity = 47u;
    stats.identity.exposureStateContentHash = 53u;
    stats.sampleCount = 4u;
    stats.receiverSampleCount = 2u;
    stats.backgroundPixelCount = 2u;
    stats.screenSpaceHitPixelCount = 1u;
    stats.localProbePixelCount = 1u;

    // A generic route-mask probe remains valid with only a local route. The
    // GFX19 sample qualification, rather than this reusable stats contract,
    // is responsible for requiring both fallback classes.
    EXPECT_TRUE(stats.IsValid());

    stats.globalIBLRadianceQualified = true;
    EXPECT_FALSE(stats.IsValid());

    stats.globalIBLRadianceQualified = false;
    stats.globalIBLPixelCount = 1u;
    stats.receiverSampleCount = 3u;
    stats.backgroundPixelCount = 1u;
    EXPECT_FALSE(stats.IsValid());

    stats.globalIBLRadianceQualified = true;
    EXPECT_TRUE(stats.IsValid());

    stats.routeMaskRadianceSchemaVersion = 0u;
    EXPECT_FALSE(stats.IsValid());
}
