#include "RenderContracts/TransmissionFootprint.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <sstream>
#include <vector>

using namespace RVX;

namespace
{
    struct ExpectedSupport
    {
        uint32 minX = 0;
        uint32 maxX = 0;
        uint32 minY = 0;
        uint32 maxY = 0;
        bool valid = false;
    };

    // This deliberately enumerates the four bilinear taps instead of
    // reproducing the production guard-band arithmetic.  It is therefore
    // useful for detecting both missing taps and over-aggressive edge logic.
    ExpectedSupport EnumerateBilinearSupport(Vec2 uv, UVec2 extent)
    {
        ExpectedSupport result;
        if (extent.x == 0 || extent.y == 0)
        {
            return result;
        }

        const float32 x = uv.x * static_cast<float32>(extent.x) - 0.5f;
        const float32 y = uv.y * static_cast<float32>(extent.y) - 0.5f;
        const int32 baseX = static_cast<int32>(std::floor(x));
        const int32 baseY = static_cast<int32>(std::floor(y));
        const float32 fractionX = x - static_cast<float32>(baseX);
        const float32 fractionY = y - static_cast<float32>(baseY);

        result.minX = extent.x - 1;
        result.maxX = 0;
        result.minY = extent.y - 1;
        result.maxY = 0;

        for (int32 dy = 0; dy != 2; ++dy)
        {
            const float32 weightY = dy == 0 ? 1.0f - fractionY : fractionY;
            for (int32 dx = 0; dx != 2; ++dx)
            {
                const float32 weightX = dx == 0 ? 1.0f - fractionX : fractionX;
                if (!(weightX > 0.0f && weightY > 0.0f))
                {
                    continue;
                }

                const int32 sampleX = std::clamp(
                    baseX + dx, 0, static_cast<int32>(extent.x - 1));
                const int32 sampleY = std::clamp(
                    baseY + dy, 0, static_cast<int32>(extent.y - 1));
                result.minX = std::min(result.minX, static_cast<uint32>(sampleX));
                result.maxX = std::max(result.maxX, static_cast<uint32>(sampleX));
                result.minY = std::min(result.minY, static_cast<uint32>(sampleY));
                result.maxY = std::max(result.maxY, static_cast<uint32>(sampleY));
                result.valid = true;
            }
        }
        return result;
    }

    void IncludeSupport(ExpectedSupport& result, const ExpectedSupport& support)
    {
        if (!support.valid)
        {
            return;
        }
        if (!result.valid)
        {
            result = support;
            return;
        }
        result.minX = std::min(result.minX, support.minX);
        result.maxX = std::max(result.maxX, support.maxX);
        result.minY = std::min(result.minY, support.minY);
        result.maxY = std::max(result.maxY, support.maxY);
    }

    ExpectedSupport EnumerateReductionSupport(Vec2 uv, UVec2 sourceExtent, bool fullResolution)
    {
        ExpectedSupport result;
        if (fullResolution)
        {
            IncludeSupport(result, EnumerateBilinearSupport(uv, sourceExtent));
            return result;
        }

        // The reduction shader averages four bilinear samples displaced by
        // half a source texel in both axes.  Enumerate those samples directly;
        // this remains independent of the production floor/guard formula.
        const Vec2 halfTexel(
            0.5f / static_cast<float32>(sourceExtent.x),
            0.5f / static_cast<float32>(sourceExtent.y));
        for (const int32 sy : {-1, 1})
        {
            for (const int32 sx : {-1, 1})
            {
                IncludeSupport(result, EnumerateBilinearSupport(
                    uv + Vec2(static_cast<float32>(sx) * halfTexel.x,
                              static_cast<float32>(sy) * halfTexel.y),
                    sourceExtent));
            }
        }
        return result;
    }

    std::vector<float32> MakeAxisSamples(uint32 extent)
    {
        std::vector<float32> samples{0.0f, 1.0f};
        constexpr std::array<float32, 5> fractions{
            0.0f, 0.25f, 0.5f, 0.75f, 0.999f};
        for (uint32 texel = 0; texel < extent; ++texel)
        {
            for (const float32 fraction : fractions)
            {
                samples.push_back(
                    (static_cast<float32>(texel) + fraction) /
                    static_cast<float32>(extent));
            }
        }
        std::sort(samples.begin(), samples.end());
        samples.erase(std::unique(samples.begin(), samples.end()), samples.end());
        return samples;
    }

    void ExpectContains(
        const TransmissionTexelBounds& actual,
        const ExpectedSupport& expected,
        UVec2 extent)
    {
        ASSERT_TRUE(expected.valid);
        ASSERT_TRUE(actual.valid);
        EXPECT_LT(actual.first.x, extent.x);
        EXPECT_LT(actual.first.y, extent.y);
        EXPECT_LT(actual.last.x, extent.x);
        EXPECT_LT(actual.last.y, extent.y);
        EXPECT_LE(actual.first.x, actual.last.x);
        EXPECT_LE(actual.first.y, actual.last.y);
        EXPECT_LE(actual.first.x, expected.minX);
        EXPECT_GE(actual.last.x, expected.maxX);
        EXPECT_LE(actual.first.y, expected.minY);
        EXPECT_GE(actual.last.y, expected.maxY);
    }

    std::string ExtentLabel(UVec2 extent)
    {
        std::ostringstream label;
        label << extent.x << "x" << extent.y;
        return label.str();
    }
}

TEST(TransmissionFootprint, BilinearAndReductionEnclosePositiveWeightTaps)
{
    // Exhaustive small extents include NPOT and skinny textures.  The sample
    // set covers texel boundaries, centers, quarters, and both image edges.
    for (uint32 width = 1; width <= 9; ++width)
    {
        for (uint32 height = 1; height <= 7; ++height)
        {
            const UVec2 extent(width, height);
            const auto xSamples = MakeAxisSamples(width);
            const auto ySamples = MakeAxisSamples(height);
            SCOPED_TRACE(ExtentLabel(extent));
            for (const float32 u : xSamples)
            {
                for (const float32 v : ySamples)
                {
                    const Vec2 uv(u, v);
                    const ExpectedSupport expectedFull =
                        EnumerateReductionSupport(uv, extent, true);
                    const ExpectedSupport expectedReduced =
                        EnumerateReductionSupport(uv, extent, false);
                    const auto fullCopy =
                        ResolveTransmissionBilinearFootprint(uv, extent);
                    const auto reduced = ResolveTransmissionReductionFootprint(
                        uv, extent, false);
                    ExpectContains(fullCopy, expectedFull, extent);
                    ExpectContains(reduced, expectedReduced, extent);
                }
            }
        }
    }
}

TEST(TransmissionFootprint, EdgeClampsAndSingleTexelExtentsRemainBounded)
{
    for (const UVec2 extent : {UVec2(1, 1), UVec2(1, 7), UVec2(9, 1), UVec2(4, 3)})
    {
        for (const Vec2 uv : {Vec2(0.0f), Vec2(0.5f), Vec2(1.0f), Vec2(0.0f, 1.0f),
                              Vec2(1.0f, 0.0f), Vec2(1.0f)})
        {
            const auto bounds = ResolveTransmissionBilinearFootprint(uv, extent);
            ASSERT_TRUE(bounds.valid);
            EXPECT_LT(bounds.first.x, extent.x);
            EXPECT_LT(bounds.first.y, extent.y);
            EXPECT_LT(bounds.last.x, extent.x);
            EXPECT_LT(bounds.last.y, extent.y);
            EXPECT_LE(bounds.first.x, bounds.last.x);
            EXPECT_LE(bounds.first.y, bounds.last.y);
            if (extent.x == 1)
            {
                EXPECT_EQ(bounds.first.x, 0u);
                EXPECT_EQ(bounds.last.x, 0u);
            }
            if (extent.y == 1)
            {
                EXPECT_EQ(bounds.first.y, 0u);
                EXPECT_EQ(bounds.last.y, 0u);
            }
        }
    }
}

TEST(TransmissionFootprint, ReductionCoversCeilAndFloorHalfResolutionDestinations)
{
    // High starts with ceil-half dimensions while later levels use floor-half
    // dimensions.  Exercise destination pixel centers for both policies.
    for (uint32 width = 2; width <= 9; ++width)
    {
        for (uint32 height = 2; height <= 7; ++height)
        {
            const UVec2 source(width, height);
            const std::array<UVec2, 2> destinations{
                UVec2((width + 1) / 2, (height + 1) / 2),
                UVec2(std::max(1u, width / 2), std::max(1u, height / 2))};
            for (const UVec2 destination : destinations)
            {
                for (uint32 y = 0; y < destination.y; ++y)
                {
                    for (uint32 x = 0; x < destination.x; ++x)
                    {
                        const Vec2 uv(
                            (static_cast<float32>(x) + 0.5f) /
                                static_cast<float32>(destination.x),
                            (static_cast<float32>(y) + 0.5f) /
                                static_cast<float32>(destination.y));
                        const auto expected =
                            EnumerateReductionSupport(uv, source, false);
                        const auto actual =
                            ResolveTransmissionReductionFootprint(uv, source, false);
                        ExpectContains(actual, expected, source);
                    }
                }
            }
        }
    }
}

TEST(TransmissionFootprint, RejectsInvalidUvAndExtents)
{
    const UVec2 validExtent(8, 5);
    for (const Vec2 uv : {
             Vec2(std::numeric_limits<float32>::quiet_NaN(), 0.5f),
             Vec2(0.5f, std::numeric_limits<float32>::quiet_NaN()),
             Vec2(std::numeric_limits<float32>::infinity(), 0.5f),
             Vec2(0.5f, -std::numeric_limits<float32>::infinity()),
             Vec2(-0.001f, 0.5f),
             Vec2(0.5f, 1.001f)})
    {
        EXPECT_FALSE(ResolveTransmissionBilinearFootprint(uv, validExtent).valid);
        EXPECT_FALSE(ResolveTransmissionReductionFootprint(uv, validExtent, false).valid);
        EXPECT_FALSE(ResolveTransmissionMipFootprint(uv, validExtent, 4, 1.0f).valid);
    }

    for (const UVec2 extent : {UVec2(0, 1), UVec2(1, 0),
                               UVec2(RVX_TRANSMISSION_FOOTPRINT_MAX_EXTENT + 1, 1),
                               UVec2(1, RVX_TRANSMISSION_FOOTPRINT_MAX_EXTENT + 1)})
    {
        EXPECT_EQ(GetTransmissionMipCount(extent), 0u);
        EXPECT_EQ(GetTransmissionMipExtent(extent, 0), UVec2(0));
        EXPECT_FALSE(ResolveTransmissionBilinearFootprint({0.5f, 0.5f}, extent).valid);
        EXPECT_FALSE(ResolveTransmissionReductionFootprint(
            {0.5f, 0.5f}, extent, false).valid);
    }
}

TEST(TransmissionFootprint, MipChainUsesClampedNpotExtents)
{
    const UVec2 base(7, 3);
    const std::array<UVec2, 3> expectedExtents{UVec2(7, 3), UVec2(3, 1), UVec2(1, 1)};
    ASSERT_EQ(GetTransmissionMipCount(base), expectedExtents.size());
    for (uint32 mip = 0; mip < expectedExtents.size(); ++mip)
    {
        EXPECT_EQ(GetTransmissionMipExtent(base, mip), expectedExtents[mip]);
    }
    EXPECT_EQ(GetTransmissionMipExtent(base, expectedExtents.size()), UVec2(0));
    EXPECT_EQ(GetTransmissionMipCount({RVX_TRANSMISSION_FOOTPRINT_MAX_EXTENT,
                                       RVX_TRANSMISSION_FOOTPRINT_MAX_EXTENT}), 17u);

    const auto footprint = ResolveTransmissionMipFootprint(
        {0.37f, 0.62f}, base, GetTransmissionMipCount(base), 1.25f);
    ASSERT_TRUE(footprint.valid);
    EXPECT_EQ(footprint.lowerMip, 1u);
    EXPECT_EQ(footprint.upperMip, 2u);
    ExpectContains(footprint.lower,
                   EnumerateReductionSupport({0.37f, 0.62f}, expectedExtents[1], true),
                   expectedExtents[1]);
    ExpectContains(footprint.upper,
                   EnumerateReductionSupport({0.37f, 0.62f}, expectedExtents[2], true),
                   expectedExtents[2]);

    const auto integerLod = ResolveTransmissionMipFootprint(
        {0.37f, 0.62f}, base, GetTransmissionMipCount(base), 1.0f);
    ASSERT_TRUE(integerLod.valid);
    EXPECT_EQ(integerLod.lowerMip, 1u);
    EXPECT_EQ(integerLod.upperMip, 1u);
    EXPECT_EQ(integerLod.lower.first, integerLod.upper.first);
    EXPECT_EQ(integerLod.lower.last, integerLod.upper.last);

    const auto clampedLow = ResolveTransmissionMipFootprint(
        {0.37f, 0.62f}, base, GetTransmissionMipCount(base), -100.0f);
    ASSERT_TRUE(clampedLow.valid);
    EXPECT_EQ(clampedLow.lowerMip, 0u);
    EXPECT_EQ(clampedLow.upperMip, 0u);
    const auto clampedHigh = ResolveTransmissionMipFootprint(
        {0.37f, 0.62f}, base, GetTransmissionMipCount(base), 100.0f);
    ASSERT_TRUE(clampedHigh.valid);
    EXPECT_EQ(clampedHigh.lowerMip, 2u);
    EXPECT_EQ(clampedHigh.upperMip, 2u);
}

TEST(TransmissionFootprint, RejectsInvalidMipRequests)
{
    const UVec2 base(7, 3);
    const uint32 mipCount = GetTransmissionMipCount(base);
    EXPECT_FALSE(ResolveTransmissionMipFootprint({0.5f, 0.5f}, base, 0, 0.0f).valid);
    EXPECT_FALSE(ResolveTransmissionMipFootprint({0.5f, 0.5f}, base, mipCount + 1, 0.0f).valid);
    EXPECT_FALSE(ResolveTransmissionMipFootprint(
        {0.5f, 0.5f}, base, mipCount, std::numeric_limits<float32>::quiet_NaN()).valid);
    EXPECT_FALSE(ResolveTransmissionMipFootprint(
        {0.5f, 0.5f}, base, mipCount, std::numeric_limits<float32>::infinity()).valid);
}

TEST(TransmissionFootprint, CanonicalDepthFailsClosedAndHandlesReverseZ)
{
    EXPECT_FLOAT_EQ(CanonicalTransmissionDepth(0.25f, false), 0.25f);
    EXPECT_FLOAT_EQ(CanonicalTransmissionDepth(0.25f, true), 0.75f);
    EXPECT_FLOAT_EQ(CanonicalTransmissionDepth(0.0f, true), 1.0f);
    EXPECT_FLOAT_EQ(CanonicalTransmissionDepth(1.0f, true), 0.0f);
    for (const float32 invalid : {-0.001f, 1.001f,
                                  std::numeric_limits<float32>::quiet_NaN(),
                                  std::numeric_limits<float32>::infinity(),
                                  -std::numeric_limits<float32>::infinity()})
    {
        EXPECT_FLOAT_EQ(CanonicalTransmissionDepth(invalid, false), 0.0f);
        EXPECT_FLOAT_EQ(CanonicalTransmissionDepth(invalid, true), 0.0f);
    }
}
