#include "Scene/ECS/SceneEcsRuntime.h"

#include <gtest/gtest.h>

namespace
{
    RVX::SceneECS::LocalTransform MakeTranslation(float x)
    {
        RVX::SceneECS::LocalTransform transform;
        transform.translation = {x, 0.0f, 0.0f};
        return transform;
    }
} // namespace

TEST(EcsTransformHierarchyValidation, ReparentIsHandleOnlyAndRejectsCycles)
{
    RVX::SceneECS::SceneEcsRuntime runtime;
    const RVX::ECS::EntityHandle parent = runtime.CreateEntity();
    const RVX::ECS::EntityHandle child = runtime.CreateEntity();
    ASSERT_TRUE(parent.IsValid());
    ASSERT_TRUE(child.IsValid());

    ASSERT_TRUE(runtime.SetLocalTransform(parent, MakeTranslation(10.0f)));
    ASSERT_TRUE(runtime.SetLocalTransform(child, MakeTranslation(3.0f)));
    EXPECT_EQ(runtime.Reparent(
                  child, parent, RVX::SceneECS::ReparentMode::KeepLocal),
              RVX::SceneECS::ReparentResult::Applied);

    const auto* childWorld = runtime.GetRegistry().TryGet<RVX::SceneECS::SimulationWorldTransform>(child);
    ASSERT_NE(childWorld, nullptr);
    EXPECT_NEAR(childWorld->matrix[3].x, 13.0f, 0.0001f);
    EXPECT_EQ(runtime.GetTransformHierarchy().GetParent(child), parent);
    EXPECT_EQ(runtime.Reparent(
                  parent, child, RVX::SceneECS::ReparentMode::KeepLocal),
              RVX::SceneECS::ReparentResult::Cycle);
}

TEST(EcsTransformHierarchyValidation, PreviousAndKeepWorldTransformsAreDeterministic)
{
    RVX::SceneECS::SceneEcsRuntime runtime;
    const RVX::ECS::EntityHandle firstParent = runtime.CreateEntity();
    const RVX::ECS::EntityHandle secondParent = runtime.CreateEntity();
    const RVX::ECS::EntityHandle child = runtime.CreateEntity();
    ASSERT_TRUE(firstParent.IsValid());
    ASSERT_TRUE(secondParent.IsValid());
    ASSERT_TRUE(child.IsValid());

    ASSERT_TRUE(runtime.SetLocalTransform(firstParent, MakeTranslation(10.0f)));
    ASSERT_TRUE(runtime.SetLocalTransform(secondParent, MakeTranslation(100.0f)));
    ASSERT_TRUE(runtime.SetLocalTransform(child, MakeTranslation(3.0f)));
    ASSERT_EQ(runtime.Reparent(child, firstParent, RVX::SceneECS::ReparentMode::KeepLocal),
              RVX::SceneECS::ReparentResult::Applied);

    runtime.BeginSimulationFrame();
    ASSERT_TRUE(runtime.SetLocalTransform(firstParent, MakeTranslation(20.0f)));
    runtime.ResolveSimulationTransforms();

    const auto* childWorld = runtime.GetRegistry().TryGet<RVX::SceneECS::SimulationWorldTransform>(child);
    const auto* previousWorld =
        runtime.GetRegistry().TryGet<RVX::SceneECS::PreviousSimulationWorldTransform>(child);
    ASSERT_NE(childWorld, nullptr);
    ASSERT_NE(previousWorld, nullptr);
    EXPECT_NEAR(childWorld->matrix[3].x, 23.0f, 0.0001f);
    EXPECT_NEAR(previousWorld->matrix[3].x, 13.0f, 0.0001f);

    ASSERT_EQ(runtime.Reparent(child, secondParent, RVX::SceneECS::ReparentMode::KeepWorld),
              RVX::SceneECS::ReparentResult::Applied);
    const auto* childLocal = runtime.GetRegistry().TryGet<RVX::SceneECS::LocalTransform>(child);
    childWorld = runtime.GetRegistry().TryGet<RVX::SceneECS::SimulationWorldTransform>(child);
    ASSERT_NE(childLocal, nullptr);
    ASSERT_NE(childWorld, nullptr);
    EXPECT_NEAR(childLocal->translation.x, -77.0f, 0.0001f);
    EXPECT_NEAR(childWorld->matrix[3].x, 23.0f, 0.0001f);

    ASSERT_EQ(runtime.Detach(child, RVX::SceneECS::ReparentMode::KeepWorld),
              RVX::SceneECS::ReparentResult::Applied);
    childLocal = runtime.GetRegistry().TryGet<RVX::SceneECS::LocalTransform>(child);
    ASSERT_NE(childLocal, nullptr);
    EXPECT_NEAR(childLocal->translation.x, 23.0f, 0.0001f);
    EXPECT_FALSE(runtime.GetTransformHierarchy().GetParent(child).IsValid());
}
