#include "World/ECS/WorldEcsCameraService.h"

#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <memory>

namespace
{
using namespace RVX;
using namespace RVX::SceneECS;
using namespace RVX::WorldECS;

[[nodiscard]] SceneEcsTickResult Tick(SceneEcsRuntime& runtime)
{
    return runtime.Tick({.variableDeltaSeconds = 1.0 / 60.0});
}

TEST(EcsCameraServiceValidation, MainCameraIsSelectedByTheSameFrameFrozenSnapshot)
{
    SceneEcsRuntime runtime;
    WorldEcsCameraService cameras(runtime);

    WorldEcsCameraCreateDesc desc;
    desc.camera.priority = -5;
    desc.camera.clearPolicy = CameraClearPolicy::SolidColor;
    desc.camera.clearColor = {0.2f, 0.3f, 0.4f, 1.0f};
    const WorldEcsCameraRef main = cameras.CreateMainCamera(desc);
    ASSERT_TRUE(main.IsValid());
    EXPECT_TRUE(cameras.SetPerspective(main, radians(65.0f), 1.6f, 0.05f, 800.0f));
    EXPECT_TRUE(cameras.SetViewport(main, {0.1f, 0.2f, 0.7f, 0.6f}));
    EXPECT_TRUE(cameras.SetCullingMask(main, 0x03u));

    const SceneEcsTickResult result = Tick(runtime);
    ASSERT_TRUE(result.succeeded);
    const std::shared_ptr<const FrozenSceneSnapshot> snapshot = runtime.GetLatestFrozenSnapshot();
    ASSERT_NE(snapshot, nullptr);
    ASSERT_TRUE(snapshot->selectedCamera.has_value());
    ASSERT_EQ(snapshot->cameras.size(), 1u);
    EXPECT_EQ(snapshot->selectedCamera->sceneRuntimeId, main.sceneRuntimeId);
    EXPECT_EQ(snapshot->selectedCamera->entity, main.entity);
    const WorldEcsCameraRef active = cameras.GetActiveCamera();
    EXPECT_EQ(active.sceneRuntimeId, main.sceneRuntimeId);
    EXPECT_EQ(active.entity, main.entity);
    EXPECT_EQ(snapshot->cameras.front().source.clearPolicy, CameraClearPolicy::SolidColor);
    EXPECT_EQ(snapshot->cameras.front().source.cullingMask, 0x03u);
}

TEST(EcsCameraServiceValidation, ActivationExclusivelyAndDeterministicallySwitchesFrozenSelection)
{
    SceneEcsRuntime runtime;
    WorldEcsCameraService cameras(runtime);
    const WorldEcsCameraRef first = cameras.CreateMainCamera();
    const WorldEcsCameraRef second = cameras.CreateCamera();
    ASSERT_TRUE(first.IsValid());
    ASSERT_TRUE(second.IsValid());
    ASSERT_TRUE(Tick(runtime).succeeded);
    WorldEcsCameraRef active = cameras.GetActiveCamera();
    EXPECT_EQ(active.sceneRuntimeId, first.sceneRuntimeId);
    EXPECT_EQ(active.entity, first.entity);

    EXPECT_TRUE(cameras.Activate(second));
    ASSERT_TRUE(Tick(runtime).succeeded);
    active = cameras.GetActiveCamera();
    EXPECT_EQ(active.sceneRuntimeId, second.sceneRuntimeId);
    EXPECT_EQ(active.entity, second.entity);

    const std::optional<Camera> firstCamera = cameras.GetCamera(first);
    const std::optional<Camera> secondCamera = cameras.GetCamera(second);
    ASSERT_TRUE(firstCamera.has_value());
    ASSERT_TRUE(secondCamera.has_value());
    EXPECT_FALSE(firstCamera->enabled);
    EXPECT_TRUE(secondCamera->enabled);
    EXPECT_EQ(secondCamera->priority, std::numeric_limits<int32>::max());
}

TEST(EcsCameraServiceValidation, ProjectionPoseAndLookAtRejectNonFiniteInputs)
{
    SceneEcsRuntime runtime;
    WorldEcsCameraService cameras(runtime);
    const WorldEcsCameraRef camera = cameras.CreateMainCamera();
    ASSERT_TRUE(camera.IsValid());

    EXPECT_TRUE(cameras.SetPerspective(camera, radians(45.0f), 16.0f / 9.0f, 0.1f, 250.0f));
    EXPECT_FALSE(cameras.SetPerspective(camera,
                                        std::numeric_limits<float32>::quiet_NaN(),
                                        1.0f,
                                        0.1f,
                                        10.0f));
    EXPECT_FALSE(cameras.SetPerspective(camera, radians(45.0f), 0.0f, 0.1f, 10.0f));
    EXPECT_TRUE(cameras.SetOrthographic(camera, 7.5f, 1.5f, 0.1f, 250.0f));
    EXPECT_FALSE(cameras.SetOrthographic(camera, 7.5f, 1.5f, 50.0f, 50.0f));
    EXPECT_FALSE(cameras.SetViewport(camera, {0.0f, 0.0f, 1.1f, 1.0f}));
    EXPECT_TRUE(cameras.SetPose(camera,
                                {.position = {3.0f, 2.0f, 6.0f},
                                 .rotation = {1.0f, 0.0f, 0.0f, 0.0f}}));
    EXPECT_FALSE(cameras.SetPose(camera,
                                 {.position = {std::numeric_limits<float32>::infinity(), 0.0f, 0.0f},
                                  .rotation = {1.0f, 0.0f, 0.0f, 0.0f}}));
    EXPECT_TRUE(cameras.LookAt(camera, {0.0f, 0.5f, 0.0f}));
    EXPECT_FALSE(cameras.LookAt(camera, {3.0f, 2.0f, 6.0f}));

    ASSERT_TRUE(Tick(runtime).succeeded);
    const std::shared_ptr<const FrozenSceneSnapshot> snapshot = runtime.GetLatestFrozenSnapshot();
    ASSERT_NE(snapshot, nullptr);
    ASSERT_EQ(snapshot->cameras.size(), 1u);
    EXPECT_EQ(snapshot->cameras.front().source.projection, CameraProjection::Orthographic);
    EXPECT_FLOAT_EQ(snapshot->cameras.front().source.orthographicHalfHeight, 7.5f);
    EXPECT_TRUE(std::isfinite(snapshot->cameras.front().worldTransform[3].x));
    EXPECT_TRUE(std::isfinite(snapshot->cameras.front().worldTransform[3].y));
    EXPECT_TRUE(std::isfinite(snapshot->cameras.front().worldTransform[3].z));
}

TEST(EcsCameraServiceValidation, RejectsForeignAndStaleGenerationQualifiedReferences)
{
    SceneEcsRuntime firstRuntime;
    SceneEcsRuntime secondRuntime;
    WorldEcsCameraService first(firstRuntime);
    WorldEcsCameraService second(secondRuntime);
    const WorldEcsCameraRef firstCamera = first.CreateMainCamera();
    ASSERT_TRUE(firstCamera.IsValid());

    EXPECT_FALSE(second.Activate(firstCamera));
    EXPECT_FALSE(second.SetExposure(firstCamera, 2.0f));
    EXPECT_EQ(second.RequestDestroy(firstCamera), DestroyRequestResult::InvalidEntity);

    ASSERT_EQ(first.RequestDestroy(firstCamera), DestroyRequestResult::Accepted);
    EXPECT_FALSE(first.SetExposure(firstCamera, 1.0f));
    EXPECT_FALSE(first.Activate(firstCamera));
    EXPECT_FALSE(first.GetCamera(firstCamera).has_value());
}

TEST(EcsCameraServiceValidation, CutAndDestroyRetirementPreserveGenerationSafety)
{
    SceneEcsRuntime runtime;
    WorldEcsCameraService cameras(runtime);
    const WorldEcsCameraRef camera = cameras.CreateMainCamera();
    ASSERT_TRUE(camera.IsValid());
    ASSERT_TRUE(Tick(runtime).succeeded);

    const std::optional<Camera> beforeCut = cameras.GetCamera(camera);
    ASSERT_TRUE(beforeCut.has_value());
    EXPECT_TRUE(cameras.MarkCut(camera));
    const std::optional<Camera> afterCut = cameras.GetCamera(camera);
    ASSERT_TRUE(afterCut.has_value());
    EXPECT_EQ(afterCut->cutRevision, beforeCut->cutRevision + 1u);
    EXPECT_NE(afterCut->cutRevision, 0u);

    ASSERT_EQ(cameras.RequestDestroy(camera), DestroyRequestResult::Accepted);
    EXPECT_FALSE(cameras.GetActiveCamera().IsValid());
    EXPECT_EQ(runtime.PublishPendingDestroyCleanup(), 1u);
    EXPECT_EQ(runtime.AdvanceRetirements(), 1u);
    EXPECT_EQ(runtime.RecycleRecyclableEntities(), 1u);
    EXPECT_FALSE(runtime.GetRegistry().IsAlive(camera.entity));
    EXPECT_FALSE(cameras.MarkCut(camera));

    const ECS::EntityHandle recycled = runtime.CreateEntity();
    ASSERT_TRUE(recycled.IsValid());
    EXPECT_EQ(recycled.GetIndex(), camera.entity.GetIndex());
    EXPECT_NE(recycled.GetGeneration(), camera.entity.GetGeneration());
}
} // namespace
