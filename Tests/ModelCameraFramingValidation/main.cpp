/**
 * @file main.cpp
 * @brief CPU-only validation for deterministic model camera framing.
 */

#include "Runtime/Camera/OrbitCameraRig.h"
#include "Samples/ModelCameraFraming.h"
#include "Samples/SampleOrbitCameraController.h"
#include "World/ECS/WorldEcsCameraService.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <limits>

namespace RVX
{
    namespace
    {
        constexpr float kVerticalFov = 0.78539816339f;

        struct ViewDepthRange
        {
            float nearest = std::numeric_limits<float>::infinity();
            float farthest = -std::numeric_limits<float>::infinity();
        };

        ViewDepthRange GetViewDepthRange(const AABB& bounds,
                                         const OrbitCameraRigPose& pose)
        {
            ViewDepthRange range;
            const Vec3 min = bounds.GetMin();
            const Vec3 max = bounds.GetMax();
            const Vec3 corners[8] = {
                Vec3(min.x, min.y, min.z), Vec3(max.x, min.y, min.z),
                Vec3(min.x, max.y, min.z), Vec3(max.x, max.y, min.z),
                Vec3(min.x, min.y, max.z), Vec3(max.x, min.y, max.z),
                Vec3(min.x, max.y, max.z), Vec3(max.x, max.y, max.z)};
            for (const Vec3& corner : corners)
            {
                const float depth = dot(corner - pose.position,
                                        pose.viewBasis.forward);
                range.nearest = std::min(range.nearest, depth);
                range.farthest = std::max(range.farthest, depth);
            }
            return range;
        }

        void ExpectBoundsInsidePerspective(const AABB& bounds,
                                           const OrbitCameraRigPose& pose)
        {
            ASSERT_TRUE(pose.valid);
            const Vec3 min = bounds.GetMin();
            const Vec3 max = bounds.GetMax();
            const Vec3 corners[8] = {
                Vec3(min.x, min.y, min.z), Vec3(max.x, min.y, min.z),
                Vec3(min.x, max.y, min.z), Vec3(max.x, max.y, min.z),
                Vec3(min.x, min.y, max.z), Vec3(max.x, min.y, max.z),
                Vec3(min.x, max.y, max.z), Vec3(max.x, max.y, max.z)};
            const float tanVertical = std::tan(pose.verticalFovRadians * 0.5f);
            const float tanHorizontal = tanVertical * pose.aspectRatio;

            for (const Vec3& corner : corners)
            {
                const Vec3 cameraToCorner = corner - pose.position;
                const float depth = dot(cameraToCorner, pose.viewBasis.forward);
                EXPECT_GT(depth, 0.0f);
                EXPECT_LE(std::abs(dot(cameraToCorner, pose.viewBasis.right)),
                          depth * tanHorizontal + 0.0001f);
                EXPECT_LE(std::abs(dot(cameraToCorner, pose.viewBasis.up)),
                          depth * tanVertical + 0.0001f);
            }
        }
    } // namespace

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
        EXPECT_LT(frame.nearPlane, frame.distance);
        EXPECT_GT(frame.farPlane, frame.distance);
    }

    TEST(ModelCameraFramingValidation,
         FitsEveryAABBCornerForPortraitLandscapeAndNonUniformScale)
    {
        const AABB bounds(Vec3(-0.002f, -3000.0f, -0.25f),
                          Vec3(0.004f, 7000.0f, 1.5f));
        for (const float aspect : {16.0f / 9.0f, 9.0f / 16.0f})
        {
            OrbitCameraRigSettings settings;
            settings.mode = OrbitCameraMode::ExteriorInspect;
            settings.bounds = bounds;
            settings.pivot = bounds.GetCenter();
            settings.yaw = 0.73f;
            settings.pitch = -0.41f;
            settings.aspectRatio = aspect;
            settings.verticalFovRadians = kVerticalFov;
            settings.maxDistance = 1.0f;

            OrbitCameraRig rig;
            ASSERT_TRUE(rig.Initialize(settings));
            ASSERT_TRUE(rig.Fit());
            ExpectBoundsInsidePerspective(bounds, rig.GetPose());
        }
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
            AABB(Vec3(-0.0001f), Vec3(0.0001f)), 1.0f, kVerticalFov);
        const ModelCameraFrame large = BuildModelCameraFrame(
            AABB(Vec3(-100000.0f), Vec3(100000.0f)), 1.0f, kVerticalFov);
        const ModelCameraFrame extreme = BuildModelCameraFrame(
            AABB(Vec3(-1.0e20f), Vec3(1.0e20f)), 1.0f, kVerticalFov);

        ASSERT_TRUE(small.valid);
        ASSERT_TRUE(large.valid);
        ASSERT_TRUE(extreme.valid);
        EXPECT_TRUE(std::isfinite(small.nearPlane));
        EXPECT_TRUE(std::isfinite(large.farPlane));
        EXPECT_TRUE(std::isfinite(extreme.farPlane));
        EXPECT_GT(large.distance, small.distance);
    }

    TEST(ModelCameraFramingValidation,
         LegacySphericalClipRangeTracksCameraDistance)
    {
        const AABB bounds(Vec3(-3.45f, -3.45f, -0.55f),
                          Vec3(3.45f, 3.45f, 0.55f));
        const float radius = glm::length(bounds.GetExtent());
        const ModelCameraFrame frame = BuildModelCameraFrame(
            bounds, 16.0f / 9.0f, kVerticalFov, 1.16f);
        ASSERT_TRUE(frame.valid);

        const ModelCameraClipRange initial =
            BuildModelCameraClipRange(frame.distance, radius);
        const float zoomedDistance = frame.distance * 0.25f;
        const ModelCameraClipRange zoomed =
            BuildModelCameraClipRange(zoomedDistance, radius);

        ASSERT_TRUE(initial.valid);
        ASSERT_TRUE(zoomed.valid);
        EXPECT_GT(initial.nearPlane, 0.0f);
        EXPECT_LT(zoomed.nearPlane, initial.nearPlane);
        EXPECT_LT(zoomed.nearPlane, zoomedDistance);
        EXPECT_GT(zoomed.farPlane, zoomedDistance + radius);
        EXPECT_NEAR(zoomed.nearPlane, radius * 0.001f, 0.000001f);
    }

    TEST(ModelCameraFramingValidation,
         ExteriorInspectExponentialZoomNeverEntersTheAABB)
    {
        const AABB bounds(Vec3(-4.0f, -0.5f, -20.0f),
                          Vec3(2.0f, 3.0f, 5.0f));
        OrbitCameraRigSettings settings;
        settings.mode = OrbitCameraMode::ExteriorInspect;
        settings.bounds = bounds;
        settings.pivot = bounds.GetCenter();
        settings.yaw = 0.87f;
        settings.pitch = -0.32f;
        settings.zoomExponent = 0.25f;
        settings.maxDistance = 10000.0f;

        OrbitCameraRig rig;
        ASSERT_TRUE(rig.Initialize(settings));
        ASSERT_TRUE(rig.Fit());
        EXPECT_TRUE(rig.ConsumeDiscontinuity());

        OrbitCameraIntent zoomIn;
        zoomIn.zoomDelta = 1.0f;
        for (uint32 iteration = 0; iteration < 128; ++iteration)
        {
            rig.ApplyIntent(zoomIn);
        }

        const OrbitCameraRigPose pose = rig.GetPose();
        EXPECT_GE(pose.distance, rig.GetMinimumDistance());
        EXPECT_FALSE(bounds.Contains(pose.position));
        EXPECT_FALSE(rig.ConsumeDiscontinuity());
    }

    TEST(ModelCameraFramingValidation,
         FocusedClipRangePreservesNearCoplanarDepthPrecision)
    {
        const AABB bounds(Vec3(-1.961848f, -0.5f, -3.341879f),
                          Vec3(1.961850f, 1.854729f, 3.755531f));
        OrbitCameraRigSettings settings;
        settings.mode = OrbitCameraMode::ExteriorInspect;
        settings.bounds = bounds;
        settings.pivot = bounds.GetCenter();
        settings.yaw = 0.91f;
        settings.pitch = -0.48f;
        settings.zoomExponent = 0.5f;
        settings.minimumFramingScale = 0.0f;
        settings.maxDistance = 1000.0f;

        OrbitCameraRig rig;
        ASSERT_TRUE(rig.Initialize(settings));
        ASSERT_TRUE(rig.Fit());

        OrbitCameraIntent zoomIn;
        zoomIn.zoomDelta = 1.0f;
        for (uint32 iteration = 0; iteration < 128; ++iteration)
        {
            rig.ApplyIntent(zoomIn);
        }

        const OrbitCameraRigPose pose = rig.GetPose();
        const ViewDepthRange depthRange = GetViewDepthRange(bounds, pose);
        const float radius = glm::length(bounds.GetExtent());
        ASSERT_TRUE(pose.valid);
        EXPECT_GT(depthRange.nearest, 0.0f);
        EXPECT_LT(pose.nearPlane, depthRange.nearest);
        EXPECT_GT(pose.farPlane, depthRange.farthest);
        EXPECT_GT(pose.nearPlane, radius * 0.001f * 5.0f);
    }

    TEST(ModelCameraFramingValidation,
         ExteriorInspectRetainsConfiguredMinimumFramingScale)
    {
        const AABB bounds(Vec3(-4.0f, -1.0f, -2.0f),
                          Vec3(3.0f, 5.0f, 7.0f));
        OrbitCameraRigSettings settings;
        settings.mode = OrbitCameraMode::ExteriorInspect;
        settings.bounds = bounds;
        settings.pivot = bounds.GetCenter();
        settings.yaw = 0.63f;
        settings.pitch = 0.29f;
        settings.zoomExponent = 0.5f;
        settings.minimumFramingScale = 0.55f;
        settings.maxDistance = 1000.0f;

        OrbitCameraRig rig;
        ASSERT_TRUE(rig.Initialize(settings));
        ASSERT_TRUE(rig.Fit());
        const float fittedDistance = rig.GetPose().distance;

        OrbitCameraIntent zoomIn;
        zoomIn.zoomDelta = 1.0f;
        for (uint32 iteration = 0; iteration < 128; ++iteration)
        {
            rig.ApplyIntent(zoomIn);
        }

        const OrbitCameraRigPose pose = rig.GetPose();
        EXPECT_GE(pose.distance, fittedDistance * 0.55f - 0.0001f);
        EXPECT_NEAR(pose.distance, rig.GetMinimumDistance(), 0.0001f);
        EXPECT_FALSE(bounds.Contains(pose.position));
    }

    TEST(ModelCameraFramingValidation, FreeOrbitRetainsCallerConfiguredMinimum)
    {
        const AABB bounds(Vec3(-1.0f), Vec3(1.0f));
        OrbitCameraRigSettings settings;
        settings.mode = OrbitCameraMode::FreeOrbit;
        settings.bounds = bounds;
        settings.pivot = bounds.GetCenter();
        settings.distance = 5.0f;
        settings.minDistance = 0.1f;
        settings.maxDistance = 10.0f;
        settings.zoomExponent = 1.0f;

        OrbitCameraRig rig;
        ASSERT_TRUE(rig.Initialize(settings));
        OrbitCameraIntent zoomIn;
        zoomIn.zoomDelta = 100.0f;
        rig.ApplyIntent(zoomIn);

        EXPECT_FLOAT_EQ(rig.GetMinimumDistance(), 0.1f);
        EXPECT_NEAR(rig.GetPose().distance, 0.1f, 0.00001f);
        EXPECT_TRUE(bounds.Contains(rig.GetPose().position));
        EXPECT_FALSE(rig.ConsumeDiscontinuity());
    }

    TEST(ModelCameraFramingValidation,
         FitFocusAndResetCoalesceExactlyOneDiscontinuity)
    {
        const AABB bounds(Vec3(-2.0f, -1.0f, -3.0f),
                          Vec3(4.0f, 5.0f, 1.0f));
        OrbitCameraRigSettings settings;
        settings.mode = OrbitCameraMode::ExteriorInspect;
        settings.maxDistance = 1000.0f;

        OrbitCameraRig rig;
        ASSERT_TRUE(rig.Initialize(settings));
        EXPECT_FALSE(rig.ConsumeDiscontinuity());
        EXPECT_TRUE(rig.SetFocus(bounds, bounds.GetCenter()));
        EXPECT_TRUE(rig.ConsumeDiscontinuity());
        EXPECT_FALSE(rig.ConsumeDiscontinuity());
        EXPECT_FALSE(rig.SetFocus(bounds, bounds.GetCenter()));
        EXPECT_FALSE(rig.ConsumeDiscontinuity());

        ASSERT_TRUE(rig.Fit());
        EXPECT_TRUE(rig.ConsumeDiscontinuity());
        EXPECT_FALSE(rig.ConsumeDiscontinuity());

        rig.CaptureResetAnchor();
        OrbitCameraIntent orbit;
        orbit.orbitActive = true;
        orbit.orbitDelta = Vec2(12.0f, -8.0f);
        rig.ApplyIntent(orbit);
        EXPECT_FALSE(rig.ConsumeDiscontinuity());

        EXPECT_TRUE(rig.Reset());
        EXPECT_TRUE(rig.ConsumeDiscontinuity());
        EXPECT_FALSE(rig.ConsumeDiscontinuity());
    }

    TEST(ModelCameraFramingValidation,
         SampleControllerUsesRigProjectionAndCachesResizeBeforeInitialization)
    {
        const AABB bounds(Vec3(-2.0f), Vec3(2.0f));
        SampleOrbitCameraSettings settings;
        settings.mode = OrbitCameraMode::ExteriorInspect;
        settings.bounds = bounds;
        settings.pivot = bounds.GetCenter();
        settings.distance = 4.2f;
        settings.maxDistance = 20.0f;
        settings.pitch = 0.0f;
        settings.verticalFovRadians = kVerticalFov;
        settings.aspectRatio = 16.0f / 9.0f;

        SampleOrbitCameraController controller;
        SceneECS::SceneEcsRuntime runtime;
        WorldECS::WorldEcsCameraService cameras(runtime);
        const WorldECS::WorldEcsCameraRef camera = cameras.CreateMainCamera();
        ASSERT_TRUE(camera.IsValid());
        ASSERT_TRUE(controller.SetAspectRatio(9.0f / 16.0f, cameras, camera));
        controller.Initialize(settings);
        ASSERT_TRUE(controller.Apply(cameras, camera));

        const OrbitCameraRigPose pose = controller.GetPose();
        ASSERT_TRUE(pose.valid);
        const std::optional<SceneECS::Camera> applied = cameras.GetCamera(camera);
        ASSERT_TRUE(applied.has_value());
        EXPECT_EQ(applied->projection, SceneECS::CameraProjection::Perspective);
        EXPECT_FLOAT_EQ(applied->verticalFieldOfViewRadians, pose.verticalFovRadians);
        EXPECT_FLOAT_EQ(applied->aspectRatio, pose.aspectRatio);
        EXPECT_FLOAT_EQ(applied->nearPlane, pose.nearPlane);
        EXPECT_FLOAT_EQ(applied->farPlane, pose.farPlane);
        EXPECT_FLOAT_EQ(controller.GetSettings().aspectRatio, 9.0f / 16.0f);
        EXPECT_EQ(applied->cutRevision, 2u);

        SampleOrbitCameraInput input;
        input.orbitActive = true;
        input.pointerDelta = Vec2(10.0f, 5.0f);
        input.scrollDelta = 1.0f;
        ASSERT_TRUE(controller.ApplyInput(input, cameras, camera));
        EXPECT_EQ(cameras.GetCamera(camera)->cutRevision, 2u);
        EXPECT_NEAR(controller.GetSettings().yaw, -0.05f, 0.00001f);
        EXPECT_NEAR(controller.GetSettings().pitch, 0.025f, 0.00001f);

        input = {};
        input.orbitActive = true;
        input.pointerDelta = Vec2(0.0f, 100000.0f);
        ASSERT_TRUE(controller.ApplyInput(input, cameras, camera));
        EXPECT_FLOAT_EQ(controller.GetSettings().pitch,
                        controller.GetSettings().maxPitch);
        EXPECT_EQ(cameras.GetCamera(camera)->cutRevision, 2u);

        input = {};
        input.reset = true;
        ASSERT_TRUE(controller.ApplyInput(input, cameras, camera));
        EXPECT_EQ(cameras.GetCamera(camera)->cutRevision, 3u);
    }

    TEST(ModelCameraFramingValidation,
         SampleControllerRejectsForeignAndStaleEcsCameraReferences)
    {
        SampleOrbitCameraSettings settings;
        settings.mode = OrbitCameraMode::ExteriorInspect;
        settings.bounds = AABB(Vec3(-1.0f), Vec3(1.0f));
        settings.pivot = settings.bounds.GetCenter();

        SampleOrbitCameraController controller;
        controller.Initialize(settings);
        ASSERT_TRUE(controller.IsInitialized());

        SceneECS::SceneEcsRuntime firstRuntime;
        SceneECS::SceneEcsRuntime secondRuntime;
        WorldECS::WorldEcsCameraService first(firstRuntime);
        WorldECS::WorldEcsCameraService second(secondRuntime);
        const WorldECS::WorldEcsCameraRef firstCamera = first.CreateMainCamera();
        const WorldECS::WorldEcsCameraRef secondCamera = second.CreateMainCamera();
        ASSERT_TRUE(firstCamera.IsValid());
        ASSERT_TRUE(secondCamera.IsValid());

        const std::optional<SceneECS::Camera> foreignBefore = second.GetCamera(secondCamera);
        ASSERT_TRUE(foreignBefore.has_value());
        EXPECT_FALSE(controller.Apply(second, firstCamera));
        EXPECT_FALSE(controller.Apply(first, secondCamera));
        const std::optional<SceneECS::Camera> foreignAfter = second.GetCamera(secondCamera);
        ASSERT_TRUE(foreignAfter.has_value());
        EXPECT_EQ(foreignAfter->cutRevision, foreignBefore->cutRevision);

        ASSERT_EQ(first.RequestDestroy(firstCamera), SceneECS::DestroyRequestResult::Accepted);
        EXPECT_FALSE(controller.Apply(first, firstCamera));
        EXPECT_FALSE(controller.SetAspectRatio(1.5f, first, firstCamera));

        ASSERT_EQ(firstRuntime.PublishPendingDestroyCleanup(), 1u);
        ASSERT_EQ(firstRuntime.AdvanceRetirements(), 1u);
        ASSERT_EQ(firstRuntime.RecycleRecyclableEntities(), 1u);
        const WorldECS::WorldEcsCameraRef recycled = first.CreateMainCamera();
        ASSERT_TRUE(recycled.IsValid());
        EXPECT_EQ(recycled.entity.GetIndex(), firstCamera.entity.GetIndex());
        EXPECT_NE(recycled.entity.GetGeneration(), firstCamera.entity.GetGeneration());
        EXPECT_FALSE(controller.Apply(first, firstCamera));
    }

    TEST(ModelCameraFramingValidation, RejectsInvalidOrNonFiniteInputs)
    {
        EXPECT_FALSE(BuildModelCameraFrame(AABB(), 1.0f, kVerticalFov).valid);
        EXPECT_FALSE(BuildModelCameraFrame(
                         AABB(Vec3(1.0f), Vec3(1.0f)), 1.0f, kVerticalFov)
                         .valid);
        EXPECT_FALSE(BuildModelCameraFrame(
                         AABB(Vec3(-1.0f), Vec3(1.0f)), 0.0f, kVerticalFov)
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
                         std::numeric_limits<float>::infinity(), 1.0f)
                         .valid);

        OrbitCameraRigSettings invalidSettings;
        invalidSettings.bounds = AABB(Vec3(-1.0f), Vec3(1.0f));
        invalidSettings.minimumFramingScale = 1.01f;
        OrbitCameraRig rig;
        EXPECT_FALSE(rig.Initialize(invalidSettings));
        EXPECT_FALSE(OrbitCameraRig::BuildFocusedClipRange(
                         invalidSettings.bounds,
                         Vec3(0.0f, 0.0f, 5.0f),
                         Vec3(0.0f))
                         .valid);
    }
} // namespace RVX
