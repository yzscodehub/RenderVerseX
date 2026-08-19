#include "World/World.h"

#include <gtest/gtest.h>

namespace
{
using namespace RVX;

template<typename T>
concept HasLegacySceneAccessor = requires(T& value)
{
    value.GetScene();
};

template<typename T>
concept HasLegacyTick = requires(T& value)
{
    value.Tick(0.0f);
};

template<typename T>
concept HasLegacySubsystems = requires(T& value)
{
    value.GetSubsystems();
};

static_assert(!HasLegacySceneAccessor<World>);
static_assert(!HasLegacyTick<World>);
static_assert(!HasLegacySubsystems<World>);

TEST(WorldEcsAuthorityValidation, OwnsOneRuntimeAndAServiceBoundToThatExactRuntime)
{
    WorldConfig config;
    config.name = "Authority";
    World world(config);

    EXPECT_EQ(world.GetName(), "Authority");
    EXPECT_EQ(world.GetCameraService().GetSceneRuntimeId(),
              world.GetSceneEcsRuntime().GetSceneRuntimeId());

    const WorldECS::WorldEcsCameraRef camera = world.GetCameraService().CreateMainCamera();
    ASSERT_TRUE(camera.IsValid());
    ASSERT_TRUE(world.GetSceneEcsRuntime().Tick({.variableDeltaSeconds = 1.0 / 60.0}).succeeded);

    const WorldECS::WorldEcsCameraRef active = world.GetCameraService().GetActiveCamera();
    EXPECT_EQ(active, camera);
    EXPECT_EQ(active.sceneRuntimeId, world.GetSceneEcsRuntime().GetSceneRuntimeId());
}
} // namespace
