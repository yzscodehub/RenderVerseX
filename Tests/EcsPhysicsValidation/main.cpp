#include "PhysicsSceneAdapters/ECS/PhysicsEcsBridge.h"
#include "Scene/ECS/AnimationFragments.h"

#include <gtest/gtest.h>

#include <limits>

namespace
{
    using namespace RVX;

    SceneECS::LocalTransform MakeTransform(float x, float y, float z)
    {
        SceneECS::LocalTransform transform;
        transform.translation = {x, y, z};
        return transform;
    }

    SceneECS::Collider MakeBoxCollider(float extent = 0.5f)
    {
        SceneECS::Collider collider;
        collider.shape = SceneECS::ColliderShapeType::Box;
        collider.boxHalfExtents = {extent, extent, extent};
        return collider;
    }

    ECS::EntityHandle AddBody(SceneECS::SceneEcsRuntime& runtime,
                              SceneECS::RigidBodyMotionType motionType,
                              const SceneECS::LocalTransform& transform,
                              SceneECS::Collider collider = MakeBoxCollider())
    {
        const ECS::EntityHandle entity = runtime.CreateEntity({.localTransform = transform});
        if (!entity.IsValid())
        {
            return ECS::EntityHandle::Invalid();
        }

        SceneECS::RigidBody body;
        body.motionType = motionType;
        body.linearDamping = 0.0f;
        body.angularDamping = 0.0f;
        body.allowSleep = false;
        if (!runtime.AddFragment<SceneECS::RigidBody>(entity, body) ||
            !runtime.AddFragment<SceneECS::Collider>(entity, collider) ||
            !runtime.AddFragment<SceneECS::PhysicsBodyState>(entity))
        {
            return ECS::EntityHandle::Invalid();
        }
        return entity;
    }

    SceneECS::SceneEcsTickResult TickFixed(SceneECS::SceneEcsRuntime& runtime)
    {
        return runtime.Tick({
            .variableDeltaSeconds = 1.0 / 60.0,
            .fixedDeltaSeconds = 1.0 / 60.0,
            .fixedStepCount = 1,
        });
    }

    void InitializeBuiltInWorld(Physics::PhysicsWorld& world)
    {
        Physics::PhysicsWorldConfig config;
        config.backend = Physics::PhysicsBackendType::BuiltIn;
        config.fixedTimeStep = 1.0f / 60.0f;
        config.maxSubSteps = 1;
        EXPECT_TRUE(world.Initialize(config));
    }

    SceneECS::RootMotionIntent MakeRootMotionIntent(
        const SceneECS::SceneEcsRuntime& runtime,
        ECS::EntityHandle entity,
        Physics::BodyHandle body,
        uint64 sourcePoseSequence,
        uint64 fixedStepSequence,
        Vec3 translation = {1.0f, 0.0f, 0.0f},
        uint64 rootMotionSequence = 0)
    {
        SceneECS::RootMotionIntent intent;
        intent.sceneRuntimeIdValue = runtime.GetSceneRuntimeId().GetValue();
        intent.targetEntity = entity;
        intent.physicsBodyHandlePacked = body.GetPackedValue();
        intent.sourcePoseSequence = sourcePoseSequence;
        intent.rootMotionSequence =
            rootMotionSequence != 0 ? rootMotionSequence : sourcePoseSequence;
        intent.fixedStepSequence = fixedStepSequence;
        intent.translationDelta = translation;
        intent.rotationDelta = {1.0f, 0.0f, 0.0f, 0.0f};
        intent.pending = true;
        return intent;
    }

    bool SetRootMotionIntent(SceneECS::SceneEcsRuntime& runtime,
                             ECS::EntityHandle entity,
                             const SceneECS::RootMotionIntent& intent)
    {
        if (runtime.GetRegistry().Has<SceneECS::RootMotionIntent>(entity))
        {
            return runtime.SetFragment(entity, intent);
        }
        return runtime.AddFragment<SceneECS::RootMotionIntent>(entity, intent);
    }

    void ExpectExactRootMotionIntent(const SceneECS::RootMotionIntent& actual,
                                     const SceneECS::RootMotionIntent& expected)
    {
        EXPECT_EQ(actual.sceneRuntimeIdValue, expected.sceneRuntimeIdValue);
        EXPECT_EQ(actual.targetEntity, expected.targetEntity);
        EXPECT_EQ(actual.physicsBodyHandlePacked, expected.physicsBodyHandlePacked);
        EXPECT_EQ(actual.sourcePoseSequence, expected.sourcePoseSequence);
        EXPECT_EQ(actual.rootMotionSequence, expected.rootMotionSequence);
        EXPECT_EQ(actual.fixedStepSequence, expected.fixedStepSequence);
        EXPECT_FLOAT_EQ(actual.translationDelta.x, expected.translationDelta.x);
        EXPECT_FLOAT_EQ(actual.translationDelta.y, expected.translationDelta.y);
        EXPECT_FLOAT_EQ(actual.translationDelta.z, expected.translationDelta.z);
        EXPECT_FLOAT_EQ(actual.rotationDelta.w, expected.rotationDelta.w);
        EXPECT_FLOAT_EQ(actual.rotationDelta.x, expected.rotationDelta.x);
        EXPECT_FLOAT_EQ(actual.rotationDelta.y, expected.rotationDelta.y);
        EXPECT_FLOAT_EQ(actual.rotationDelta.z, expected.rotationDelta.z);
        EXPECT_EQ(actual.pending, expected.pending);
        EXPECT_EQ(actual.sequenceRejected, expected.sequenceRejected);
    }
} // namespace

TEST(EcsPhysicsValidation, DynamicGravityWritesBackWorldPoseInFixedSequence)
{
    SceneECS::SceneEcsRuntime runtime;
    Physics::PhysicsWorld physicsWorld;
    InitializeBuiltInWorld(physicsWorld);
    PhysicsSceneAdapters::PhysicsEcsBridge bridge(runtime, physicsWorld);
    ASSERT_EQ(bridge.RegisterProcessors(),
              PhysicsSceneAdapters::PhysicsEcsBridgeRegistrationResult::Registered);

    const ECS::EntityHandle entity = AddBody(
        runtime, SceneECS::RigidBodyMotionType::Dynamic, MakeTransform(0.0f, 10.0f, 0.0f));
    ASSERT_TRUE(entity.IsValid());

    const SceneECS::SceneEcsTickResult tick = TickFixed(runtime);
    ASSERT_TRUE(tick.succeeded);
    const Physics::BodyHandle body = bridge.FindBody(runtime.GetSceneRuntimeId(), entity);
    ASSERT_TRUE(body.IsValid());
    const std::optional<Physics::BodyState> state = physicsWorld.ReadBodyState(body);
    ASSERT_TRUE(state.has_value());
    EXPECT_LT(state->pose.position.y, 10.0f);

    const auto* transform = runtime.GetRegistry().TryGet<SceneECS::SimulationWorldTransform>(entity);
    const auto* bodyState = runtime.GetRegistry().TryGet<SceneECS::PhysicsBodyState>(entity);
    ASSERT_NE(transform, nullptr);
    ASSERT_NE(bodyState, nullptr);
    EXPECT_NEAR(transform->matrix[3].y, state->pose.position.y, 0.0001f);
    EXPECT_EQ(bodyState->status, SceneECS::PhysicsBodyBindingStatus::Active);
    EXPECT_EQ(bodyState->lastSynchronizedFixedStep, 1u);
    EXPECT_EQ(physicsWorld.GetRuntimeDiagnosticsSnapshot().fixedStepSequence, 1u);
}

TEST(EcsPhysicsValidation, DynamicWritebackConvertsWorldPoseThroughParentLocalSpace)
{
    SceneECS::SceneEcsRuntime runtime;
    Physics::PhysicsWorld physicsWorld;
    InitializeBuiltInWorld(physicsWorld);
    PhysicsSceneAdapters::PhysicsEcsBridge bridge(runtime, physicsWorld);
    ASSERT_EQ(bridge.RegisterProcessors(),
              PhysicsSceneAdapters::PhysicsEcsBridgeRegistrationResult::Registered);

    const ECS::EntityHandle parent = runtime.CreateEntity({.localTransform = MakeTransform(10.0f, 0.0f, 0.0f)});
    const ECS::EntityHandle child = AddBody(
        runtime, SceneECS::RigidBodyMotionType::Dynamic, MakeTransform(2.0f, 5.0f, 0.0f));
    ASSERT_TRUE(parent.IsValid());
    ASSERT_TRUE(child.IsValid());
    ASSERT_EQ(runtime.Reparent(child, parent, SceneECS::ReparentMode::KeepLocal),
              SceneECS::ReparentResult::Applied);

    ASSERT_TRUE(TickFixed(runtime).succeeded);
    const Physics::BodyHandle body = bridge.FindBody(runtime.GetSceneRuntimeId(), child);
    const std::optional<Physics::BodyState> state = physicsWorld.ReadBodyState(body);
    ASSERT_TRUE(state.has_value());
    const auto* local = runtime.GetRegistry().TryGet<SceneECS::LocalTransform>(child);
    const auto* world = runtime.GetRegistry().TryGet<SceneECS::SimulationWorldTransform>(child);
    ASSERT_NE(local, nullptr);
    ASSERT_NE(world, nullptr);
    EXPECT_NEAR(local->translation.x, 2.0f, 0.0001f);
    EXPECT_NEAR(local->translation.y, state->pose.position.y, 0.0001f);
    EXPECT_NEAR(world->matrix[3].x, state->pose.position.x, 0.0001f);
    EXPECT_NEAR(world->matrix[3].y, state->pose.position.y, 0.0001f);
}

TEST(EcsPhysicsValidation, KinematicUsesPreResolvedTransformAndColliderReplaceIsAtomic)
{
    SceneECS::SceneEcsRuntime runtime;
    Physics::PhysicsWorld physicsWorld;
    InitializeBuiltInWorld(physicsWorld);
    PhysicsSceneAdapters::PhysicsEcsBridge bridge(runtime, physicsWorld);
    ASSERT_EQ(bridge.RegisterProcessors(),
              PhysicsSceneAdapters::PhysicsEcsBridgeRegistrationResult::Registered);

    const ECS::EntityHandle entity = AddBody(
        runtime, SceneECS::RigidBodyMotionType::Kinematic, MakeTransform(0.0f, 2.0f, 0.0f));
    ASSERT_TRUE(entity.IsValid());
    ASSERT_TRUE(TickFixed(runtime).succeeded);

    ASSERT_TRUE(runtime.SetLocalTransform(entity, MakeTransform(3.0f, 9.0f, -2.0f)));
    SceneECS::Collider sphere;
    sphere.shape = SceneECS::ColliderShapeType::Sphere;
    sphere.sphereRadius = 1.25f;
    SceneECS::SceneCommandBuffer commands = runtime.CreateCommandBuffer();
    ASSERT_TRUE(commands.Remove<SceneECS::Collider>(entity).IsQueued());
    ASSERT_TRUE(commands.Add<SceneECS::Collider>(entity, sphere).IsQueued());
    ASSERT_TRUE(runtime.SubmitCommandBuffer(
                            std::move(commands), SceneECS::SceneCommandBarrier::BeforeFixedStep)
                            .IsQueued());

    ASSERT_TRUE(TickFixed(runtime).succeeded);
    const Physics::BodyHandle body = bridge.FindBody(runtime.GetSceneRuntimeId(), entity);
    ASSERT_TRUE(body.IsValid());
    const std::optional<Physics::BodyState> state = physicsWorld.ReadBodyState(body);
    ASSERT_TRUE(state.has_value());
    EXPECT_NEAR(state->pose.position.x, 3.0f, 0.0001f);
    EXPECT_NEAR(state->pose.position.y, 9.0f, 0.0001f);
    EXPECT_NEAR(state->pose.position.z, -2.0f, 0.0001f);
    EXPECT_EQ(physicsWorld.GetBodyCount(), 1u);
    EXPECT_EQ(physicsWorld.GetColliderCount(), 1u);
    EXPECT_EQ(physicsWorld.GetRuntimeDiagnosticsSnapshot().colliderRebuildCount, 1u);
}

TEST(EcsPhysicsValidation, EnableRemoveDestroyAndRecycleLeaveNoGhostBody)
{
    SceneECS::SceneEcsRuntime runtime;
    Physics::PhysicsWorld physicsWorld;
    InitializeBuiltInWorld(physicsWorld);
    PhysicsSceneAdapters::PhysicsEcsBridge bridge(runtime, physicsWorld);
    ASSERT_EQ(bridge.RegisterProcessors(),
              PhysicsSceneAdapters::PhysicsEcsBridgeRegistrationResult::Registered);

    const ECS::EntityHandle entity = AddBody(
        runtime, SceneECS::RigidBodyMotionType::Dynamic, MakeTransform(0.0f, 3.0f, 0.0f));
    ASSERT_TRUE(TickFixed(runtime).succeeded);
    ASSERT_EQ(physicsWorld.GetBodyCount(), 1u);

    ASSERT_TRUE(runtime.SetFragmentEnabled<SceneECS::Collider>(entity, false));
    ASSERT_TRUE(runtime.Tick().succeeded);
    EXPECT_EQ(physicsWorld.GetBodyCount(), 0u);
    EXPECT_FALSE(bridge.FindBody(runtime.GetSceneRuntimeId(), entity).IsValid());

    ASSERT_TRUE(runtime.SetFragmentEnabled<SceneECS::Collider>(entity, true));
    ASSERT_TRUE(TickFixed(runtime).succeeded);
    ASSERT_EQ(physicsWorld.GetBodyCount(), 1u);

    ASSERT_TRUE(runtime.RemoveFragment<SceneECS::Collider>(entity));
    ASSERT_TRUE(runtime.Tick().succeeded);
    EXPECT_EQ(physicsWorld.GetBodyCount(), 0u);

    ASSERT_TRUE(runtime.AddFragment<SceneECS::Collider>(entity, MakeBoxCollider()));
    ASSERT_TRUE(TickFixed(runtime).succeeded);
    ASSERT_EQ(physicsWorld.GetBodyCount(), 1u);

    ASSERT_EQ(runtime.RequestDestroy(
                  entity,
                  SceneECS::ToCleanupDomainMask(SceneECS::CleanupDomain::Physics)),
              SceneECS::DestroyRequestResult::Accepted);
    ASSERT_TRUE(runtime.Tick().succeeded);
    EXPECT_EQ(physicsWorld.GetBodyCount(), 0u);
    EXPECT_FALSE(runtime.GetRegistry().IsAlive(entity));
    EXPECT_FALSE(bridge.FindBody(runtime.GetSceneRuntimeId(), entity).IsValid());

    const ECS::EntityHandle recycled = runtime.CreateEntity();
    ASSERT_TRUE(recycled.IsValid());
    EXPECT_EQ(recycled.GetIndex(), entity.GetIndex());
    EXPECT_NE(recycled.GetGeneration(), entity.GetGeneration());
}

TEST(EcsPhysicsValidation, CleanupHistoryLossRebuildsFromLifecycleAndRejectsForeignReferences)
{
    SceneECS::SceneEcsRuntime runtime(1);
    Physics::PhysicsWorld physicsWorld;
    InitializeBuiltInWorld(physicsWorld);
    PhysicsSceneAdapters::PhysicsEcsBridge bridge(runtime, physicsWorld);
    ASSERT_EQ(bridge.RegisterProcessors(),
              PhysicsSceneAdapters::PhysicsEcsBridgeRegistrationResult::Registered);

    const ECS::EntityHandle first = AddBody(
        runtime, SceneECS::RigidBodyMotionType::Dynamic, MakeTransform(0.0f, 3.0f, 0.0f));
    const ECS::EntityHandle second = AddBody(
        runtime, SceneECS::RigidBodyMotionType::Static, MakeTransform(4.0f, 0.0f, 0.0f));
    ASSERT_TRUE(TickFixed(runtime).succeeded);
    ASSERT_EQ(physicsWorld.GetBodyCount(), 2u);
    const auto activeDiagnostics = bridge.GetDiagnosticsSnapshot();
    ASSERT_EQ(activeDiagnostics.bindings.size(), 2u);
    EXPECT_LT(activeDiagnostics.bindings[0].entity, activeDiagnostics.bindings[1].entity);

    EXPECT_FALSE(bridge.FindBody(ECS::SceneRuntimeId(999), first).IsValid());
    ASSERT_EQ(runtime.RequestDestroy(
                  first,
                  SceneECS::ToCleanupDomainMask(SceneECS::CleanupDomain::Physics)),
              SceneECS::DestroyRequestResult::Accepted);
    ASSERT_EQ(runtime.RequestDestroy(
                  second,
                  SceneECS::ToCleanupDomainMask(SceneECS::CleanupDomain::Physics)),
              SceneECS::DestroyRequestResult::Accepted);
    ASSERT_EQ(runtime.PublishPendingDestroyCleanup(), 2u);

    ASSERT_TRUE(runtime.Tick().succeeded);
    const auto diagnostics = bridge.GetDiagnosticsSnapshot();
    EXPECT_EQ(diagnostics.cleanupContinuityLossCount, 1u);
    EXPECT_TRUE(diagnostics.bindings.empty());
    EXPECT_EQ(physicsWorld.GetBodyCount(), 0u);
    EXPECT_FALSE(runtime.GetRegistry().IsAlive(first));
    EXPECT_FALSE(runtime.GetRegistry().IsAlive(second));
}

TEST(EcsPhysicsValidation, StructuralHistoryLossRebuildsIncludeDisabledBindings)
{
    // Two retained entries are insufficient for entity creation plus the three
    // physics fragments. The bridge must treat the lost cursor as an
    // authoritative, include-disabled reconcile rather than accepting a
    // partial journal suffix.
    SceneECS::SceneEcsRuntime runtime(8192, 2);
    Physics::PhysicsWorld physicsWorld;
    InitializeBuiltInWorld(physicsWorld);
    PhysicsSceneAdapters::PhysicsEcsBridge bridge(runtime, physicsWorld);
    ASSERT_EQ(bridge.RegisterProcessors(),
              PhysicsSceneAdapters::PhysicsEcsBridgeRegistrationResult::Registered);

    const ECS::EntityHandle entity = AddBody(
        runtime, SceneECS::RigidBodyMotionType::Static, MakeTransform(2.0f, 0.0f, 0.0f));
    ASSERT_TRUE(entity.IsValid());
    ASSERT_TRUE(TickFixed(runtime).succeeded);

    const auto diagnostics = bridge.GetDiagnosticsSnapshot();
    EXPECT_EQ(diagnostics.structuralContinuityLossCount, 1u);
    EXPECT_GE(diagnostics.authoritativeReconcileCount, 1u);
    EXPECT_EQ(diagnostics.activeBodyCount, 1u);
    ASSERT_EQ(diagnostics.bindings.size(), 1u);
    EXPECT_EQ(diagnostics.bindings.front().entity, entity);
}

TEST(EcsPhysicsValidation, ValueOnlyBodyLifecycleRejectsStaleHandles)
{
    Physics::PhysicsWorld physicsWorld;
    InitializeBuiltInWorld(physicsWorld);

    Physics::RigidBodyDesc desc;
    desc.type = Physics::BodyType::Kinematic;
    desc.position = {1.0f, 2.0f, 3.0f};
    const Physics::BodyCreateResult created = physicsWorld.CreateBodyValue(desc);
    ASSERT_TRUE(created.Succeeded());
    ASSERT_TRUE(physicsWorld.ReadBodyState(created.handle).has_value());
    EXPECT_TRUE(physicsWorld.WriteBodyPose(
        created.handle, {.position = {4.0f, 5.0f, 6.0f}, .rotation = {1.0f, 0.0f, 0.0f, 0.0f}}));
    EXPECT_EQ(physicsWorld.DestroyBodyValue(created.handle), Physics::BodyDestroyStatus::Destroyed);
    EXPECT_EQ(physicsWorld.DestroyBodyValue(created.handle), Physics::BodyDestroyStatus::AlreadyAbsent);
    EXPECT_FALSE(physicsWorld.ReadBodyState(created.handle).has_value());

    const Physics::BodyCreateResult replacement = physicsWorld.CreateBodyValue(desc);
    ASSERT_TRUE(replacement.Succeeded());
    EXPECT_NE(replacement.handle, created.handle);
    EXPECT_FALSE(physicsWorld.WriteBodyPose(
        created.handle, {.position = {}, .rotation = {1.0f, 0.0f, 0.0f, 0.0f}}));
}

TEST(EcsPhysicsValidation, BridgeDestructionReleasesItsOwnedBodies)
{
    SceneECS::SceneEcsRuntime runtime;
    Physics::PhysicsWorld physicsWorld;
    InitializeBuiltInWorld(physicsWorld);
    const ECS::EntityHandle entity = AddBody(
        runtime, SceneECS::RigidBodyMotionType::Static, MakeTransform(0.0f, 0.0f, 0.0f));
    ASSERT_TRUE(entity.IsValid());

    {
        PhysicsSceneAdapters::PhysicsEcsBridge bridge(runtime, physicsWorld);
        ASSERT_EQ(bridge.RegisterProcessors(),
                  PhysicsSceneAdapters::PhysicsEcsBridgeRegistrationResult::Registered);
        ASSERT_TRUE(TickFixed(runtime).succeeded);
        ASSERT_EQ(physicsWorld.GetBodyCount(), 1u);
    }

    EXPECT_EQ(physicsWorld.GetBodyCount(), 0u);
}

TEST(EcsPhysicsValidation, MeshAndConvexNeverFallbackAndJoltIsExplicitlyUnsupported)
{
    SceneECS::SceneEcsRuntime runtime;
    Physics::PhysicsWorld physicsWorld;
    InitializeBuiltInWorld(physicsWorld);
    PhysicsSceneAdapters::PhysicsEcsBridge bridge(runtime, physicsWorld);
    ASSERT_EQ(bridge.RegisterProcessors(),
              PhysicsSceneAdapters::PhysicsEcsBridgeRegistrationResult::Registered);

    SceneECS::Collider mesh;
    mesh.shape = SceneECS::ColliderShapeType::Mesh;
    const ECS::EntityHandle entity = AddBody(
        runtime, SceneECS::RigidBodyMotionType::Static, MakeTransform(0.0f, 0.0f, 0.0f), mesh);
    ASSERT_TRUE(runtime.Tick().succeeded);
    EXPECT_EQ(physicsWorld.GetBodyCount(), 0u);
    const auto* state = runtime.GetRegistry().TryGet<SceneECS::PhysicsBodyState>(entity);
    ASSERT_NE(state, nullptr);
    EXPECT_EQ(state->status, SceneECS::PhysicsBodyBindingStatus::UnsupportedCollider);

    Physics::PhysicsWorld joltRequested;
    Physics::PhysicsWorldConfig joltConfig;
    joltConfig.backend = Physics::PhysicsBackendType::Jolt;
    ASSERT_TRUE(joltRequested.Initialize(joltConfig));
    SceneECS::SceneEcsRuntime joltRuntime;
    PhysicsSceneAdapters::PhysicsEcsBridge joltBridge(joltRuntime, joltRequested);
    EXPECT_EQ(joltBridge.RegisterProcessors(),
              PhysicsSceneAdapters::PhysicsEcsBridgeRegistrationResult::UnsupportedBackend);
}

TEST(EcsPhysicsValidation, InvalidPhysicsValuesFailClosedBeforeBodyCreation)
{
    SceneECS::SceneEcsRuntime runtime;
    Physics::PhysicsWorld physicsWorld;
    InitializeBuiltInWorld(physicsWorld);
    PhysicsSceneAdapters::PhysicsEcsBridge bridge(runtime, physicsWorld);
    ASSERT_EQ(bridge.RegisterProcessors(),
              PhysicsSceneAdapters::PhysicsEcsBridgeRegistrationResult::Registered);

    const ECS::EntityHandle invalidMotion = AddBody(
        runtime, SceneECS::RigidBodyMotionType::Dynamic, MakeTransform(0.0f, 1.0f, 0.0f));
    SceneECS::RigidBody invalidRigidBody;
    invalidRigidBody.motionType = static_cast<SceneECS::RigidBodyMotionType>(255u);
    ASSERT_TRUE(runtime.SetFragment(invalidMotion, invalidRigidBody));

    SceneECS::Collider invalidCollider = MakeBoxCollider();
    invalidCollider.shape = static_cast<SceneECS::ColliderShapeType>(255u);
    const ECS::EntityHandle invalidShape = AddBody(
        runtime, SceneECS::RigidBodyMotionType::Static, MakeTransform(1.0f, 0.0f, 0.0f),
        invalidCollider);

    SceneECS::LocalTransform nonFiniteTransform = MakeTransform(2.0f, 0.0f, 0.0f);
    nonFiniteTransform.translation.x = std::numeric_limits<float>::quiet_NaN();
    const ECS::EntityHandle invalidTransform = AddBody(
        runtime, SceneECS::RigidBodyMotionType::Kinematic, nonFiniteTransform);

    const ECS::EntityHandle invalidVelocity = AddBody(
        runtime, SceneECS::RigidBodyMotionType::Dynamic, MakeTransform(3.0f, 0.0f, 0.0f));
    SceneECS::PhysicsBodyState invalidBodyState;
    invalidBodyState.linearVelocity.x = std::numeric_limits<float>::infinity();
    ASSERT_TRUE(runtime.SetFragment(invalidVelocity, invalidBodyState));

    const ECS::EntityHandle invalidConfiguration = AddBody(
        runtime, SceneECS::RigidBodyMotionType::Dynamic, MakeTransform(4.0f, 0.0f, 0.0f));
    SceneECS::RigidBody nonFiniteConfiguration;
    nonFiniteConfiguration.linearDamping = std::numeric_limits<float>::quiet_NaN();
    ASSERT_TRUE(runtime.SetFragment(invalidConfiguration, nonFiniteConfiguration));

    ASSERT_TRUE(TickFixed(runtime).succeeded);
    EXPECT_EQ(physicsWorld.GetBodyCount(), 0u);
    for (const ECS::EntityHandle entity :
         {invalidMotion, invalidShape, invalidTransform, invalidVelocity, invalidConfiguration})
    {
        const auto* state = runtime.GetRegistry().TryGet<SceneECS::PhysicsBodyState>(entity);
        ASSERT_NE(state, nullptr);
        EXPECT_EQ(state->status, SceneECS::PhysicsBodyBindingStatus::InvalidConfiguration);
        EXPECT_FALSE(bridge.FindBody(runtime.GetSceneRuntimeId(), entity).IsValid());
    }
}

TEST(EcsPhysicsValidation, DynamicBodyWithNonInvertibleParentNeverBindsOrSimulates)
{
    SceneECS::SceneEcsRuntime runtime;
    Physics::PhysicsWorld physicsWorld;
    InitializeBuiltInWorld(physicsWorld);
    PhysicsSceneAdapters::PhysicsEcsBridge bridge(runtime, physicsWorld);
    ASSERT_EQ(bridge.RegisterProcessors(),
              PhysicsSceneAdapters::PhysicsEcsBridgeRegistrationResult::Registered);

    const ECS::EntityHandle parent = runtime.CreateEntity({.localTransform = MakeTransform(4.0f, 0.0f, 0.0f)});
    SceneECS::LocalTransform singularParent = MakeTransform(4.0f, 0.0f, 0.0f);
    singularParent.scale = {0.0f, 1.0f, 1.0f};
    ASSERT_TRUE(runtime.SetLocalTransform(parent, singularParent));

    const ECS::EntityHandle child = AddBody(
        runtime, SceneECS::RigidBodyMotionType::Dynamic, MakeTransform(1.0f, 3.0f, 0.0f));
    ASSERT_EQ(runtime.Reparent(child, parent, SceneECS::ReparentMode::KeepLocal),
              SceneECS::ReparentResult::Applied);

    ASSERT_TRUE(TickFixed(runtime).succeeded);
    ASSERT_TRUE(TickFixed(runtime).succeeded);
    EXPECT_EQ(physicsWorld.GetBodyCount(), 0u);
    EXPECT_FALSE(bridge.FindBody(runtime.GetSceneRuntimeId(), child).IsValid());
    const auto* state = runtime.GetRegistry().TryGet<SceneECS::PhysicsBodyState>(child);
    ASSERT_NE(state, nullptr);
    EXPECT_EQ(state->status, SceneECS::PhysicsBodyBindingStatus::InvalidHierarchy);
}

TEST(EcsPhysicsValidation, CleanupAcknowledgesNeverBoundInvalidBodyFromDurableLifecycleEvidence)
{
    SceneECS::SceneEcsRuntime runtime;
    Physics::PhysicsWorld physicsWorld;
    InitializeBuiltInWorld(physicsWorld);
    PhysicsSceneAdapters::PhysicsEcsBridge bridge(runtime, physicsWorld);
    ASSERT_EQ(bridge.RegisterProcessors(),
              PhysicsSceneAdapters::PhysicsEcsBridgeRegistrationResult::Registered);

    SceneECS::Collider mesh;
    mesh.shape = SceneECS::ColliderShapeType::Mesh;
    const ECS::EntityHandle entity = AddBody(
        runtime, SceneECS::RigidBodyMotionType::Static, MakeTransform(0.0f, 0.0f, 0.0f), mesh);
    ASSERT_TRUE(TickFixed(runtime).succeeded);
    ASSERT_EQ(physicsWorld.GetBodyCount(), 0u);

    ASSERT_EQ(runtime.RequestDestroy(
                  entity,
                  SceneECS::ToCleanupDomainMask(SceneECS::CleanupDomain::Physics)),
              SceneECS::DestroyRequestResult::Accepted);
    ASSERT_EQ(runtime.PublishPendingDestroyCleanup(), 1u);
    ASSERT_TRUE(runtime.Tick().succeeded);
    EXPECT_FALSE(runtime.GetRegistry().IsAlive(entity));
    EXPECT_EQ(bridge.GetDiagnosticsSnapshot().pendingCleanupCount, 0u);
}

TEST(EcsPhysicsValidation, RootMotionConsumesExactFixedIntentOnceAndUpdatesKinematicBody)
{
    SceneECS::SceneEcsRuntime runtime;
    Physics::PhysicsWorld physicsWorld;
    InitializeBuiltInWorld(physicsWorld);
    PhysicsSceneAdapters::PhysicsEcsBridge bridge(runtime, physicsWorld);
    ASSERT_EQ(bridge.RegisterProcessors(),
              PhysicsSceneAdapters::PhysicsEcsBridgeRegistrationResult::Registered);
    const ECS::EntityHandle entity = AddBody(
        runtime, SceneECS::RigidBodyMotionType::Kinematic, MakeTransform(0.0f, 2.0f, 0.0f));
    ASSERT_TRUE(TickFixed(runtime).succeeded);
    const Physics::BodyHandle body = bridge.FindBody(runtime.GetSceneRuntimeId(), entity);
    ASSERT_TRUE(body.IsValid());

    ASSERT_TRUE(SetRootMotionIntent(
        runtime, entity, MakeRootMotionIntent(runtime, entity, body, 1, 2, {2.0f, 0.0f, 0.0f})));
    ASSERT_TRUE(TickFixed(runtime).succeeded);
    const std::optional<Physics::BodyState> applied = physicsWorld.ReadBodyState(body);
    ASSERT_TRUE(applied.has_value());
    EXPECT_NEAR(applied->pose.position.x, 2.0f, 0.0001f);
    const auto* intent = runtime.GetRegistry().TryGet<SceneECS::RootMotionIntent>(entity);
    ASSERT_NE(intent, nullptr);
    EXPECT_FALSE(intent->pending);
    const auto firstDiagnostics = bridge.GetDiagnosticsSnapshot();
    EXPECT_EQ(firstDiagnostics.rootMotionAppliedCount, 1u);
    ASSERT_EQ(firstDiagnostics.bindings.size(), 1u);
    EXPECT_EQ(firstDiagnostics.bindings.front().lastConsumedRootMotionSequence, 1u);
    EXPECT_EQ(firstDiagnostics.bindings.front().lastConsumedRootMotionFixedStep, 2u);

    ASSERT_TRUE(TickFixed(runtime).succeeded);
    const std::optional<Physics::BodyState> replayed = physicsWorld.ReadBodyState(body);
    ASSERT_TRUE(replayed.has_value());
    EXPECT_NEAR(replayed->pose.position.x, 2.0f, 0.0001f);
    EXPECT_EQ(bridge.GetDiagnosticsSnapshot().rootMotionAppliedCount, 1u);
}

TEST(EcsPhysicsValidation, RootMotionReplayAndGapAreRejectedWithoutAdvancingConsumption)
{
    SceneECS::SceneEcsRuntime runtime;
    Physics::PhysicsWorld physicsWorld;
    InitializeBuiltInWorld(physicsWorld);
    PhysicsSceneAdapters::PhysicsEcsBridge bridge(runtime, physicsWorld);
    ASSERT_EQ(bridge.RegisterProcessors(),
              PhysicsSceneAdapters::PhysicsEcsBridgeRegistrationResult::Registered);
    const ECS::EntityHandle entity = AddBody(
        runtime, SceneECS::RigidBodyMotionType::Kinematic, MakeTransform(0.0f, 0.0f, 0.0f));
    ASSERT_TRUE(TickFixed(runtime).succeeded);
    const Physics::BodyHandle body = bridge.FindBody(runtime.GetSceneRuntimeId(), entity);
    ASSERT_TRUE(body.IsValid());

    ASSERT_TRUE(SetRootMotionIntent(runtime, entity,
                                     MakeRootMotionIntent(runtime, entity, body, 1, 2)));
    ASSERT_TRUE(TickFixed(runtime).succeeded);
    ASSERT_TRUE(SetRootMotionIntent(runtime, entity,
                                     MakeRootMotionIntent(runtime, entity, body, 1, 3)));
    ASSERT_TRUE(TickFixed(runtime).succeeded);
    const auto* replay = runtime.GetRegistry().TryGet<SceneECS::RootMotionIntent>(entity);
    ASSERT_NE(replay, nullptr);
    EXPECT_TRUE(replay->pending);

    ASSERT_TRUE(SetRootMotionIntent(runtime, entity,
                                     MakeRootMotionIntent(runtime, entity, body, 3, 4)));
    ASSERT_TRUE(TickFixed(runtime).succeeded);
    const auto* gap = runtime.GetRegistry().TryGet<SceneECS::RootMotionIntent>(entity);
    ASSERT_NE(gap, nullptr);
    EXPECT_TRUE(gap->pending);
    const auto diagnostics = bridge.GetDiagnosticsSnapshot();
    ASSERT_EQ(diagnostics.bindings.size(), 1u);
    EXPECT_EQ(diagnostics.bindings.front().lastConsumedRootMotionSequence, 1u);
    EXPECT_EQ(diagnostics.rootMotionAppliedCount, 1u);
    EXPECT_EQ(diagnostics.rootMotionReplayCount, 1u);
    EXPECT_EQ(diagnostics.rootMotionGapCount, 1u);
}

TEST(EcsPhysicsValidation, FirstIntentEstablishesExactBodyEpochThenReplayAndGapStayStrict)
{
    SceneECS::SceneEcsRuntime runtime;
    Physics::PhysicsWorld physicsWorld;
    InitializeBuiltInWorld(physicsWorld);
    PhysicsSceneAdapters::PhysicsEcsBridge bridge(runtime, physicsWorld);
    ASSERT_EQ(bridge.RegisterProcessors(),
              PhysicsSceneAdapters::PhysicsEcsBridgeRegistrationResult::Registered);
    const ECS::EntityHandle entity = AddBody(
        runtime, SceneECS::RigidBodyMotionType::Kinematic, MakeTransform(0.0f, 0.0f, 0.0f));
    ASSERT_TRUE(TickFixed(runtime).succeeded);
    const Physics::BodyHandle body = bridge.FindBody(runtime.GetSceneRuntimeId(), entity);
    ASSERT_TRUE(body.IsValid());

    // A paused initial-palette evaluation can commit pose N+1 before it emits
    // root-motion stream record N. The first stream record for this exact
    // generation-safe body is therefore a baseline, not a gap.
    ASSERT_TRUE(SetRootMotionIntent(runtime, entity,
                                     MakeRootMotionIntent(runtime, entity, body, 8, 2,
                                                          {1.0f, 0.0f, 0.0f}, 7)));
    ASSERT_TRUE(TickFixed(runtime).succeeded);
    const auto* firstIntent =
        runtime.GetRegistry().TryGet<SceneECS::RootMotionIntent>(entity);
    ASSERT_NE(firstIntent, nullptr);
    EXPECT_EQ(firstIntent->sourcePoseSequence, 8u);
    EXPECT_EQ(firstIntent->rootMotionSequence, 7u);
    EXPECT_FALSE(firstIntent->pending);
    auto diagnostics = bridge.GetDiagnosticsSnapshot();
    ASSERT_EQ(diagnostics.bindings.size(), 1u);
    EXPECT_EQ(diagnostics.bindings.front().lastConsumedRootMotionSequence, 7u);
    EXPECT_EQ(diagnostics.rootMotionAppliedCount, 1u);
    EXPECT_EQ(diagnostics.rootMotionReplayCount, 0u);
    EXPECT_EQ(diagnostics.rootMotionGapCount, 0u);

    ASSERT_TRUE(SetRootMotionIntent(runtime, entity,
                                     MakeRootMotionIntent(runtime, entity, body, 9, 3,
                                                          {1.0f, 0.0f, 0.0f}, 7)));
    ASSERT_TRUE(TickFixed(runtime).succeeded);
    ASSERT_TRUE(SetRootMotionIntent(runtime, entity,
                                     MakeRootMotionIntent(runtime, entity, body, 10, 4,
                                                          {1.0f, 0.0f, 0.0f}, 9)));
    ASSERT_TRUE(TickFixed(runtime).succeeded);
    diagnostics = bridge.GetDiagnosticsSnapshot();
    ASSERT_EQ(diagnostics.bindings.size(), 1u);
    EXPECT_EQ(diagnostics.bindings.front().lastConsumedRootMotionSequence, 7u);
    EXPECT_EQ(diagnostics.rootMotionReplayCount, 1u);
    EXPECT_EQ(diagnostics.rootMotionGapCount, 1u);

    ASSERT_TRUE(SetRootMotionIntent(runtime, entity,
                                     MakeRootMotionIntent(runtime, entity, body, 11, 5,
                                                          {1.0f, 0.0f, 0.0f}, 8)));
    ASSERT_TRUE(TickFixed(runtime).succeeded);
    diagnostics = bridge.GetDiagnosticsSnapshot();
    ASSERT_EQ(diagnostics.bindings.size(), 1u);
    EXPECT_EQ(diagnostics.bindings.front().lastConsumedRootMotionSequence, 8u);
    EXPECT_EQ(diagnostics.rootMotionAppliedCount, 2u);
    EXPECT_EQ(diagnostics.rootMotionReplayCount, 1u);
    EXPECT_EQ(diagnostics.rootMotionGapCount, 1u);
}

TEST(EcsPhysicsValidation, RootMotionRejectsStaleEntityBodySceneAndFixedStepBeforeApply)
{
    SceneECS::SceneEcsRuntime runtime;
    Physics::PhysicsWorld physicsWorld;
    InitializeBuiltInWorld(physicsWorld);
    PhysicsSceneAdapters::PhysicsEcsBridge bridge(runtime, physicsWorld);
    ASSERT_EQ(bridge.RegisterProcessors(),
              PhysicsSceneAdapters::PhysicsEcsBridgeRegistrationResult::Registered);
    const ECS::EntityHandle entity = AddBody(
        runtime, SceneECS::RigidBodyMotionType::Kinematic, MakeTransform(0.0f, 0.0f, 0.0f));
    ASSERT_TRUE(TickFixed(runtime).succeeded);
    const Physics::BodyHandle body = bridge.FindBody(runtime.GetSceneRuntimeId(), entity);
    ASSERT_TRUE(body.IsValid());

    SceneECS::RootMotionIntent staleEntity = MakeRootMotionIntent(runtime, entity, body, 1, 2);
    staleEntity.targetEntity = ECS::EntityHandle::Create(
        entity.GetIndex(), entity.GetGeneration() + 1u);
    ASSERT_TRUE(SetRootMotionIntent(runtime, entity, staleEntity));
    ASSERT_TRUE(TickFixed(runtime).succeeded);

    SceneECS::RootMotionIntent staleBody = MakeRootMotionIntent(runtime, entity, body, 1, 3);
    ++staleBody.physicsBodyHandlePacked;
    ASSERT_TRUE(SetRootMotionIntent(runtime, entity, staleBody));
    ASSERT_TRUE(TickFixed(runtime).succeeded);

    SceneECS::RootMotionIntent wrongScene = MakeRootMotionIntent(runtime, entity, body, 1, 4);
    ++wrongScene.sceneRuntimeIdValue;
    ASSERT_TRUE(SetRootMotionIntent(runtime, entity, wrongScene));
    ASSERT_TRUE(TickFixed(runtime).succeeded);

    SceneECS::RootMotionIntent wrongStep = MakeRootMotionIntent(runtime, entity, body, 1, 99);
    ASSERT_TRUE(SetRootMotionIntent(runtime, entity, wrongStep));
    ASSERT_TRUE(TickFixed(runtime).succeeded);

    ASSERT_TRUE(SetRootMotionIntent(runtime, entity,
                                     MakeRootMotionIntent(runtime, entity, body, 9, 6)));
    ASSERT_TRUE(TickFixed(runtime).succeeded);

    const std::optional<Physics::BodyState> applied = physicsWorld.ReadBodyState(body);
    ASSERT_TRUE(applied.has_value());
    EXPECT_NEAR(applied->pose.position.x, 1.0f, 0.0001f);
    const auto diagnostics = bridge.GetDiagnosticsSnapshot();
    ASSERT_EQ(diagnostics.bindings.size(), 1u);
    EXPECT_EQ(diagnostics.bindings.front().lastConsumedRootMotionSequence, 9u);
    EXPECT_EQ(diagnostics.rootMotionAppliedCount, 1u);
    EXPECT_EQ(diagnostics.rootMotionRejectedCount, 4u);
}

TEST(EcsPhysicsValidation, RootMotionFailedApplyPreservesExactRetryAcrossFixedSteps)
{
    SceneECS::SceneEcsRuntime runtime;
    Physics::PhysicsWorld physicsWorld;
    InitializeBuiltInWorld(physicsWorld);
    PhysicsSceneAdapters::PhysicsEcsBridge bridge(runtime, physicsWorld);
    ASSERT_EQ(bridge.RegisterProcessors(),
              PhysicsSceneAdapters::PhysicsEcsBridgeRegistrationResult::Registered);
    const ECS::EntityHandle entity = AddBody(
        runtime, SceneECS::RigidBodyMotionType::Kinematic, MakeTransform(0.0f, 0.0f, 0.0f));
    ASSERT_TRUE(TickFixed(runtime).succeeded);
    const Physics::BodyHandle body = bridge.FindBody(runtime.GetSceneRuntimeId(), entity);
    ASSERT_TRUE(body.IsValid());

    const ECS::EntityHandle singularParent = runtime.CreateEntity();
    ASSERT_TRUE(singularParent.IsValid());
    SceneECS::LocalTransform singular = MakeTransform(0.0f, 0.0f, 0.0f);
    singular.scale = {0.0f, 1.0f, 1.0f};
    ASSERT_TRUE(runtime.SetLocalTransform(singularParent, singular));
    ASSERT_EQ(runtime.Reparent(entity, singularParent, SceneECS::ReparentMode::KeepLocal),
              SceneECS::ReparentResult::Applied);
    ASSERT_TRUE(SetRootMotionIntent(runtime, entity,
                                     MakeRootMotionIntent(runtime, entity, body, 8, 2,
                                                          {1.0f, 0.0f, 0.0f}, 7)));
    ASSERT_TRUE(TickFixed(runtime).succeeded);
    const auto* retained = runtime.GetRegistry().TryGet<SceneECS::RootMotionIntent>(entity);
    ASSERT_NE(retained, nullptr);
    EXPECT_TRUE(retained->pending);
    EXPECT_FALSE(retained->sequenceRejected);
    EXPECT_EQ(retained->sceneRuntimeIdValue, runtime.GetSceneRuntimeId().GetValue());
    EXPECT_EQ(retained->targetEntity, entity);
    EXPECT_EQ(retained->physicsBodyHandlePacked, body.GetPackedValue());
    EXPECT_EQ(retained->sourcePoseSequence, 8u);
    EXPECT_EQ(retained->rootMotionSequence, 7u);
    EXPECT_EQ(retained->fixedStepSequence, 2u);
    EXPECT_FLOAT_EQ(retained->translationDelta.x, 1.0f);
    EXPECT_FLOAT_EQ(retained->translationDelta.y, 0.0f);
    EXPECT_FLOAT_EQ(retained->translationDelta.z, 0.0f);
    EXPECT_FLOAT_EQ(retained->rotationDelta.w, 1.0f);
    EXPECT_FLOAT_EQ(retained->rotationDelta.x, 0.0f);
    EXPECT_FLOAT_EQ(retained->rotationDelta.y, 0.0f);
    EXPECT_FLOAT_EQ(retained->rotationDelta.z, 0.0f);
    EXPECT_EQ(bridge.GetDiagnosticsSnapshot().bindings.front().lastConsumedRootMotionSequence, 0u);

    // A different pose provenance cannot replace the locked first-epoch
    // retry, even when it claims the same root-motion stream record.
    ASSERT_TRUE(SetRootMotionIntent(runtime, entity,
                                     MakeRootMotionIntent(runtime, entity, body, 9, 3,
                                                          {1.0f, 0.0f, 0.0f}, 7)));
    ASSERT_TRUE(TickFixed(runtime).succeeded);
    const auto* skipped = runtime.GetRegistry().TryGet<SceneECS::RootMotionIntent>(entity);
    ASSERT_NE(skipped, nullptr);
    EXPECT_TRUE(skipped->pending);
    EXPECT_FALSE(skipped->sequenceRejected);
    EXPECT_EQ(skipped->sceneRuntimeIdValue, retained->sceneRuntimeIdValue);
    EXPECT_EQ(skipped->targetEntity, retained->targetEntity);
    EXPECT_EQ(skipped->physicsBodyHandlePacked, retained->physicsBodyHandlePacked);
    EXPECT_EQ(skipped->sourcePoseSequence, retained->sourcePoseSequence);
    EXPECT_EQ(skipped->rootMotionSequence, retained->rootMotionSequence);
    EXPECT_EQ(skipped->fixedStepSequence, retained->fixedStepSequence);
    EXPECT_FLOAT_EQ(skipped->translationDelta.x, retained->translationDelta.x);
    EXPECT_FLOAT_EQ(skipped->translationDelta.y, retained->translationDelta.y);
    EXPECT_FLOAT_EQ(skipped->translationDelta.z, retained->translationDelta.z);
    EXPECT_FLOAT_EQ(skipped->rotationDelta.w, retained->rotationDelta.w);
    EXPECT_FLOAT_EQ(skipped->rotationDelta.x, retained->rotationDelta.x);
    EXPECT_FLOAT_EQ(skipped->rotationDelta.y, retained->rotationDelta.y);
    EXPECT_FLOAT_EQ(skipped->rotationDelta.z, retained->rotationDelta.z);
    EXPECT_EQ(bridge.GetDiagnosticsSnapshot().bindings.front().lastConsumedRootMotionSequence, 0u);

    ASSERT_EQ(runtime.Detach(entity, SceneECS::ReparentMode::KeepLocal),
              SceneECS::ReparentResult::Applied);
    // No fragment mutation is needed to retry the old fixed-step record after
    // the parent singularity has been repaired.
    ASSERT_TRUE(TickFixed(runtime).succeeded);
    ASSERT_TRUE(SetRootMotionIntent(runtime, entity,
                                     MakeRootMotionIntent(runtime, entity, body, 9, 5,
                                                          {1.0f, 0.0f, 0.0f}, 8)));
    ASSERT_TRUE(TickFixed(runtime).succeeded);
    const auto diagnostics = bridge.GetDiagnosticsSnapshot();
    ASSERT_EQ(diagnostics.bindings.size(), 1u);
    EXPECT_EQ(diagnostics.bindings.front().lastConsumedRootMotionSequence, 8u);
    EXPECT_EQ(diagnostics.bindings.front().lastConsumedRootMotionFixedStep, 5u);
    EXPECT_EQ(diagnostics.rootMotionAppliedCount, 2u);
    EXPECT_EQ(diagnostics.rootMotionGapCount, 0u);
}

TEST(EcsPhysicsValidation, RootMotionPreClaimFailureRetainsExactRetryAcrossFixedSteps)
{
    SceneECS::SceneEcsRuntime runtime;
    Physics::PhysicsWorld physicsWorld;
    InitializeBuiltInWorld(physicsWorld);
    PhysicsSceneAdapters::PhysicsEcsBridge bridge(runtime, physicsWorld);
    ASSERT_EQ(bridge.RegisterProcessors(),
              PhysicsSceneAdapters::PhysicsEcsBridgeRegistrationResult::Registered);
    const ECS::EntityHandle entity = AddBody(
        runtime, SceneECS::RigidBodyMotionType::Kinematic, MakeTransform(0.0f, 0.0f, 0.0f));
    ASSERT_TRUE(TickFixed(runtime).succeeded);
    const Physics::BodyHandle body = bridge.FindBody(runtime.GetSceneRuntimeId(), entity);
    ASSERT_TRUE(body.IsValid());

    // Make the backend body temporarily disagree with its authoritative ECS
    // kinematic contract. This fails before claiming the root-motion fragment.
    const std::optional<Physics::BodyState> initialBodyState = physicsWorld.ReadBodyState(body);
    ASSERT_TRUE(initialBodyState.has_value());
    Physics::BodyConfiguration transientConfiguration = initialBodyState->configuration;
    transientConfiguration.type = Physics::BodyType::Dynamic;
    transientConfiguration.mass = 1.0f;
    ASSERT_TRUE(physicsWorld.WriteBodyConfiguration(body, transientConfiguration));

    const SceneECS::RootMotionIntent original =
        MakeRootMotionIntent(runtime, entity, body, 8, 2, {1.0f, 0.0f, 0.0f}, 7);
    ASSERT_TRUE(SetRootMotionIntent(runtime, entity, original));
    ASSERT_TRUE(TickFixed(runtime).succeeded);
    const auto* retained = runtime.GetRegistry().TryGet<SceneECS::RootMotionIntent>(entity);
    ASSERT_NE(retained, nullptr);
    ExpectExactRootMotionIntent(*retained, original);
    EXPECT_EQ(bridge.GetDiagnosticsSnapshot().bindings.front().lastConsumedRootMotionSequence, 0u);

    // A changed pose provenance with the same stream record cannot replace the
    // retry. A later stream record is a gap and must be restored as well.
    ASSERT_TRUE(SetRootMotionIntent(runtime, entity,
                                     MakeRootMotionIntent(runtime, entity, body, 9, 3,
                                                          {1.0f, 0.0f, 0.0f}, 7)));
    ASSERT_TRUE(TickFixed(runtime).succeeded);
    const auto* changedProvenance =
        runtime.GetRegistry().TryGet<SceneECS::RootMotionIntent>(entity);
    ASSERT_NE(changedProvenance, nullptr);
    ExpectExactRootMotionIntent(*changedProvenance, original);

    ASSERT_TRUE(SetRootMotionIntent(runtime, entity,
                                     MakeRootMotionIntent(runtime, entity, body, 10, 4,
                                                          {1.0f, 0.0f, 0.0f}, 8)));
    ASSERT_TRUE(TickFixed(runtime).succeeded);
    const auto* advancedSource =
        runtime.GetRegistry().TryGet<SceneECS::RootMotionIntent>(entity);
    ASSERT_NE(advancedSource, nullptr);
    ExpectExactRootMotionIntent(*advancedSource, original);

    // Repair the transient backend condition without mutating the intent. The
    // original fixed-step record must now apply despite being older than this step.
    ASSERT_TRUE(physicsWorld.WriteBodyConfiguration(body, initialBodyState->configuration));
    ASSERT_TRUE(TickFixed(runtime).succeeded);
    const std::optional<Physics::BodyState> applied = physicsWorld.ReadBodyState(body);
    ASSERT_TRUE(applied.has_value());
    EXPECT_EQ(applied->configuration.type, Physics::BodyType::Kinematic);
    EXPECT_NEAR(applied->pose.position.x, 1.0f, 0.0001f);
    const auto diagnostics = bridge.GetDiagnosticsSnapshot();
    ASSERT_EQ(diagnostics.bindings.size(), 1u);
    EXPECT_EQ(diagnostics.bindings.front().lastConsumedRootMotionSequence, 7u);
    EXPECT_EQ(diagnostics.bindings.front().lastConsumedRootMotionFixedStep, 2u);
    EXPECT_EQ(diagnostics.rootMotionAppliedCount, 1u);
    EXPECT_EQ(diagnostics.rootMotionGapCount, 1u);
}

TEST(EcsPhysicsValidation, AtomicProcessorRegistrationRejectsConflictWithoutPartialBridgeOrRetryLatch)
{
    SceneECS::SceneEcsRuntime runtime;
    Physics::PhysicsWorld physicsWorld;
    InitializeBuiltInWorld(physicsWorld);
    const std::string conflictName = "PhysicsEcsBridge." +
                                     std::to_string(runtime.GetSceneRuntimeId().GetValue()) +
                                     ".PhysicsToScene";
    ASSERT_TRUE(runtime.RegisterProcessor({
        .name = conflictName,
        .phase = ECS::ProcessorPhase::PhysicsToScene,
        .stepMode = ECS::ProcessorStepMode::Fixed,
        .runWithContext = [](ECS::ProcessorExecutionContext&) {},
    }));

    PhysicsSceneAdapters::PhysicsEcsBridge conflictedBridge(runtime, physicsWorld);
    EXPECT_EQ(conflictedBridge.RegisterProcessors(),
              PhysicsSceneAdapters::PhysicsEcsBridgeRegistrationResult::ProcessorRegistrationFailed);
    // The batch rejection is retryable; it is not converted into a permanent
    // bridge-side latch even while this deliberately persistent conflict remains.
    EXPECT_EQ(conflictedBridge.RegisterProcessors(),
              PhysicsSceneAdapters::PhysicsEcsBridgeRegistrationResult::ProcessorRegistrationFailed);

    const ECS::EntityHandle entity = AddBody(
        runtime, SceneECS::RigidBodyMotionType::Dynamic, MakeTransform(0.0f, 2.0f, 0.0f));
    ASSERT_TRUE(TickFixed(runtime).succeeded);
    EXPECT_EQ(physicsWorld.GetBodyCount(), 0u);
    EXPECT_FALSE(conflictedBridge.FindBody(runtime.GetSceneRuntimeId(), entity).IsValid());

    SceneECS::SceneEcsRuntime retryRuntime;
    Physics::PhysicsWorld retryWorld;
    PhysicsSceneAdapters::PhysicsEcsBridge retryBridge(retryRuntime, retryWorld);
    EXPECT_EQ(retryBridge.RegisterProcessors(),
              PhysicsSceneAdapters::PhysicsEcsBridgeRegistrationResult::PhysicsWorldUnavailable);
    InitializeBuiltInWorld(retryWorld);
    EXPECT_EQ(retryBridge.RegisterProcessors(),
              PhysicsSceneAdapters::PhysicsEcsBridgeRegistrationResult::Registered);
}
