#include "RenderContracts/TransmissionProjection.h"

#include <gtest/gtest.h>
#include <limits>

using namespace RVX;

TEST(TransmissionProjection, NormalIncidenceUsesNormalThickness)
{
    const auto ray = ResolveTransmissionRay({0, 0, -1}, {0, 0, 1}, 1.5f, 2.0f);
    ASSERT_TRUE(ray.valid);
    EXPECT_FLOAT_EQ(ray.pathLength, 2.0f);
    EXPECT_EQ(ray.displacement, Vec3(0, 0, -2));
}

TEST(TransmissionProjection, ObliquePathObeysSnellAndSlabThickness)
{
    const auto ray = ResolveTransmissionRay({0.6f, 0, -0.8f}, {0, 0, 1}, 1.5f, 1);
    ASSERT_TRUE(ray.valid);
    EXPECT_NEAR(normalize(ray.displacement).x, 0.4f, 1.0e-6f);
    EXPECT_NEAR(ray.displacement.z, -1.0f, 1.0e-6f);
    EXPECT_GT(ray.pathLength, 1.0f);
    const auto back = ResolveTransmissionRay({0.6f, 0, -0.8f}, {0, 0, -1}, 1.5f, 1);
    EXPECT_EQ(back.displacement, ray.displacement);
}

TEST(TransmissionProjection, UnitIORAndZeroThicknessDoNotShiftPerspectiveRay)
{
    const Mat4 projection = glm::perspective(glm::radians(60.0f), 1.5f, 0.1f, 100.0f);
    const Vec3 position(0.5f, 0.25f, -3.0f);
    Vec2 base;
    ASSERT_TRUE(ProjectTransmissionCandidate(projection, position, {640, 360}, base));
    for (const Vec2 optical : {Vec2(1.0f, 2.0f), Vec2(1.5f, 0.0f)})
    {
        const auto ray = ResolveTransmissionRay(position, {0, 0, 1}, optical.x, optical.y);
        ASSERT_TRUE(ray.valid);
        Vec2 uv;
        ASSERT_TRUE(ProjectTransmissionCandidate(projection, position + ray.displacement,
                                                 {640, 360}, uv));
        EXPECT_NEAR(uv.x, base.x, 1.0e-6f);
        EXPECT_NEAR(uv.y, base.y, 1.0e-6f);
    }
}

TEST(TransmissionProjection, RejectsOffscreenBehindCameraAndNonfiniteCandidates)
{
    const Mat4 projection = glm::perspective(glm::radians(60.0f), 1.0f, 0.1f, 100.0f);
    Vec2 uv(0.25f);
    for (const Vec3 position : {Vec3(100, 0, -1), Vec3(0, 0, 1),
         Vec3(std::numeric_limits<float>::infinity(), 0, -1)})
    {
        EXPECT_FALSE(ProjectTransmissionCandidate(projection, position, {640, 360}, uv));
        EXPECT_EQ(uv, Vec2(0.25f));
    }
    EXPECT_FALSE(ProjectTransmissionCandidate(projection, {0, 0, -1}, {0, 360}, uv));
}

TEST(TransmissionProjection, RejectsForegroundAtBothDepthConventions)
{
    for (const bool reverse : {false, true})
    {
        const auto encode = [reverse](float d) { return reverse ? 1.0f - d : d; };
        EXPECT_TRUE(IsTransmissionDepthBehindExit(encode(0.8f), encode(0.5f), reverse));
        EXPECT_TRUE(IsTransmissionDepthBehindExit(encode(1.0f), encode(0.5f), reverse));
        EXPECT_FALSE(IsTransmissionDepthBehindExit(encode(0.2f), encode(0.5f), reverse));
        EXPECT_FALSE(IsTransmissionDepthBehindExit(encode(0.5f), encode(0.5f), reverse));
        EXPECT_FALSE(IsTransmissionDepthBehindExit(-1.0f, encode(0.5f), reverse));
        EXPECT_FALSE(IsTransmissionDepthBehindExit(
            std::numeric_limits<float>::quiet_NaN(), encode(0.5f), reverse));
    }
}

TEST(TransmissionProjection, RejectsInvalidOpticalInputs)
{
    EXPECT_FALSE(ResolveTransmissionRay({0, 0, 0}, {0, 0, 1}, 1.5f, 1).valid);
    EXPECT_FALSE(ResolveTransmissionRay({0, 0, -1}, {0, 0, 0}, 1.5f, 1).valid);
    EXPECT_FALSE(ResolveTransmissionRay({0, 0, -1}, {0, 0, 1}, 0.9f, 1).valid);
    EXPECT_FALSE(ResolveTransmissionRay({0, 0, -1}, {0, 0, 1}, 1.5f, -1).valid);
    EXPECT_FALSE(ResolveTransmissionRay({1, 0, 0}, {0, 0, 1}, 1, 1).valid);
    EXPECT_FALSE(ResolveTransmissionRay({0, 0, -1}, {0, 0, 1},
        std::numeric_limits<float>::quiet_NaN(), 1).valid);
}
