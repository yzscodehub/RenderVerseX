#include "Scene/ECS/SceneEcsRuntime.h"

#include <gtest/gtest.h>

TEST(EcsChangeTrackingValidation, RuntimeTransformsUseRegistryWriteVersionsAndSnapshots)
{
    RVX::SceneECS::SceneEcsRuntime runtime;
    const RVX::ECS::EntityHandle entity = runtime.CreateEntity();
    ASSERT_TRUE(entity.IsValid());

    const RVX::SceneECS::SceneEcsDiagnosticsSnapshot before = runtime.GetDiagnosticsSnapshot();
    RVX::SceneECS::LocalTransform transform;
    transform.translation = {4.0f, 0.0f, 0.0f};
    ASSERT_TRUE(runtime.SetLocalTransform(entity, transform));
    EXPECT_GT(runtime.GetDiagnosticsSnapshot().localTransformWriteVersion,
              before.localTransformWriteVersion);

    runtime.ResolveSimulationTransforms();
    const RVX::SceneECS::SceneEcsDiagnosticsSnapshot resolved = runtime.GetDiagnosticsSnapshot();
    EXPECT_GT(resolved.simulationWorldTransformWriteVersion,
              before.simulationWorldTransformWriteVersion);
    EXPECT_EQ(runtime.SynchronizeRenderWorldTransforms(), 1u);
    EXPECT_GT(runtime.GetDiagnosticsSnapshot().renderWorldTransformWriteVersion,
              before.renderWorldTransformWriteVersion);

    transform.translation.x = 9.0f;
    ASSERT_TRUE(runtime.SetLocalTransform(entity, transform));
    runtime.ResolveSimulationTransforms();
    const auto* simulation =
        runtime.GetRegistry().TryGet<RVX::SceneECS::SimulationWorldTransform>(entity);
    ASSERT_NE(simulation, nullptr);
    EXPECT_NEAR(simulation->matrix[3].x, 9.0f, 0.0001f);
}
