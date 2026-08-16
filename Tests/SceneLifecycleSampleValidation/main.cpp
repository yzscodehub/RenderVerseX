#include "Scenes/SceneLifecycleSample.h"

#include "Scene/ECS/RenderFragments.h"
#include "Scene/ECS/SceneEcsRuntime.h"
#include "World/ECS/WorldEcsCameraService.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string_view>

namespace RVX
{
    namespace
    {
        [[nodiscard]] bool ContainsCode(const auto& values, std::string_view code)
        {
            return std::any_of(values.begin(), values.end(),
                               [code](const auto& value)
                               {
                                   return value.code.GetValue() == code;
                               });
        }

        [[nodiscard]] SceneECS::LocalTransform MakeTranslation(float32 x)
        {
            SceneECS::LocalTransform transform;
            transform.translation = {x, 0.0f, 0.0f};
            return transform;
        }
    } // namespace

    TEST(SceneLifecycleSampleValidation, DeclaresTheCataloguedWaterBottleScenario)
    {
        SceneLifecycleSample sample;
        const SampleInfo& info = sample.GetInfo();

        EXPECT_EQ(info.id, "scene-lifecycle");
        EXPECT_EQ(info.defaultAssetId, "water-bottle");
        EXPECT_EQ(info.assetPolicy, SampleAssetPolicy::Fixed);
        EXPECT_EQ(info.environmentPolicy, SampleEnvironmentPolicy::None);
        EXPECT_TRUE(info.supportsRenderPathSelection);
    }

    TEST(SceneLifecycleSampleValidation, DeclaresThePureEcsLifecycleContract)
    {
        SceneLifecycleSample sample;
        const SampleAssessmentContract contract = sample.GetAssessmentContract();

        EXPECT_TRUE(contract.IsValid());
        EXPECT_EQ(contract.code.GetValue(), "SAMPLE.SCENE_LIFECYCLE");
        EXPECT_EQ(contract.revision, "3");
        EXPECT_EQ(contract.actions.size(), 8u);
        EXPECT_TRUE(ContainsCode(
            contract.actions, "SCENE.ACTION.DEFERRED_ENTITY_SPAWN"));
        EXPECT_TRUE(ContainsCode(
            contract.actions, "SCENE.ACTION.REPARENT_KEEP_LOCAL"));
        EXPECT_TRUE(ContainsCode(
            contract.actions, "SCENE.ACTION.REPARENT_KEEP_WORLD"));
        EXPECT_TRUE(ContainsCode(
            contract.actions, "SCENE.ACTION.CAMERA_ENTITY_GENERATION_REUSE"));
        EXPECT_TRUE(ContainsCode(
            contract.actions, "SCENE.ACTION.CAMERA_SWITCH_AND_CUT"));
        EXPECT_TRUE(ContainsCode(
            contract.actions, "SCENE.ACTION.LIGHT_FRAGMENT_MUTATION"));
        EXPECT_TRUE(ContainsCode(
            contract.actions, "SCENE.ACTION.MATERIAL_SLOT_MUTATION"));
        EXPECT_TRUE(ContainsCode(
            contract.actions, "SCENE.ACTION.DESTROY_ENTITY_SLOT_REUSE"));
        EXPECT_TRUE(ContainsCode(
            contract.invariants, "SCENE.HIERARCHY.AUTHORITY_UNIFIED"));
        EXPECT_TRUE(ContainsCode(
            contract.invariants, "SCENE.HANDLE.STALE_REJECTED"));
        EXPECT_TRUE(ContainsCode(
            contract.invariants, "ENGINE.CAMERA.CUT_ADVANCED"));
        EXPECT_TRUE(ContainsCode(
            contract.invariants, "SCENE.LIGHT.WRITE_VERSION_ADVANCED"));
        EXPECT_TRUE(ContainsCode(
            contract.invariants, "SCENE.MATERIAL.SLOT_WRITE_VERSION_ADVANCED"));
        EXPECT_TRUE(ContainsCode(
            contract.invariants, "SCENE.CLEANUP.RETIREMENT_RECYCLED"));
        EXPECT_TRUE(contract.capabilities.empty());
    }

    TEST(SceneLifecycleSampleValidation,
         DeferredCommandReceiptsPublishSceneQualifiedEntityRefs)
    {
        SceneECS::SceneEcsRuntime runtime;
        SceneECS::SceneCommandBuffer commands = runtime.CreateCommandBuffer();
        SceneECS::RuntimeEntityDesc desc;
        desc.localTransform.translation = {2.0f, 0.0f, 0.0f};
        const SceneECS::SceneEntityReceipt created = commands.Create(desc);
        const SceneECS::SceneCommandReceipt lightAdded =
            commands.Add(created, SceneECS::Light{.type = SceneECS::LightType::Point});
        const SceneECS::SceneCommandBufferReceipt submitted =
            runtime.SubmitCommandBuffer(std::move(commands));

        EXPECT_TRUE(submitted.IsQueued());
        EXPECT_TRUE(created.IsQueued());
        EXPECT_TRUE(lightAdded.IsQueued());
        EXPECT_FALSE(created.GetEntity().IsValid());

        ASSERT_TRUE(runtime.Tick().succeeded);
        ASSERT_TRUE(submitted.IsApplied());
        ASSERT_TRUE(created.IsResolved());
        ASSERT_TRUE(lightAdded.IsApplied());

        const ECS::EntityHandle entity = created.GetEntity();
        const SceneECS::SceneEntityRef ref = runtime.GetEntityRef(entity);
        ASSERT_TRUE(ref.IsValid());
        EXPECT_EQ(ref.sceneRuntimeId, runtime.GetSceneRuntimeId());
        EXPECT_EQ(ref.entity, entity);
        EXPECT_NE(runtime.GetRegistry().TryGet<SceneECS::Light>(entity), nullptr);

        SceneECS::SceneEcsRuntime otherRuntime;
        const ECS::EntityHandle coincident = otherRuntime.CreateEntity();
        const SceneECS::SceneEntityRef otherRef = otherRuntime.GetEntityRef(coincident);
        ASSERT_TRUE(otherRef.IsValid());
        EXPECT_EQ(coincident, entity);
        EXPECT_NE(otherRef.sceneRuntimeId, ref.sceneRuntimeId);
    }

    TEST(SceneLifecycleSampleValidation,
         KeepLocalAndKeepWorldUseTheAuthoritativeParentRelation)
    {
        SceneECS::SceneEcsRuntime runtime;
        const ECS::EntityHandle firstParent = runtime.CreateEntity();
        const ECS::EntityHandle secondParent = runtime.CreateEntity();
        const ECS::EntityHandle child = runtime.CreateEntity();
        ASSERT_TRUE(runtime.SetLocalTransform(firstParent, MakeTranslation(10.0f)));
        ASSERT_TRUE(runtime.SetLocalTransform(secondParent, MakeTranslation(100.0f)));
        ASSERT_TRUE(runtime.SetLocalTransform(child, MakeTranslation(3.0f)));

        ASSERT_EQ(runtime.Reparent(
                      child, firstParent, SceneECS::ReparentMode::KeepLocal),
                  SceneECS::ReparentResult::Applied);
        const SceneECS::LocalTransform* local =
            runtime.GetRegistry().TryGet<SceneECS::LocalTransform>(child);
        const SceneECS::SimulationWorldTransform* world =
            runtime.GetRegistry().TryGet<SceneECS::SimulationWorldTransform>(child);
        ASSERT_NE(local, nullptr);
        ASSERT_NE(world, nullptr);
        EXPECT_NEAR(local->translation.x, 3.0f, 0.0001f);
        EXPECT_NEAR(world->matrix[3].x, 13.0f, 0.0001f);
        EXPECT_EQ(runtime.GetTransformHierarchy().GetParent(child), firstParent);

        ASSERT_EQ(runtime.Reparent(
                      child, secondParent, SceneECS::ReparentMode::KeepWorld),
                  SceneECS::ReparentResult::Applied);
        local = runtime.GetRegistry().TryGet<SceneECS::LocalTransform>(child);
        world = runtime.GetRegistry().TryGet<SceneECS::SimulationWorldTransform>(child);
        const SceneECS::ParentRelation* relation =
            runtime.GetRegistry().TryGet<SceneECS::ParentRelation>(child);
        ASSERT_NE(local, nullptr);
        ASSERT_NE(world, nullptr);
        ASSERT_NE(relation, nullptr);
        EXPECT_NEAR(local->translation.x, -87.0f, 0.0001f);
        EXPECT_NEAR(world->matrix[3].x, 13.0f, 0.0001f);
        EXPECT_EQ(relation->parent, secondParent);
        EXPECT_EQ(world->resolvedParent, secondParent);
    }

    TEST(SceneLifecycleSampleValidation,
         EcsCameraServiceSwitchesAndAdvancesThePrimaryCut)
    {
        SceneECS::SceneEcsRuntime runtime;
        WorldECS::WorldEcsCameraService cameras(runtime);
        const WorldECS::WorldEcsCameraRef primary = cameras.CreateMainCamera();
        const WorldECS::WorldEcsCameraRef secondary = cameras.CreateCamera();
        ASSERT_TRUE(primary.IsValid());
        ASSERT_TRUE(secondary.IsValid());

        const std::optional<SceneECS::Camera> before = cameras.GetCamera(primary);
        ASSERT_TRUE(before.has_value());
        ASSERT_TRUE(cameras.Activate(secondary));
        ASSERT_TRUE(runtime.Tick().succeeded);
        EXPECT_EQ(cameras.GetActiveCamera(), secondary);

        ASSERT_TRUE(cameras.MarkCut(primary));
        ASSERT_TRUE(cameras.Activate(primary));
        ASSERT_TRUE(runtime.Tick().succeeded);
        EXPECT_EQ(cameras.GetActiveCamera(), primary);
        const std::optional<SceneECS::Camera> after = cameras.GetCamera(primary);
        ASSERT_TRUE(after.has_value());
        EXPECT_GT(after->cutRevision, before->cutRevision);
    }

    TEST(SceneLifecycleSampleValidation,
         LightAndMaterialSlotsWritesAdvanceExactFragmentVersions)
    {
        SceneECS::SceneEcsRuntime runtime;
        const ECS::EntityHandle entity = runtime.CreateEntity();
        ASSERT_TRUE(entity.IsValid());
        ASSERT_TRUE(runtime.AddFragment<SceneECS::Light>(
            entity, {.type = SceneECS::LightType::Point, .intensity = 2.0f}));

        SceneECS::MaterialSlots slots;
        slots.count = 1;
        ASSERT_TRUE(runtime.AddFragment<SceneECS::MaterialSlots>(entity, slots));

        const uint64 lightBefore =
            runtime.GetRegistry().GetFragmentWriteVersion<SceneECS::Light>(entity);
        const uint64 slotsBefore =
            runtime.GetRegistry().GetFragmentWriteVersion<SceneECS::MaterialSlots>(entity);
        ASSERT_TRUE(runtime.SetFragment<SceneECS::Light>(
            entity, {.type = SceneECS::LightType::Point,
                     .color = {0.35f, 0.65f, 1.0f},
                     .intensity = 3.0f,
                     .range = 8.0f}));
        ASSERT_TRUE(runtime.SetFragment<SceneECS::MaterialSlots>(entity, slots));

        const uint64 lightAfter =
            runtime.GetRegistry().GetFragmentWriteVersion<SceneECS::Light>(entity);
        const uint64 slotsAfter =
            runtime.GetRegistry().GetFragmentWriteVersion<SceneECS::MaterialSlots>(entity);
        EXPECT_GT(lightAfter, lightBefore);
        EXPECT_GT(slotsAfter, slotsBefore);
        EXPECT_EQ(runtime.GetRegistry().TryGet<SceneECS::MaterialSlots>(entity)->count,
                  slots.count);
    }

    TEST(SceneLifecycleSampleValidation,
         PendingDestroyRecyclesTheGenerationBeforeEntitySlotReuse)
    {
        SceneECS::SceneEcsRuntime runtime;
        const ECS::EntityHandle destroyed = runtime.CreateEntity();
        const SceneECS::SceneEntityRef stale = runtime.GetEntityRef(destroyed);
        ASSERT_TRUE(stale.IsValid());

        ASSERT_EQ(runtime.RequestDestroy(
                      destroyed,
                      SceneECS::ToCleanupDomainMask(SceneECS::CleanupDomain::None)),
                  SceneECS::DestroyRequestResult::Accepted);
        EXPECT_EQ(runtime.GetDiagnosticsSnapshot().pendingDestroyCount, 1u);
        ASSERT_EQ(runtime.PublishPendingDestroyCleanup(), 1u);
        ASSERT_EQ(runtime.AdvanceRetirements(), 1u);
        ASSERT_EQ(runtime.RecycleRecyclableEntities(), 1u);
        EXPECT_FALSE(runtime.GetEntityRef(destroyed).IsValid());

        const ECS::EntityHandle replacement = runtime.CreateEntity();
        const SceneECS::SceneEntityRef replacementRef =
            runtime.GetEntityRef(replacement);
        ASSERT_TRUE(replacementRef.IsValid());
        EXPECT_EQ(replacement.GetIndex(), destroyed.GetIndex());
        EXPECT_NE(replacement.GetGeneration(), destroyed.GetGeneration());
        EXPECT_NE(replacementRef, stale);

        const SceneECS::SceneEcsDiagnosticsSnapshot diagnostics =
            runtime.GetDiagnosticsSnapshot();
        EXPECT_EQ(diagnostics.pendingDestroyCount, 0u);
        EXPECT_EQ(diagnostics.cleanupRequiredCount, 0u);
        EXPECT_EQ(diagnostics.retiringCount, 0u);
        EXPECT_EQ(diagnostics.recyclableCount, 0u);
    }
} // namespace RVX
