/**
 * @file main.cpp
 * @brief CPU-only validation for deterministic model camera framing.
 */

#include "Samples/ModelCameraFraming.h"
#include "Samples/SampleOrbitCameraController.h"
#include "Scene/Components/CameraComponent.h"
#include "Scene/SceneEntity.h"

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

    TEST(ModelCameraFramingValidation, OrbitClipRangeTracksCameraDistance)
    {
        const AABB bounds(Vec3(-3.45f, -3.45f, -0.55f),
                          Vec3(3.45f, 3.45f, 0.55f));
        const float radius = glm::length(bounds.GetExtent());
        const ModelCameraFrame frame =
            BuildModelCameraFrame(bounds, 16.0f / 9.0f, kVerticalFov, 1.16f);
        ASSERT_TRUE(frame.valid);

        const ModelCameraClipRange initial =
            BuildModelCameraClipRange(frame.distance, radius);
        const float zoomedDistance = frame.distance * 0.25f;
        const ModelCameraClipRange zoomed =
            BuildModelCameraClipRange(zoomedDistance, radius);

        ASSERT_TRUE(initial.valid);
        ASSERT_TRUE(zoomed.valid);
        EXPECT_FLOAT_EQ(initial.nearPlane, frame.nearPlane);
        EXPECT_LT(zoomed.nearPlane, initial.nearPlane);
        EXPECT_LT(zoomed.nearPlane, zoomedDistance);
        EXPECT_GT(zoomed.farPlane, zoomedDistance + radius);
        EXPECT_NEAR(zoomed.nearPlane, radius * 0.001f, 0.000001f);
    }

    TEST(ModelCameraFramingValidation, OrbitControllerAppliesDynamicClipProjection)
    {
        SampleOrbitCameraSettings settings;
        settings.target = Vec3(0.0f);
        settings.distance = 4.2f;
        settings.minDistance = 4.0f;
        settings.maxDistance = 20.0f;
        settings.pitch = 0.0f;
        settings.verticalFovRadians = kVerticalFov;
        settings.aspectRatio = 16.0f / 9.0f;
        settings.boundsRadius = 4.9f;

        SampleOrbitCameraController controller;
        SceneEntity cameraActor("OrbitCamera");
        CameraComponent* camera =
            cameraActor.AddComponent<CameraComponent>();
        ASSERT_NE(camera, nullptr);
        controller.Initialize(settings);
        controller.Apply(*camera);

        const ModelCameraClipRange clipRange = BuildModelCameraClipRange(
            settings.distance,
            settings.boundsRadius);
        ASSERT_TRUE(clipRange.valid);
        const Mat4 expectedProjection = MakePerspective(
            settings.verticalFovRadians,
            settings.aspectRatio,
            clipRange.nearPlane,
            clipRange.farPlane);
        for (uint32 column = 0; column < 4; ++column)
        {
            for (uint32 row = 0; row < 4; ++row)
            {
                EXPECT_FLOAT_EQ(camera->GetProjectionMatrix()[column][row],
                                expectedProjection[column][row]);
            }
        }
        EXPECT_FLOAT_EQ(cameraActor.GetWorldPosition().z, settings.distance);
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
        EXPECT_FALSE(BuildModelCameraClipRange(0.0f, 1.0f).valid);
        EXPECT_FALSE(BuildModelCameraClipRange(1.0f, 0.0f).valid);
        EXPECT_FALSE(BuildModelCameraClipRange(
                         std::numeric_limits<float>::infinity(),
                         1.0f)
                         .valid);
    }
} // namespace RVX
