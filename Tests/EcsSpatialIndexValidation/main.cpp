#include "Scene/ECS/SceneEcsRuntime.h"

#include <gtest/gtest.h>

namespace
{
    RVX::SceneECS::RuntimeEntityDesc MakeEntity(float x, RVX::uint32 layer)
    {
        RVX::SceneECS::RuntimeEntityDesc desc;
        desc.localTransform.translation = {x, 0.0f, 0.0f};
        desc.bounds.center = {0.0f, 0.0f, 0.0f};
        desc.bounds.extents = {0.5f, 0.5f, 0.5f};
        desc.layer.value = layer;
        return desc;
    }
} // namespace

TEST(EcsSpatialIndexValidation, SynchronizeBuildsAHandleOnlyDerivedView)
{
    RVX::SceneECS::SceneEcsRuntime runtime;
    const auto second = runtime.CreateEntity(MakeEntity(4.0f, 1));
    const auto first = runtime.CreateEntity(MakeEntity(0.0f, 0));
    ASSERT_TRUE(second.IsValid());
    ASSERT_TRUE(first.IsValid());
    const RVX::SceneECS::SceneEcsTickResult tick = runtime.Tick();
    ASSERT_TRUE(tick.succeeded);
    const auto initial = tick.spatial;
    EXPECT_EQ(initial.inserted, 2u);
    EXPECT_EQ(initial.indexed, 2u);

    const RVX::AABB all({-1.0f, -1.0f, -1.0f}, {5.0f, 1.0f, 1.0f});
    const RVX::SceneECS::SceneSpatialIndex& spatial = runtime.GetSpatialIndex();
    const auto results = spatial.QueryAabb(all);
    ASSERT_EQ(results.size(), 2u);
    EXPECT_LT(results[0].id.entity, results[1].id.entity);
    EXPECT_EQ(results[0].id.sceneRuntimeId, runtime.GetSceneRuntimeId());
    EXPECT_TRUE(results[0].id.IsValid());

    const auto layerOne = spatial.QueryAabb(all, 1u << 1u);
    ASSERT_EQ(layerOne.size(), 1u);
    EXPECT_EQ(layerOne[0].id.entity, second);
}

TEST(EcsSpatialIndexValidation, DisabledMovedAndDestroyedEntitiesNeverLeaveStaleEntries)
{
    RVX::SceneECS::SceneEcsRuntime runtime;
    const auto entity = runtime.CreateEntity(MakeEntity(0.0f, 0));
    ASSERT_TRUE(entity.IsValid());
    ASSERT_TRUE(runtime.Tick().succeeded);
    const RVX::SceneECS::SceneSpatialIndex& spatial = runtime.GetSpatialIndex();
    ASSERT_EQ(spatial.GetEntryCount(), 1u);
    EXPECT_EQ(spatial.QueryPoint({0.0f, 0.0f, 0.0f}).size(), 1u);

    ASSERT_TRUE(runtime.SetLocalTransform(
        entity,
        RVX::SceneECS::LocalTransform{.translation = {10.0f, 0.0f, 0.0f}}));
    const RVX::SceneECS::SceneEcsTickResult movedTick = runtime.Tick();
    ASSERT_TRUE(movedTick.succeeded);
    EXPECT_EQ(movedTick.spatial.updated, 1u);
    EXPECT_TRUE(spatial.QueryPoint({0.0f, 0.0f, 0.0f}).empty());
    EXPECT_EQ(spatial.QueryPoint({10.0f, 0.0f, 0.0f}).size(), 1u);

    ASSERT_TRUE(runtime.SetActive(entity, false));
    const RVX::SceneECS::SceneEcsTickResult disabledTick = runtime.Tick();
    ASSERT_TRUE(disabledTick.succeeded);
    const auto disabled = disabledTick.spatial;
    EXPECT_EQ(disabled.removed, 1u);
    EXPECT_EQ(disabled.indexed, 0u);

    ASSERT_TRUE(runtime.SetActive(entity, true));
    const RVX::SceneECS::SceneEcsTickResult reenabledTick = runtime.Tick();
    ASSERT_TRUE(reenabledTick.succeeded);
    EXPECT_EQ(reenabledTick.spatial.inserted, 1u);
    ASSERT_EQ(runtime.RequestDestroy(entity, RVX::SceneECS::ToCleanupDomainMask(
                                               RVX::SceneECS::CleanupDomain::None)),
              RVX::SceneECS::DestroyRequestResult::Accepted);
    const RVX::SceneECS::SceneEcsTickResult destroyedTick = runtime.Tick();
    ASSERT_TRUE(destroyedTick.succeeded);
    EXPECT_EQ(destroyedTick.spatial.removed, 1u);
    EXPECT_EQ(spatial.GetEntryCount(), 0u);
}
