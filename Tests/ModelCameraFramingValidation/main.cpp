/**
 * @file main.cpp
 * @brief CPU-only validation for deterministic model camera framing.
 */

#include "Samples/ModelCameraFraming.h"

#include <gtest/gtest.h>

#include <cmath>
#include <limits>

namespace RVX
{
    namespace
    {
        constexpr float kVerticalFov = 0.78539816339f;
    }

    TEST(ModelCameraFramingValidation, FitsFiniteBoundsAndDerivesSafeClipPlanes)
    {
        const AABB bounds(Vec3(-2.0f, -1.0f, -3.0f),
                          Vec3(4.0f, 5.0f, 1.0f));
        const ModelCameraFrame frame =
            BuildModelCameraFrame(bounds, 16.0f / 9.0f, kVerticalFov);
        const float radius = glm::length(bounds.GetExtent());

        ASSERT_TRUE(frame.valid);
        EXPECT_EQ(bounds.GetCenter(), frame.target);
        EXPECT_GT(frame.distance, radius);
        EXPECT_GT(frame.nearPlane, 0.0f);
        EXPECT_LT(frame.nearPlane, frame.distance - radius);
        EXPECT_GT(frame.farPlane, frame.distance + radius);
    }

    TEST(ModelCameraFramingValidation, PortraitViewportUsesTheHorizontalLimit)
    {
        const AABB bounds(Vec3(-1.0f), Vec3(1.0f));
        const ModelCameraFrame landscape =
            BuildModelCameraFrame(bounds, 16.0f / 9.0f, kVerticalFov);
        const ModelCameraFrame portrait =
            BuildModelCameraFrame(bounds, 9.0f / 16.0f, kVerticalFov);

        ASSERT_TRUE(landscape.valid);
        ASSERT_TRUE(portrait.valid);
        EXPECT_GT(portrait.distance, landscape.distance);
    }

    TEST(ModelCameraFramingValidation, SupportsVerySmallAndVeryLargeAssetUnits)
    {
        const ModelCameraFrame small = BuildModelCameraFrame(
            AABB(Vec3(-0.0001f), Vec3(0.0001f)),
            1.0f,
            kVerticalFov);
        const ModelCameraFrame large = BuildModelCameraFrame(
            AABB(Vec3(-100000.0f), Vec3(100000.0f)),
            1.0f,
            kVerticalFov);

        ASSERT_TRUE(small.valid);
        ASSERT_TRUE(large.valid);
        EXPECT_TRUE(std::isfinite(small.nearPlane));
        EXPECT_TRUE(std::isfinite(large.farPlane));
        EXPECT_GT(large.distance, small.distance);
    }

    TEST(ModelCameraFramingValidation, RejectsInvalidOrNonFiniteInputs)
    {
        EXPECT_FALSE(BuildModelCameraFrame(AABB(), 1.0f, kVerticalFov).valid);
        EXPECT_FALSE(BuildModelCameraFrame(
                         AABB(Vec3(1.0f), Vec3(1.0f)),
                         1.0f,
                         kVerticalFov)
                         .valid);
        EXPECT_FALSE(BuildModelCameraFrame(
                         AABB(Vec3(-1.0f), Vec3(1.0f)),
                         0.0f,
                         kVerticalFov)
                         .valid);
        EXPECT_FALSE(BuildModelCameraFrame(
                         AABB(Vec3(-1.0f), Vec3(1.0f)),
                         1.0f,
                         std::numeric_limits<float>::infinity())
                         .valid);
        EXPECT_FALSE(BuildModelCameraFrame(
                         AABB(Vec3(-1.0f), Vec3(1.0f)),
                         1.0f,
                         kVerticalFov,
                         0.99f)
                         .valid);
    }
} // namespace RVX
