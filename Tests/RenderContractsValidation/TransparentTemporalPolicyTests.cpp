#include "RenderContracts/TransparentTemporalPolicy.h"

#include <gtest/gtest.h>
#include <limits>

namespace RVX
{
    namespace
    {
        TransparentTemporalSample Surface()
        {
            TransparentTemporalSample s;
            s.motion = {0.1f, 0.2f};
            s.coverage = 1.0f;
            s.viewDepth = 2.0f;
            s.instanceId = s.previousInstanceId = 4;
            s.generation = s.previousGeneration = 7;
            s.previousSubmittedFrame = 8;
            s.previousShapeAvailable = true;
            return s;
        }
    }

    TEST(TransparentTemporalPolicy, UsesLastSubmittedSurfaceAcrossSimulationSequenceGaps)
    {
        const auto result = ResolveTransparentTemporalSample(Surface(), {0, 0}, 10, 12, 8);
        ASSERT_TRUE(result.valid);
        EXPECT_EQ(result.source, TransparentTemporalVelocitySource::Transparent);
        EXPECT_EQ(result.motion, Vec2(0.1f, 0.2f));
    }

    TEST(TransparentTemporalPolicy, RejectsHistoryFromUnsubmittedFrameOrReusedInstance)
    {
        for (int cause = 0; cause != 4; ++cause)
        {
            auto s = Surface();
            if (cause == 0) s.previousSubmittedFrame = 9;
            if (cause == 1) ++s.generation;
            if (cause == 2) s.previousShapeAvailable = false;
            if (cause == 3) s.discontinuity = true;
            const auto result = ResolveTransparentTemporalSample(s, {0, 0}, 10, 12, 8);
            ASSERT_TRUE(result.valid);
            EXPECT_EQ(result.source, TransparentTemporalVelocitySource::Opaque);
            EXPECT_EQ(result.reactive, 1.0f);
        }
    }

    TEST(TransparentTemporalPolicy, MixturesRemainReactiveWithoutOverwritingOpaqueVelocity)
    {
        for (int mode = 0; mode != 4; ++mode)
        {
            auto s = Surface();
            if (mode == 0) s.coverage = 0.5f;
            if (mode == 1) s.overlappingLayers = true;
            if (mode == 2) s.mode = TransparentTemporalMode::WeightedBlended;
            if (mode == 3) s.refractive = true;
            const auto result = ResolveTransparentTemporalSample(s, {0.3f, 0.4f}, 10, 12, 8);
            ASSERT_TRUE(result.valid);
            EXPECT_EQ(result.source, TransparentTemporalVelocitySource::Opaque);
            EXPECT_EQ(result.motion, Vec2(0.3f, 0.4f));
            EXPECT_EQ(result.reactive, s.coverage);
        }
    }

    TEST(TransparentTemporalPolicy, OccludedSurfaceCannotReplaceMotion)
    {
        auto s = Surface();
        s.viewDepth = 11;
        const auto result = ResolveTransparentTemporalSample(s, {0, 0}, 10, 12, 8);
        EXPECT_TRUE(result.valid);
        EXPECT_EQ(result.source, TransparentTemporalVelocitySource::Opaque);
        EXPECT_EQ(result.reactive, 0.0f);
        s.coverage = std::numeric_limits<float>::quiet_NaN();
        EXPECT_FALSE(ResolveTransparentTemporalSample(s, {0, 0}, 10, 12, 8).valid);
    }
}
