#include "Core/Log.h"
#include "Physics/Constraints/IConstraint.h"
#include "Physics/PhysicsWorld.h"
#include "Physics/Shapes/CollisionShape.h"
#include "Scene/Components/ColliderComponent.h"
#include "Scene/Components/RigidBodyComponent.h"
#include "World/PhysicsSubsystem.h"
#include "World/World.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace
{
    class LogEnvironment final : public ::testing::Environment
    {
    public:
        void SetUp() override
        {
            RVX::Log::Initialize();
        }

        void TearDown() override
        {
            RVX::Log::Shutdown();
        }
    };

    [[maybe_unused]] ::testing::Environment* g_logEnvironment =
        ::testing::AddGlobalTestEnvironment(new LogEnvironment());

    void ExpectNearVec3(const RVX::Vec3& actual, const RVX::Vec3& expected, float epsilon = 0.0001f)
    {
        EXPECT_NEAR(actual.x, expected.x, epsilon);
        EXPECT_NEAR(actual.y, expected.y, epsilon);
        EXPECT_NEAR(actual.z, expected.z, epsilon);
    }

    bool ContainsBodyHandle(const std::vector<RVX::Physics::BodyHandle>& handles,
                            RVX::Physics::BodyHandle expected)
    {
        return std::find(handles.begin(), handles.end(), expected) != handles.end();
    }

    class CountingConstraint final : public RVX::Physics::IConstraint
    {
    public:
        RVX::Physics::ConstraintType GetType() const override
        {
            return RVX::Physics::ConstraintType::Generic;
        }

        const char* GetTypeName() const override
        {
            return "CountingConstraint";
        }

        void SetBodies(RVX::Physics::RigidBody* bodyA, RVX::Physics::RigidBody* bodyB)
        {
            m_bodyA = bodyA;
            m_bodyB = bodyB;
        }

        void PreSolve(float deltaTime) override
        {
            ++preSolveCount;
            lastDeltaTime = deltaTime;
        }

        void SolveVelocity(float deltaTime) override
        {
            ++velocitySolveCount;
            lastDeltaTime = deltaTime;
        }

        void SolvePosition(float deltaTime) override
        {
            ++positionSolveCount;
            lastDeltaTime = deltaTime;
        }

        float GetAppliedImpulse() const override
        {
            return appliedImpulse;
        }

        int preSolveCount = 0;
        int velocitySolveCount = 0;
        int positionSolveCount = 0;
        float appliedImpulse = 0.0f;
        float lastDeltaTime = 0.0f;
    };

    RVX::Physics::BodyHandle CreateBoxBody(RVX::Physics::PhysicsWorld& physicsWorld,
                                           RVX::Physics::BodyType bodyType,
                                           const RVX::Vec3& position,
                                           bool isTrigger = false,
                                           RVX::Vec3 halfExtents = RVX::Vec3(1.0f))
    {
        RVX::Physics::RigidBodyDesc desc;
        desc.type = bodyType;
        desc.position = position;
        desc.gravityScale = 0.0f;
        desc.allowSleep = false;
        desc.isTrigger = isTrigger;

        RVX::Physics::BodyHandle handle = physicsWorld.CreateBody(desc);
        physicsWorld.AddShape(handle, RVX::Physics::BoxShape::Create(halfExtents));
        return handle;
    }
} // namespace

TEST(PhysicsWorldIntegrationValidation, DynamicRigidBodyRegistersStepsAndSyncsSceneTransform)
{
    RVX::WorldConfig config;
    config.name = "PhysicsIntegrationWorld";
    config.autoInitializeSpatial = false;
    config.autoInitializePhysics = true;

    RVX::World world;
    world.Initialize(config);

    auto* physicsSubsystem = world.GetPhysics();
    ASSERT_NE(nullptr, physicsSubsystem);
    ASSERT_NE(nullptr, physicsSubsystem->GetPhysicsWorld());
    EXPECT_EQ(0u, physicsSubsystem->GetPhysicsWorld()->GetBodyCount());

    RVX::ActorSpawnParams spawnParams;
    spawnParams.name = "FallingBody";
    spawnParams.localPosition = RVX::Vec3(0.0f, 10.0f, 0.0f);

    RVX::SceneEntity* entity = world.SpawnActor(spawnParams);
    ASSERT_NE(nullptr, entity);

    auto* rigidBody = entity->AddComponent<RVX::RigidBodyComponent>();
    ASSERT_NE(nullptr, rigidBody);
    rigidBody->SetBodyType(RVX::RigidBodyType::Dynamic);
    rigidBody->SetLinearDamping(0.0f);
    rigidBody->SetAngularDamping(0.0f);

    world.Tick(1.0f / 60.0f);

    ASSERT_NE(nullptr, rigidBody->GetBody());
    EXPECT_EQ(physicsSubsystem->GetPhysicsWorld(), rigidBody->GetPhysicsWorld());
    EXPECT_TRUE(rigidBody->IsRegisteredWithPhysicsWorld());
    EXPECT_EQ(1u, physicsSubsystem->GetPhysicsWorld()->GetBodyCount());
    EXPECT_EQ(1u, physicsSubsystem->GetLastRegisteredBodyComponentCount());
    EXPECT_LT(entity->GetWorldPosition().y, 10.0f);

    entity->SetPosition(RVX::Vec3(0.0f, 20.0f, 0.0f));
    world.Tick(1.0f / 60.0f);

    EXPECT_GT(entity->GetWorldPosition().y, 19.0f);
    EXPECT_LT(entity->GetWorldPosition().y, 20.0f);

    world.Shutdown();
}

TEST(PhysicsWorldIntegrationValidation, PendingForcesAndImpulsesApplyWhenBodyRegisters)
{
    RVX::WorldConfig config;
    config.name = "PendingPhysicsActionsWorld";
    config.autoInitializeSpatial = false;
    config.autoInitializePhysics = true;
    config.physics.gravity = RVX::Vec3(0.0f);
    config.physics.fixedTimeStep = 1.0f;
    config.physics.maxSubSteps = 1;

    RVX::World world;
    world.Initialize(config);

    RVX::ActorSpawnParams spawnParams;
    spawnParams.name = "PendingActionsBody";

    RVX::SceneEntity* entity = world.SpawnActor(spawnParams);
    ASSERT_NE(nullptr, entity);

    auto* rigidBody = entity->AddComponent<RVX::RigidBodyComponent>();
    ASSERT_NE(nullptr, rigidBody);
    ASSERT_EQ(nullptr, rigidBody->GetBody());

    rigidBody->SetBodyType(RVX::RigidBodyType::Dynamic);
    rigidBody->SetMass(2.0f);
    rigidBody->SetUseGravity(false);
    rigidBody->SetLinearDamping(0.0f);
    rigidBody->SetAngularDamping(0.0f);
    rigidBody->SetCanSleep(false);

    rigidBody->ApplyForceAtPoint(RVX::Vec3(4.0f, 0.0f, 0.0f), RVX::Vec3(0.0f, 0.0f, 1.0f));
    rigidBody->ApplyImpulseAtPoint(RVX::Vec3(0.0f, 2.0f, 0.0f), RVX::Vec3(1.0f, 0.0f, 0.0f));
    rigidBody->ApplyTorque(RVX::Vec3(0.0f, 0.0f, 6.0f));
    rigidBody->ApplyAngularImpulse(RVX::Vec3(0.0f, 0.0f, 1.0f));

    world.Tick(1.0f);

    auto body = rigidBody->GetBody();
    ASSERT_NE(nullptr, body);
    ExpectNearVec3(body->GetLinearVelocity(), RVX::Vec3(2.0f, 1.0f, 0.0f));
    ExpectNearVec3(body->GetAngularVelocity(), RVX::Vec3(0.0f, 2.0f, 6.0f));
    ExpectNearVec3(body->GetPosition(), RVX::Vec3(2.0f, 1.0f, 0.0f));
    ExpectNearVec3(entity->GetWorldPosition(), RVX::Vec3(2.0f, 1.0f, 0.0f));
    ExpectNearVec3(body->GetAccumulatedForce(), RVX::Vec3(0.0f));
    ExpectNearVec3(body->GetAccumulatedTorque(), RVX::Vec3(0.0f));

    world.Tick(1.0f);

    ExpectNearVec3(body->GetLinearVelocity(), RVX::Vec3(2.0f, 1.0f, 0.0f));
    ExpectNearVec3(body->GetAngularVelocity(), RVX::Vec3(0.0f, 2.0f, 6.0f));
    ExpectNearVec3(body->GetPosition(), RVX::Vec3(4.0f, 2.0f, 0.0f));

    world.Shutdown();
}

TEST(PhysicsWorldIntegrationValidation, BodyCreationRequiresInitializationAndRespectsMaxBodies)
{
    RVX::Physics::RigidBodyDesc desc;
    desc.type = RVX::Physics::BodyType::Dynamic;

    RVX::Physics::PhysicsWorld physicsWorld;
    EXPECT_FALSE(physicsWorld.CreateBody(desc).IsValid());
    EXPECT_EQ(0u, physicsWorld.GetBodyCount());

    RVX::Physics::PhysicsWorldConfig config;
    config.maxBodies = 1;
    ASSERT_TRUE(physicsWorld.Initialize(config));

    const RVX::Physics::BodyHandle firstBody = physicsWorld.CreateBody(desc);
    EXPECT_TRUE(firstBody.IsValid());
    EXPECT_EQ(1u, physicsWorld.GetBodyCount());

    const RVX::Physics::BodyHandle overflowBody = physicsWorld.CreateBody(desc);
    EXPECT_FALSE(overflowBody.IsValid());
    EXPECT_EQ(1u, physicsWorld.GetBodyCount());
    EXPECT_EQ(nullptr, physicsWorld.GetBody(overflowBody));

    physicsWorld.DestroyBody(firstBody);
    EXPECT_EQ(0u, physicsWorld.GetBodyCount());

    const RVX::Physics::BodyHandle replacementBody = physicsWorld.CreateBody(desc);
    EXPECT_TRUE(replacementBody.IsValid());
    EXPECT_EQ(1u, physicsWorld.GetBodyCount());

    physicsWorld.Shutdown();
}

TEST(PhysicsWorldIntegrationValidation, BackendSelectionFallsBackToBuiltInWhenJoltUnavailable)
{
    EXPECT_TRUE(RVX::Physics::PhysicsBackendFactory::IsAvailable(RVX::Physics::PhysicsBackendType::Auto));
    EXPECT_TRUE(RVX::Physics::PhysicsBackendFactory::IsAvailable(RVX::Physics::PhysicsBackendType::BuiltIn));
    EXPECT_FALSE(RVX::Physics::PhysicsBackendFactory::IsAvailable(RVX::Physics::PhysicsBackendType::Jolt));

    auto defaultBackend = RVX::Physics::PhysicsBackendFactory::Create(RVX::Physics::PhysicsBackendType::Auto);
    ASSERT_NE(nullptr, defaultBackend);
    EXPECT_EQ(RVX::Physics::PhysicsBackendType::BuiltIn, defaultBackend->GetType());
    auto joltBackend = RVX::Physics::PhysicsBackendFactory::Create(RVX::Physics::PhysicsBackendType::Jolt);
    EXPECT_EQ(nullptr, joltBackend.get());

    RVX::Physics::PhysicsWorldConfig config;
    config.backend = RVX::Physics::PhysicsBackendType::Jolt;
    config.fixedTimeStep = 1.0f;
    config.maxSubSteps = 1;

    RVX::Physics::PhysicsWorld physicsWorld;
    ASSERT_TRUE(physicsWorld.Initialize(config));
    EXPECT_EQ(RVX::Physics::PhysicsBackendType::Jolt, physicsWorld.GetRequestedBackendType());
    EXPECT_EQ(RVX::Physics::PhysicsBackendType::BuiltIn, physicsWorld.GetActiveBackendType());
    EXPECT_STREQ("Built-in", physicsWorld.GetActiveBackendName());
    EXPECT_TRUE(physicsWorld.IsBackendFallbackActive());

    RVX::Physics::RigidBodyDesc desc;
    desc.type = RVX::Physics::BodyType::Dynamic;
    desc.position = RVX::Vec3(0.0f, 10.0f, 0.0f);
    desc.linearDamping = 0.0f;
    desc.angularDamping = 0.0f;

    const RVX::Physics::BodyHandle bodyHandle = physicsWorld.CreateBody(desc);
    ASSERT_TRUE(bodyHandle.IsValid());
    physicsWorld.Step(1.0f);

    const RVX::Physics::RigidBody* body = physicsWorld.GetBody(bodyHandle);
    ASSERT_NE(nullptr, body);
    EXPECT_LT(body->GetPosition().y, 10.0f);
    EXPECT_LT(body->GetLinearVelocity().y, 0.0f);

    physicsWorld.Shutdown();
}

TEST(PhysicsWorldIntegrationValidation, AccumulatedForceAndTorqueAffectDynamicBodyStepOnce)
{
    RVX::Physics::PhysicsWorldConfig config;
    config.gravity = RVX::Vec3(0.0f);
    config.fixedTimeStep = 1.0f;
    config.maxSubSteps = 1;

    RVX::Physics::PhysicsWorld physicsWorld;
    ASSERT_TRUE(physicsWorld.Initialize(config));

    RVX::Physics::RigidBodyDesc desc;
    desc.type = RVX::Physics::BodyType::Dynamic;
    desc.mass = 2.0f;
    desc.linearDamping = 0.0f;
    desc.angularDamping = 0.0f;
    desc.gravityScale = 0.0f;
    desc.allowSleep = false;

    const RVX::Physics::BodyHandle bodyHandle = physicsWorld.CreateBody(desc);
    RVX::Physics::RigidBody* body = physicsWorld.GetBody(bodyHandle);
    ASSERT_NE(nullptr, body);

    body->ApplyForce(RVX::Vec3(4.0f, 0.0f, 0.0f));
    body->ApplyTorque(RVX::Vec3(0.0f, 6.0f, 0.0f));
    ExpectNearVec3(body->GetAccumulatedForce(), RVX::Vec3(4.0f, 0.0f, 0.0f));
    ExpectNearVec3(body->GetAccumulatedTorque(), RVX::Vec3(0.0f, 6.0f, 0.0f));

    physicsWorld.Step(1.0f);

    ExpectNearVec3(body->GetLinearVelocity(), RVX::Vec3(2.0f, 0.0f, 0.0f));
    ExpectNearVec3(body->GetAngularVelocity(), RVX::Vec3(0.0f, 3.0f, 0.0f));
    ExpectNearVec3(body->GetPosition(), RVX::Vec3(2.0f, 0.0f, 0.0f));
    ExpectNearVec3(body->GetAccumulatedForce(), RVX::Vec3(0.0f));
    ExpectNearVec3(body->GetAccumulatedTorque(), RVX::Vec3(0.0f));

    physicsWorld.Step(1.0f);

    ExpectNearVec3(body->GetLinearVelocity(), RVX::Vec3(2.0f, 0.0f, 0.0f));
    ExpectNearVec3(body->GetAngularVelocity(), RVX::Vec3(0.0f, 3.0f, 0.0f));
    ExpectNearVec3(body->GetPosition(), RVX::Vec3(4.0f, 0.0f, 0.0f));

    physicsWorld.Shutdown();
}

TEST(PhysicsWorldIntegrationValidation, ConstraintLifecycleAssignsIdsSolvesAndDestroys)
{
    RVX::Physics::PhysicsWorldConfig config;
    config.gravity = RVX::Vec3(0.0f);
    config.fixedTimeStep = 0.1f;
    config.maxSubSteps = 1;
    config.velocitySteps = 2;
    config.positionSteps = 3;

    RVX::Physics::PhysicsWorld physicsWorld;
    ASSERT_TRUE(physicsWorld.Initialize(config));

    EXPECT_EQ(0u, physicsWorld.CreateConstraint(std::shared_ptr<RVX::Physics::Constraint>{}));
    EXPECT_EQ(0u, physicsWorld.GetConstraintCount());

    auto constraint = std::make_shared<CountingConstraint>();
    const RVX::uint64 constraintId = physicsWorld.CreateConstraint(constraint);

    ASSERT_NE(0u, constraintId);
    EXPECT_EQ(constraintId, constraint->GetId());
    EXPECT_EQ(1u, physicsWorld.GetConstraintCount());

    physicsWorld.Step(0.1f);

    EXPECT_EQ(1, constraint->preSolveCount);
    EXPECT_EQ(config.velocitySteps, constraint->velocitySolveCount);
    EXPECT_EQ(config.positionSteps, constraint->positionSolveCount);
    EXPECT_NEAR(config.fixedTimeStep, constraint->lastDeltaTime, 0.0001f);

    const int preSolveCount = constraint->preSolveCount;
    const int velocitySolveCount = constraint->velocitySolveCount;
    const int positionSolveCount = constraint->positionSolveCount;

    physicsWorld.DestroyConstraint(constraintId);
    EXPECT_EQ(0u, constraint->GetId());
    EXPECT_EQ(0u, physicsWorld.GetConstraintCount());

    physicsWorld.Step(0.1f);

    EXPECT_EQ(preSolveCount, constraint->preSolveCount);
    EXPECT_EQ(velocitySolveCount, constraint->velocitySolveCount);
    EXPECT_EQ(positionSolveCount, constraint->positionSolveCount);

    physicsWorld.DestroyConstraint(constraintId);
    physicsWorld.DestroyConstraint(0);
    EXPECT_EQ(0u, physicsWorld.GetConstraintCount());

    physicsWorld.Shutdown();
}

TEST(PhysicsWorldIntegrationValidation, ConstraintBreakingStopsFutureSolves)
{
    RVX::Physics::PhysicsWorldConfig config;
    config.gravity = RVX::Vec3(0.0f);
    config.fixedTimeStep = 0.1f;
    config.maxSubSteps = 1;
    config.velocitySteps = 1;
    config.positionSteps = 1;

    RVX::Physics::PhysicsWorld physicsWorld;
    ASSERT_TRUE(physicsWorld.Initialize(config));

    auto constraint = std::make_shared<CountingConstraint>();
    constraint->SetBreakingForce(2.0f);
    constraint->appliedImpulse = 0.3f;

    const RVX::uint64 constraintId = physicsWorld.CreateConstraint(constraint);
    ASSERT_NE(0u, constraintId);

    physicsWorld.Step(0.1f);

    EXPECT_TRUE(constraint->IsBroken());
    EXPECT_EQ(1, constraint->preSolveCount);
    EXPECT_EQ(1, constraint->velocitySolveCount);
    EXPECT_EQ(1, constraint->positionSolveCount);

    const int preSolveCount = constraint->preSolveCount;
    const int velocitySolveCount = constraint->velocitySolveCount;
    const int positionSolveCount = constraint->positionSolveCount;

    physicsWorld.Step(0.1f);

    EXPECT_EQ(preSolveCount, constraint->preSolveCount);
    EXPECT_EQ(velocitySolveCount, constraint->velocitySolveCount);
    EXPECT_EQ(positionSolveCount, constraint->positionSolveCount);

    physicsWorld.Shutdown();
}

TEST(PhysicsWorldIntegrationValidation, ContinuousDetectionPropagatesToPhysicsBody)
{
    RVX::WorldConfig config;
    config.name = "PhysicsContinuousDetectionWorld";
    config.autoInitializeSpatial = false;
    config.autoInitializePhysics = true;

    RVX::World world;
    world.Initialize(config);

    RVX::SceneEntity* entity = world.SpawnActor();
    ASSERT_NE(nullptr, entity);

    auto* rigidBody = entity->AddComponent<RVX::RigidBodyComponent>();
    ASSERT_NE(nullptr, rigidBody);
    rigidBody->SetContinuousDetection(true);

    world.Tick(1.0f / 60.0f);

    ASSERT_NE(nullptr, rigidBody->GetBody());
    EXPECT_EQ(RVX::Physics::MotionQuality::LinearCast, rigidBody->GetBody()->GetMotionQuality());

    rigidBody->SetContinuousDetection(false);
    EXPECT_EQ(RVX::Physics::MotionQuality::Discrete, rigidBody->GetBody()->GetMotionQuality());

    world.Shutdown();
}

TEST(PhysicsWorldIntegrationValidation, CollisionGroupPropagatesToPhysicsBody)
{
    RVX::WorldConfig config;
    config.name = "PhysicsCollisionGroupWorld";
    config.autoInitializeSpatial = false;
    config.autoInitializePhysics = true;

    RVX::World world;
    world.Initialize(config);

    RVX::SceneEntity* entity = world.SpawnActor();
    ASSERT_NE(nullptr, entity);

    auto* rigidBody = entity->AddComponent<RVX::RigidBodyComponent>();
    ASSERT_NE(nullptr, rigidBody);
    rigidBody->SetCollisionGroup(7u, 3u);

    world.Tick(1.0f / 60.0f);

    ASSERT_NE(nullptr, rigidBody->GetBody());
    EXPECT_EQ(7u, rigidBody->GetBody()->GetGroup().groupId);
    EXPECT_EQ(3u, rigidBody->GetBody()->GetGroup().subGroupId);

    rigidBody->SetCollisionGroup(8u, 4u);
    EXPECT_EQ(8u, rigidBody->GetBody()->GetGroup().groupId);
    EXPECT_EQ(4u, rigidBody->GetBody()->GetGroup().subGroupId);

    world.Shutdown();
}

TEST(PhysicsWorldIntegrationValidation, FixedStepAccumulatorControlsScenePhysicsSync)
{
    RVX::WorldConfig config;
    config.name = "PhysicsFixedStepWorld";
    config.autoInitializeSpatial = false;
    config.autoInitializePhysics = true;
    config.physics.gravity = RVX::Vec3(0.0f, -10.0f, 0.0f);
    config.physics.fixedTimeStep = 0.1f;
    config.physics.maxSubSteps = 2;

    RVX::World world;
    world.Initialize(config);

    auto* physicsSubsystem = world.GetPhysics();
    ASSERT_NE(nullptr, physicsSubsystem);
    ASSERT_NE(nullptr, physicsSubsystem->GetPhysicsWorld());

    RVX::ActorSpawnParams spawnParams;
    spawnParams.name = "FixedStepBody";
    spawnParams.localPosition = RVX::Vec3(0.0f, 10.0f, 0.0f);

    RVX::SceneEntity* entity = world.SpawnActor(spawnParams);
    ASSERT_NE(nullptr, entity);

    auto* rigidBody = entity->AddComponent<RVX::RigidBodyComponent>();
    ASSERT_NE(nullptr, rigidBody);
    rigidBody->SetBodyType(RVX::RigidBodyType::Dynamic);
    rigidBody->SetLinearDamping(0.0f);
    rigidBody->SetAngularDamping(0.0f);

    world.Tick(0.05f);
    ASSERT_NE(nullptr, rigidBody->GetBody());
    EXPECT_EQ(0u, physicsSubsystem->GetLastPhysicsStepCount());
    EXPECT_NEAR(0.05f, physicsSubsystem->GetPhysicsAccumulatorSeconds(), 0.0001f);
    EXPECT_NEAR(10.0f, entity->GetWorldPosition().y, 0.0001f);

    world.Tick(0.05f);
    EXPECT_EQ(1u, physicsSubsystem->GetLastPhysicsStepCount());
    EXPECT_NEAR(0.0f, physicsSubsystem->GetPhysicsAccumulatorSeconds(), 0.0001f);
    EXPECT_NEAR(9.9f, entity->GetWorldPosition().y, 0.0001f);

    world.Tick(0.35f);
    EXPECT_EQ(2u, physicsSubsystem->GetLastPhysicsStepCount());
    EXPECT_NEAR(0.0f, physicsSubsystem->GetPhysicsAccumulatorSeconds(), 0.0001f);
    EXPECT_NEAR(9.4f, entity->GetWorldPosition().y, 0.0001f);

    world.Shutdown();
}

TEST(PhysicsWorldIntegrationValidation, ColliderAddedAfterRigidBodyRefreshesWorldBodyShape)
{
    RVX::WorldConfig config;
    config.name = "PhysicsColliderRefreshWorld";
    config.autoInitializeSpatial = false;
    config.autoInitializePhysics = true;

    RVX::World world;
    world.Initialize(config);

    auto* physicsSubsystem = world.GetPhysics();
    ASSERT_NE(nullptr, physicsSubsystem);
    ASSERT_NE(nullptr, physicsSubsystem->GetPhysicsWorld());

    RVX::SceneEntity* entity = world.SpawnActor();
    ASSERT_NE(nullptr, entity);

    auto* rigidBody = entity->AddComponent<RVX::RigidBodyComponent>();
    ASSERT_NE(nullptr, rigidBody);
    rigidBody->SetBodyType(RVX::RigidBodyType::Static);

    world.Tick(1.0f / 60.0f);

    ASSERT_NE(nullptr, rigidBody->GetBody());
    EXPECT_EQ(0u, rigidBody->GetBody()->GetShapeCount());

    auto* collider = entity->AddComponent<RVX::ColliderComponent>();
    ASSERT_NE(nullptr, collider);

    EXPECT_EQ(1u, rigidBody->GetBody()->GetShapeCount());

    collider->SetCenter(RVX::Vec3(2.0f, 0.0f, 0.0f));
    const auto offsetBounds = rigidBody->GetBody()->GetAABB();
    ExpectNearVec3((offsetBounds.min + offsetBounds.max) * 0.5f, RVX::Vec3(2.0f, 0.0f, 0.0f));

    collider->SetHalfExtents(RVX::Vec3(1.0f, 2.0f, 3.0f));
    const auto resizedBounds = rigidBody->GetBody()->GetAABB();
    ExpectNearVec3(resizedBounds.max - resizedBounds.min, RVX::Vec3(2.0f, 4.0f, 6.0f));
    EXPECT_EQ(1u, rigidBody->GetBody()->GetShapeCount());

    collider->SetTrigger(true);
    EXPECT_TRUE(rigidBody->GetBody()->IsTrigger());

    world.Shutdown();
}

TEST(PhysicsWorldIntegrationValidation, CollisionMaskPropagatesToPhysicsQueries)
{
    RVX::WorldConfig config;
    config.name = "PhysicsCollisionMaskWorld";
    config.autoInitializeSpatial = false;
    config.autoInitializePhysics = true;

    RVX::World world;
    world.Initialize(config);

    auto* physicsSubsystem = world.GetPhysics();
    ASSERT_NE(nullptr, physicsSubsystem);
    ASSERT_NE(nullptr, physicsSubsystem->GetPhysicsWorld());

    RVX::ActorSpawnParams spawnParams;
    spawnParams.localPosition = RVX::Vec3(0.0f, 0.0f, 0.0f);

    RVX::SceneEntity* entity = world.SpawnActor(spawnParams);
    ASSERT_NE(nullptr, entity);

    auto* rigidBody = entity->AddComponent<RVX::RigidBodyComponent>();
    ASSERT_NE(nullptr, rigidBody);
    rigidBody->SetBodyType(RVX::RigidBodyType::Static);
    rigidBody->SetCollisionLayer(3u);
    rigidBody->SetCollisionMask(0xFFFFFFFFu);

    auto* collider = entity->AddComponent<RVX::ColliderComponent>();
    ASSERT_NE(nullptr, collider);
    collider->SetHalfExtents(RVX::Vec3(1.0f));

    world.Tick(1.0f / 60.0f);
    ASSERT_NE(nullptr, rigidBody->GetBody());
    EXPECT_EQ(0xFFFFFFFFu, rigidBody->GetBody()->GetCollisionMask());

    RVX::Physics::RaycastHit hit;
    const RVX::Vec3 origin(0.0f, 0.0f, 5.0f);
    const RVX::Vec3 direction(0.0f, 0.0f, -1.0f);
    constexpr uint32_t layer3Mask = 1u << 3u;
    constexpr uint32_t layer4Mask = 1u << 4u;

    EXPECT_TRUE(physicsSubsystem->GetPhysicsWorld()->Raycast(origin, direction, 10.0f, hit, layer3Mask));
    EXPECT_FALSE(physicsSubsystem->GetPhysicsWorld()->Raycast(origin, direction, 10.0f, hit, layer4Mask));

    rigidBody->SetCollisionMask(0u);
    EXPECT_EQ(0u, rigidBody->GetBody()->GetCollisionMask());
    EXPECT_FALSE(physicsSubsystem->GetPhysicsWorld()->Raycast(origin, direction, 10.0f, hit, layer3Mask));

    rigidBody->SetCollisionMask(layer3Mask);
    EXPECT_EQ(layer3Mask, rigidBody->GetBody()->GetCollisionMask());
    EXPECT_TRUE(physicsSubsystem->GetPhysicsWorld()->Raycast(origin, direction, 10.0f, hit, layer3Mask));

    world.Shutdown();
}

TEST(PhysicsWorldIntegrationValidation, CastQueriesRejectInvalidInput)
{
    RVX::WorldConfig config;
    config.name = "PhysicsInvalidCastQueryWorld";
    config.autoInitializeSpatial = false;
    config.autoInitializePhysics = true;

    RVX::World world;
    world.Initialize(config);

    auto* physicsSubsystem = world.GetPhysics();
    ASSERT_NE(nullptr, physicsSubsystem);
    ASSERT_NE(nullptr, physicsSubsystem->GetPhysicsWorld());

    RVX::ActorSpawnParams spawnParams;
    spawnParams.localPosition = RVX::Vec3(0.0f, 0.0f, 0.0f);
    RVX::SceneEntity* entity = world.SpawnActor(spawnParams);
    ASSERT_NE(nullptr, entity);

    auto* rigidBody = entity->AddComponent<RVX::RigidBodyComponent>();
    ASSERT_NE(nullptr, rigidBody);
    rigidBody->SetBodyType(RVX::RigidBodyType::Static);

    auto* collider = entity->AddComponent<RVX::ColliderComponent>();
    ASSERT_NE(nullptr, collider);
    collider->SetHalfExtents(RVX::Vec3(1.0f));

    world.Tick(1.0f / 60.0f);

    auto* physicsWorld = physicsSubsystem->GetPhysicsWorld();
    const RVX::Vec3 origin(0.0f, 0.0f, 5.0f);
    const RVX::Vec3 direction(0.0f, 0.0f, -1.0f);
    const RVX::Vec3 zeroDirection(0.0f);
    const RVX::Vec3 invalidOrigin(std::numeric_limits<float>::quiet_NaN(), 0.0f, 5.0f);

    RVX::Physics::RaycastHit rayHit;
    ASSERT_TRUE(physicsWorld->Raycast(origin, direction, 10.0f, rayHit));
    EXPECT_TRUE(rayHit.hit);
    EXPECT_FALSE(physicsWorld->Raycast(origin, zeroDirection, 10.0f, rayHit));
    EXPECT_FALSE(rayHit.hit);
    EXPECT_EQ(0u, rayHit.bodyId);
    EXPECT_FALSE(physicsWorld->Raycast(invalidOrigin, direction, 10.0f, rayHit));
    EXPECT_FALSE(rayHit.hit);
    EXPECT_FALSE(physicsWorld->Raycast(origin, direction, 0.0f, rayHit));
    EXPECT_FALSE(rayHit.hit);

    std::vector<RVX::Physics::RaycastHit> rayHits;
    ASSERT_EQ(1u, physicsWorld->RaycastAll(origin, direction, 10.0f, rayHits));
    EXPECT_EQ(0u, physicsWorld->RaycastAll(origin, zeroDirection, 10.0f, rayHits));
    EXPECT_TRUE(rayHits.empty());

    RVX::Physics::ShapeCastHit shapeHit;
    ASSERT_TRUE(physicsWorld->SphereCast(origin, 0.25f, direction, 10.0f, shapeHit));
    EXPECT_TRUE(shapeHit.hit);
    EXPECT_FALSE(physicsWorld->SphereCast(origin, -1.0f, direction, 10.0f, shapeHit));
    EXPECT_FALSE(shapeHit.hit);
    EXPECT_EQ(0u, shapeHit.bodyId);
    EXPECT_FALSE(physicsWorld->SphereCast(origin, 0.25f, zeroDirection, 10.0f, shapeHit));
    EXPECT_FALSE(shapeHit.hit);
    EXPECT_FALSE(physicsWorld->SphereCast(origin, 0.25f, direction, 0.0f, shapeHit));
    EXPECT_FALSE(shapeHit.hit);

    world.Shutdown();
}

TEST(PhysicsWorldIntegrationValidation, QueryBroadphasePrunesRaycastAndOverlapCandidates)
{
    RVX::Physics::PhysicsWorldConfig config;
    config.gravity = RVX::Vec3(0.0f);

    RVX::Physics::PhysicsWorld physicsWorld;
    ASSERT_TRUE(physicsWorld.Initialize(config));

    const RVX::Physics::BodyHandle targetBody =
        CreateBoxBody(physicsWorld, RVX::Physics::BodyType::Static, RVX::Vec3(0.0f));

    constexpr int farBodyCount = 64;
    for (int i = 0; i < farBodyCount; ++i)
    {
        const float x = static_cast<float>((i % 8) * 6 - 24);
        const float y = static_cast<float>((i / 8) * 6 + 20);
        CreateBoxBody(physicsWorld, RVX::Physics::BodyType::Static, RVX::Vec3(x, y, 0.0f));
    }

    ASSERT_EQ(static_cast<size_t>(farBodyCount + 1), physicsWorld.GetBodyCount());

    RVX::Physics::RaycastHit rayHit;
    ASSERT_TRUE(physicsWorld.Raycast(RVX::Vec3(-5.0f, 0.0f, 0.0f),
                                     RVX::Vec3(1.0f, 0.0f, 0.0f),
                                     10.0f,
                                     rayHit));
    EXPECT_EQ(targetBody.GetId(), rayHit.bodyId);

    const auto rayStats = physicsWorld.GetLastQueryStats();
    EXPECT_GT(rayStats.broadphaseNodeVisits, 0u);
    EXPECT_EQ(1u, rayStats.broadphaseCandidateCount);
    EXPECT_EQ(1u, rayStats.narrowphaseTestCount);
    EXPECT_LT(rayStats.narrowphaseTestCount, physicsWorld.GetBodyCount());

    std::vector<RVX::Physics::BodyHandle> overlaps;
    ASSERT_EQ(1u, physicsWorld.OverlapSphere(RVX::Vec3(0.0f), 1.5f, overlaps));
    EXPECT_EQ(targetBody.GetId(), overlaps.front().GetId());

    const auto overlapStats = physicsWorld.GetLastQueryStats();
    EXPECT_GT(overlapStats.broadphaseNodeVisits, 0u);
    EXPECT_EQ(1u, overlapStats.broadphaseCandidateCount);
    EXPECT_EQ(1u, overlapStats.narrowphaseTestCount);
    EXPECT_LT(overlapStats.narrowphaseTestCount, physicsWorld.GetBodyCount());

    physicsWorld.Shutdown();
}

TEST(PhysicsWorldIntegrationValidation, CollisionCallbacksFireEnterAndExitOnce)
{
    RVX::Physics::PhysicsWorldConfig config;
    config.gravity = RVX::Vec3(0.0f);
    config.fixedTimeStep = 0.1f;
    config.maxSubSteps = 1;

    RVX::Physics::PhysicsWorld physicsWorld;
    ASSERT_TRUE(physicsWorld.Initialize(config));

    const RVX::Physics::BodyHandle staticBody =
        CreateBoxBody(physicsWorld, RVX::Physics::BodyType::Static, RVX::Vec3(0.0f));
    const RVX::Physics::BodyHandle dynamicBody =
        CreateBoxBody(physicsWorld, RVX::Physics::BodyType::Dynamic, RVX::Vec3(0.0f));

    int collisionEnterCount = 0;
    int collisionExitCount = 0;
    int triggerEnterCount = 0;
    uint64_t enteredBodyA = 0;
    uint64_t enteredBodyB = 0;

    physicsWorld.SetOnCollisionEnter(
        [&](const RVX::Physics::CollisionEvent& event)
        {
            ++collisionEnterCount;
            enteredBodyA = event.bodyIdA;
            enteredBodyB = event.bodyIdB;
            EXPECT_FALSE(event.isTrigger);
        });
    physicsWorld.SetOnCollisionExit(
        [&](const RVX::Physics::CollisionEvent& event)
        {
            ++collisionExitCount;
            EXPECT_FALSE(event.isTrigger);
        });
    physicsWorld.SetOnTriggerEnter(
        [&](const RVX::Physics::CollisionEvent&)
        {
            ++triggerEnterCount;
        });

    physicsWorld.Step(0.1f);
    EXPECT_EQ(1, collisionEnterCount);
    EXPECT_EQ(0, collisionExitCount);
    EXPECT_EQ(0, triggerEnterCount);
    EXPECT_NE(enteredBodyA, enteredBodyB);
    EXPECT_TRUE(enteredBodyA == staticBody.GetId() || enteredBodyB == staticBody.GetId());
    EXPECT_TRUE(enteredBodyA == dynamicBody.GetId() || enteredBodyB == dynamicBody.GetId());

    physicsWorld.Step(0.1f);
    EXPECT_EQ(1, collisionEnterCount);
    EXPECT_EQ(0, collisionExitCount);

    physicsWorld.SetBodyPosition(dynamicBody, RVX::Vec3(5.0f, 0.0f, 0.0f));
    physicsWorld.Step(0.1f);
    EXPECT_EQ(1, collisionEnterCount);
    EXPECT_EQ(1, collisionExitCount);

    physicsWorld.Shutdown();
}

TEST(PhysicsWorldIntegrationValidation, MaxBodyPairsLimitsCollisionEventPairs)
{
    RVX::Physics::PhysicsWorldConfig config;
    config.gravity = RVX::Vec3(0.0f);
    config.fixedTimeStep = 0.1f;
    config.maxSubSteps = 1;
    config.maxBodyPairs = 0;

    RVX::Physics::PhysicsWorld physicsWorld;
    ASSERT_TRUE(physicsWorld.Initialize(config));

    CreateBoxBody(physicsWorld, RVX::Physics::BodyType::Static, RVX::Vec3(0.0f));
    CreateBoxBody(physicsWorld, RVX::Physics::BodyType::Dynamic, RVX::Vec3(0.0f));

    int collisionEnterCount = 0;
    physicsWorld.SetOnCollisionEnter(
        [&](const RVX::Physics::CollisionEvent&)
        {
            ++collisionEnterCount;
        });

    physicsWorld.Step(0.1f);
    EXPECT_EQ(0, collisionEnterCount);

    RVX::Physics::PhysicsWorld::DebugDrawOptions options;
    options.drawBodies = false;
    options.drawShapes = false;
    options.drawContacts = true;

    std::vector<RVX::Vec3> lines;
    std::vector<RVX::Vec4> colors;
    physicsWorld.GetDebugDrawData(lines, colors, options);
    EXPECT_TRUE(lines.empty());
    EXPECT_TRUE(colors.empty());

    physicsWorld.Shutdown();
}

TEST(PhysicsWorldIntegrationValidation, TriggerCallbacksUseTriggerChannel)
{
    RVX::Physics::PhysicsWorldConfig config;
    config.gravity = RVX::Vec3(0.0f);
    config.fixedTimeStep = 0.1f;
    config.maxSubSteps = 1;

    RVX::Physics::PhysicsWorld physicsWorld;
    ASSERT_TRUE(physicsWorld.Initialize(config));

    const RVX::Physics::BodyHandle triggerBody =
        CreateBoxBody(physicsWorld, RVX::Physics::BodyType::Static, RVX::Vec3(0.0f), true);
    const RVX::Physics::BodyHandle dynamicBody =
        CreateBoxBody(physicsWorld, RVX::Physics::BodyType::Dynamic, RVX::Vec3(0.0f));

    int collisionEnterCount = 0;
    int collisionExitCount = 0;
    int triggerEnterCount = 0;
    int triggerExitCount = 0;
    uint64_t triggeredBodyA = 0;
    uint64_t triggeredBodyB = 0;

    physicsWorld.SetOnCollisionEnter(
        [&](const RVX::Physics::CollisionEvent&)
        {
            ++collisionEnterCount;
        });
    physicsWorld.SetOnCollisionExit(
        [&](const RVX::Physics::CollisionEvent&)
        {
            ++collisionExitCount;
        });
    physicsWorld.SetOnTriggerEnter(
        [&](const RVX::Physics::CollisionEvent& event)
        {
            ++triggerEnterCount;
            triggeredBodyA = event.bodyIdA;
            triggeredBodyB = event.bodyIdB;
            EXPECT_TRUE(event.isTrigger);
        });
    physicsWorld.SetOnTriggerExit(
        [&](const RVX::Physics::CollisionEvent& event)
        {
            ++triggerExitCount;
            EXPECT_TRUE(event.isTrigger);
        });

    physicsWorld.Step(0.1f);
    EXPECT_EQ(0, collisionEnterCount);
    EXPECT_EQ(0, collisionExitCount);
    EXPECT_EQ(1, triggerEnterCount);
    EXPECT_EQ(0, triggerExitCount);
    EXPECT_TRUE(triggeredBodyA == triggerBody.GetId() || triggeredBodyB == triggerBody.GetId());
    EXPECT_TRUE(triggeredBodyA == dynamicBody.GetId() || triggeredBodyB == dynamicBody.GetId());

    physicsWorld.Step(0.1f);
    EXPECT_EQ(1, triggerEnterCount);
    EXPECT_EQ(0, triggerExitCount);

    physicsWorld.SetBodyPosition(dynamicBody, RVX::Vec3(5.0f, 0.0f, 0.0f));
    physicsWorld.Step(0.1f);
    EXPECT_EQ(0, collisionEnterCount);
    EXPECT_EQ(0, collisionExitCount);
    EXPECT_EQ(1, triggerEnterCount);
    EXPECT_EQ(1, triggerExitCount);

    physicsWorld.Shutdown();
}

TEST(PhysicsWorldIntegrationValidation, MaxContactConstraintsLimitsContactResolutionOnly)
{
    RVX::Physics::PhysicsWorldConfig config;
    config.gravity = RVX::Vec3(0.0f);
    config.fixedTimeStep = 0.1f;
    config.maxSubSteps = 1;
    config.maxBodyPairs = 4;
    config.maxContactConstraints = 0;

    RVX::Physics::PhysicsWorld physicsWorld;
    ASSERT_TRUE(physicsWorld.Initialize(config));

    CreateBoxBody(physicsWorld, RVX::Physics::BodyType::Static, RVX::Vec3(0.0f));
    const RVX::Physics::BodyHandle dynamicBody =
        CreateBoxBody(physicsWorld, RVX::Physics::BodyType::Dynamic, RVX::Vec3(0.0f));

    int collisionEnterCount = 0;
    physicsWorld.SetOnCollisionEnter(
        [&](const RVX::Physics::CollisionEvent&)
        {
            ++collisionEnterCount;
        });

    physicsWorld.Step(0.1f);

    RVX::Physics::RigidBody* body = physicsWorld.GetBody(dynamicBody);
    ASSERT_NE(nullptr, body);
    EXPECT_EQ(1, collisionEnterCount);
    ExpectNearVec3(body->GetPosition(), RVX::Vec3(0.0f));

    physicsWorld.Shutdown();
}

TEST(PhysicsWorldIntegrationValidation, CollisionGroupFiltersBodyBodyContactsAndEvents)
{
    RVX::Physics::PhysicsWorldConfig config;
    config.gravity = RVX::Vec3(0.0f);
    config.fixedTimeStep = 0.1f;
    config.maxSubSteps = 1;

    RVX::Physics::PhysicsWorld physicsWorld;
    ASSERT_TRUE(physicsWorld.Initialize(config));

    RVX::Physics::RigidBodyDesc staticDesc;
    staticDesc.type = RVX::Physics::BodyType::Static;
    staticDesc.position = RVX::Vec3(0.0f);
    staticDesc.group.groupId = 42u;
    staticDesc.group.subGroupId = 9u;

    const RVX::Physics::BodyHandle staticBody = physicsWorld.CreateBody(staticDesc);
    physicsWorld.AddShape(staticBody, RVX::Physics::BoxShape::Create(RVX::Vec3(1.0f)));

    RVX::Physics::RigidBodyDesc dynamicDesc;
    dynamicDesc.type = RVX::Physics::BodyType::Dynamic;
    dynamicDesc.position = RVX::Vec3(0.0f);
    dynamicDesc.gravityScale = 0.0f;
    dynamicDesc.linearDamping = 0.0f;
    dynamicDesc.angularDamping = 0.0f;
    dynamicDesc.allowSleep = false;
    dynamicDesc.group.groupId = 42u;
    dynamicDesc.group.subGroupId = 9u;

    const RVX::Physics::BodyHandle dynamicBody = physicsWorld.CreateBody(dynamicDesc);
    physicsWorld.AddShape(dynamicBody, RVX::Physics::BoxShape::Create(RVX::Vec3(1.0f)));

    int collisionEnterCount = 0;
    physicsWorld.SetOnCollisionEnter(
        [&](const RVX::Physics::CollisionEvent&)
        {
            ++collisionEnterCount;
        });

    physicsWorld.Step(0.1f);

    RVX::Physics::RigidBody* dynamicRigidBody = physicsWorld.GetBody(dynamicBody);
    ASSERT_NE(nullptr, dynamicRigidBody);
    EXPECT_EQ(0, collisionEnterCount);
    ExpectNearVec3(dynamicRigidBody->GetPosition(), RVX::Vec3(0.0f));

    RVX::Physics::CollisionGroup differentSubGroup;
    differentSubGroup.groupId = 42u;
    differentSubGroup.subGroupId = 10u;
    dynamicRigidBody->SetGroup(differentSubGroup);

    physicsWorld.Step(0.1f);

    EXPECT_EQ(1, collisionEnterCount);
    EXPECT_NEAR(2.0f, glm::length(dynamicRigidBody->GetPosition()), 0.0001f);

    physicsWorld.Shutdown();
}

TEST(PhysicsWorldIntegrationValidation, DynamicBodyStopsOnStaticAABBContact)
{
    RVX::Physics::PhysicsWorldConfig config;
    config.gravity = RVX::Vec3(0.0f, -10.0f, 0.0f);
    config.fixedTimeStep = 0.1f;
    config.maxSubSteps = 1;
    config.sleepVelocityThreshold = 0.01f;
    config.sleepTimeThreshold = 1.0f;

    RVX::Physics::PhysicsWorld physicsWorld;
    ASSERT_TRUE(physicsWorld.Initialize(config));

    const RVX::Physics::BodyHandle floorBody =
        CreateBoxBody(physicsWorld,
                      RVX::Physics::BodyType::Static,
                      RVX::Vec3(0.0f),
                      false,
                      RVX::Vec3(5.0f, 0.5f, 5.0f));

    RVX::Physics::RigidBodyDesc dynamicDesc;
    dynamicDesc.type = RVX::Physics::BodyType::Dynamic;
    dynamicDesc.position = RVX::Vec3(0.0f, 2.0f, 0.0f);
    dynamicDesc.gravityScale = 1.0f;
    dynamicDesc.linearDamping = 0.0f;
    dynamicDesc.angularDamping = 0.0f;
    dynamicDesc.allowSleep = false;

    const RVX::Physics::BodyHandle dynamicBody = physicsWorld.CreateBody(dynamicDesc);
    physicsWorld.AddShape(dynamicBody, RVX::Physics::BoxShape::Create(RVX::Vec3(0.5f)));

    for (int i = 0; i < 12; ++i)
    {
        physicsWorld.Step(0.1f);
    }

    const RVX::Physics::RigidBody* body = physicsWorld.GetBody(dynamicBody);
    ASSERT_NE(nullptr, body);
    EXPECT_NEAR(1.0f, body->GetPosition().y, 0.0001f);
    EXPECT_NEAR(0.0f, body->GetLinearVelocity().y, 0.0001f);

    const RVX::Physics::RigidBody* floor = physicsWorld.GetBody(floorBody);
    ASSERT_NE(nullptr, floor);
    EXPECT_NEAR(0.0f, floor->GetPosition().y, 0.0001f);

    physicsWorld.Shutdown();
}

TEST(PhysicsWorldIntegrationValidation, PositionConstraintPreventsContactCorrectionOnLockedAxis)
{
    RVX::Physics::PhysicsWorldConfig config;
    config.gravity = RVX::Vec3(0.0f);
    config.fixedTimeStep = 0.1f;
    config.maxSubSteps = 1;

    RVX::Physics::PhysicsWorld physicsWorld;
    ASSERT_TRUE(physicsWorld.Initialize(config));

    CreateBoxBody(physicsWorld,
                  RVX::Physics::BodyType::Static,
                  RVX::Vec3(0.0f),
                  false,
                  RVX::Vec3(5.0f, 0.5f, 5.0f));

    RVX::Physics::RigidBodyDesc dynamicDesc;
    dynamicDesc.type = RVX::Physics::BodyType::Dynamic;
    dynamicDesc.position = RVX::Vec3(0.0f, 0.75f, 0.0f);
    dynamicDesc.gravityScale = 0.0f;
    dynamicDesc.linearDamping = 0.0f;
    dynamicDesc.angularDamping = 0.0f;
    dynamicDesc.allowSleep = false;
    dynamicDesc.positionConstraints = 2u;

    const RVX::Physics::BodyHandle dynamicBody = physicsWorld.CreateBody(dynamicDesc);
    physicsWorld.AddShape(dynamicBody, RVX::Physics::BoxShape::Create(RVX::Vec3(0.5f)));

    RVX::Physics::RigidBody* body = physicsWorld.GetBody(dynamicBody);
    ASSERT_NE(nullptr, body);
    body->SetLinearVelocity(RVX::Vec3(0.0f, -5.0f, 0.0f));

    physicsWorld.Step(0.1f);

    EXPECT_NEAR(0.75f, body->GetPosition().y, 0.0001f);
    EXPECT_NEAR(0.0f, body->GetLinearVelocity().y, 0.0001f);
    EXPECT_EQ(2u, body->GetPositionConstraints());

    physicsWorld.Shutdown();
}

TEST(PhysicsWorldIntegrationValidation, LinearCastBodyStopsBeforeTunnelingThroughStaticAABB)
{
    RVX::Physics::PhysicsWorldConfig config;
    config.gravity = RVX::Vec3(0.0f);
    config.fixedTimeStep = 1.0f;
    config.maxSubSteps = 1;

    RVX::Physics::PhysicsWorld physicsWorld;
    ASSERT_TRUE(physicsWorld.Initialize(config));

    CreateBoxBody(physicsWorld,
                  RVX::Physics::BodyType::Static,
                  RVX::Vec3(0.0f),
                  false,
                  RVX::Vec3(5.0f, 0.1f, 5.0f));

    RVX::Physics::RigidBodyDesc dynamicDesc;
    dynamicDesc.type = RVX::Physics::BodyType::Dynamic;
    dynamicDesc.motionQuality = RVX::Physics::MotionQuality::LinearCast;
    dynamicDesc.position = RVX::Vec3(0.0f, 2.0f, 0.0f);
    dynamicDesc.linearVelocity = RVX::Vec3(0.0f, -10.0f, 0.0f);
    dynamicDesc.gravityScale = 0.0f;
    dynamicDesc.linearDamping = 0.0f;
    dynamicDesc.angularDamping = 0.0f;
    dynamicDesc.allowSleep = false;

    const RVX::Physics::BodyHandle dynamicBody = physicsWorld.CreateBody(dynamicDesc);
    physicsWorld.AddShape(dynamicBody, RVX::Physics::BoxShape::Create(RVX::Vec3(0.5f)));

    physicsWorld.Step(1.0f);

    const RVX::Physics::RigidBody* body = physicsWorld.GetBody(dynamicBody);
    ASSERT_NE(nullptr, body);
    EXPECT_EQ(RVX::Physics::MotionQuality::LinearCast, body->GetMotionQuality());
    EXPECT_GT(body->GetPosition().y, 0.59f);
    EXPECT_LT(body->GetPosition().y, 0.61f);
    EXPECT_NEAR(0.0f, body->GetLinearVelocity().y, 0.0001f);

    physicsWorld.Shutdown();
}

TEST(PhysicsWorldIntegrationValidation, DebugDrawOutputsBodyAABBLines)
{
    RVX::Physics::PhysicsWorld physicsWorld;
    ASSERT_TRUE(physicsWorld.Initialize({}));

    CreateBoxBody(physicsWorld,
                  RVX::Physics::BodyType::Static,
                  RVX::Vec3(2.0f, 3.0f, 4.0f),
                  false,
                  RVX::Vec3(1.0f, 2.0f, 3.0f));

    std::vector<RVX::Vec3> lines;
    std::vector<RVX::Vec4> colors;
    physicsWorld.GetDebugDrawData(lines, colors);

    ASSERT_EQ(24u, lines.size());
    ASSERT_EQ(lines.size(), colors.size());
    ExpectNearVec3(lines[0], RVX::Vec3(1.0f, 1.0f, 1.0f));
    ExpectNearVec3(lines[1], RVX::Vec3(3.0f, 1.0f, 1.0f));
    EXPECT_NEAR(0.55f, colors[0].x, 0.0001f);
    EXPECT_NEAR(0.6f, colors[0].y, 0.0001f);
    EXPECT_NEAR(0.65f, colors[0].z, 0.0001f);
    EXPECT_NEAR(1.0f, colors[0].w, 0.0001f);

    RVX::Physics::PhysicsWorld::DebugDrawOptions options;
    options.drawBodies = false;
    options.drawShapes = false;
    physicsWorld.GetDebugDrawData(lines, colors, options);
    EXPECT_TRUE(lines.empty());
    EXPECT_TRUE(colors.empty());

    physicsWorld.Shutdown();
}

TEST(PhysicsWorldIntegrationValidation, DebugDrawOutputsActiveContactNormals)
{
    RVX::Physics::PhysicsWorldConfig config;
    config.gravity = RVX::Vec3(0.0f);
    config.fixedTimeStep = 0.1f;
    config.maxSubSteps = 1;

    RVX::Physics::PhysicsWorld physicsWorld;
    ASSERT_TRUE(physicsWorld.Initialize(config));

    CreateBoxBody(physicsWorld, RVX::Physics::BodyType::Static, RVX::Vec3(0.0f));
    CreateBoxBody(physicsWorld, RVX::Physics::BodyType::Dynamic, RVX::Vec3(0.0f));
    physicsWorld.Step(0.1f);

    RVX::Physics::PhysicsWorld::DebugDrawOptions options;
    options.drawBodies = false;
    options.drawShapes = false;
    options.drawContacts = true;

    std::vector<RVX::Vec3> lines;
    std::vector<RVX::Vec4> colors;
    physicsWorld.GetDebugDrawData(lines, colors, options);

    ASSERT_EQ(2u, lines.size());
    ASSERT_EQ(lines.size(), colors.size());
    ExpectNearVec3(lines[0], RVX::Vec3(1.0f, 0.0f, 0.0f));
    ExpectNearVec3(lines[1], RVX::Vec3(1.5f, 0.0f, 0.0f));
    EXPECT_NEAR(1.0f, colors[0].x, 0.0001f);
    EXPECT_NEAR(0.2f, colors[0].y, 0.0001f);
    EXPECT_NEAR(0.2f, colors[0].z, 0.0001f);
    EXPECT_NEAR(1.0f, colors[0].w, 0.0001f);

    physicsWorld.Shutdown();
}

TEST(PhysicsWorldIntegrationValidation, DebugDrawOutputsBroadphaseAABBLines)
{
    RVX::Physics::PhysicsWorld physicsWorld;
    ASSERT_TRUE(physicsWorld.Initialize({}));

    CreateBoxBody(physicsWorld,
                  RVX::Physics::BodyType::Static,
                  RVX::Vec3(2.0f, 3.0f, 4.0f),
                  false,
                  RVX::Vec3(1.0f, 2.0f, 3.0f));

    RVX::Physics::PhysicsWorld::DebugDrawOptions options;
    options.drawBodies = false;
    options.drawShapes = false;
    options.drawBroadphase = true;

    std::vector<RVX::Vec3> lines;
    std::vector<RVX::Vec4> colors;
    physicsWorld.GetDebugDrawData(lines, colors, options);

    ASSERT_EQ(24u, lines.size());
    ASSERT_EQ(lines.size(), colors.size());
    ExpectNearVec3(lines[0], RVX::Vec3(1.0f, 1.0f, 1.0f));
    ExpectNearVec3(lines[1], RVX::Vec3(3.0f, 1.0f, 1.0f));
    EXPECT_NEAR(0.75f, colors[0].x, 0.0001f);
    EXPECT_NEAR(0.35f, colors[0].y, 0.0001f);
    EXPECT_NEAR(1.0f, colors[0].z, 0.0001f);
    EXPECT_NEAR(1.0f, colors[0].w, 0.0001f);

    physicsWorld.Shutdown();
}

TEST(PhysicsWorldIntegrationValidation, DebugDrawOutputsActiveConstraintLines)
{
    RVX::Physics::PhysicsWorld physicsWorld;
    ASSERT_TRUE(physicsWorld.Initialize({}));

    const RVX::Physics::BodyHandle bodyAHandle =
        CreateBoxBody(physicsWorld, RVX::Physics::BodyType::Static, RVX::Vec3(1.0f, 0.0f, 0.0f));
    const RVX::Physics::BodyHandle bodyBHandle =
        CreateBoxBody(physicsWorld, RVX::Physics::BodyType::Dynamic, RVX::Vec3(4.0f, 0.0f, 0.0f));

    RVX::Physics::RigidBody* bodyA = physicsWorld.GetBody(bodyAHandle);
    RVX::Physics::RigidBody* bodyB = physicsWorld.GetBody(bodyBHandle);
    ASSERT_NE(nullptr, bodyA);
    ASSERT_NE(nullptr, bodyB);

    auto constraint = std::make_shared<CountingConstraint>();
    constraint->SetBodies(bodyA, bodyB);
    constraint->SetAnchors(RVX::Vec3(0.25f, 0.0f, 0.0f), RVX::Vec3(-0.5f, 0.0f, 0.0f));
    ASSERT_NE(0u, physicsWorld.CreateConstraint(constraint));

    RVX::Physics::PhysicsWorld::DebugDrawOptions options;
    options.drawBodies = false;
    options.drawShapes = false;
    options.drawConstraints = true;

    std::vector<RVX::Vec3> lines;
    std::vector<RVX::Vec4> colors;
    physicsWorld.GetDebugDrawData(lines, colors, options);

    ASSERT_EQ(2u, lines.size());
    ASSERT_EQ(lines.size(), colors.size());
    ExpectNearVec3(lines[0], RVX::Vec3(1.25f, 0.0f, 0.0f));
    ExpectNearVec3(lines[1], RVX::Vec3(3.5f, 0.0f, 0.0f));
    EXPECT_NEAR(0.6f, colors[0].x, 0.0001f);
    EXPECT_NEAR(0.85f, colors[0].y, 0.0001f);
    EXPECT_NEAR(1.0f, colors[0].z, 0.0001f);
    EXPECT_NEAR(1.0f, colors[0].w, 0.0001f);

    constraint->Break();
    physicsWorld.GetDebugDrawData(lines, colors, options);
    EXPECT_TRUE(lines.empty());
    EXPECT_TRUE(colors.empty());

    physicsWorld.Shutdown();
}

TEST(PhysicsWorldIntegrationValidation, BoxAndCapsuleOverlapQueriesReturnFilteredBodies)
{
    RVX::WorldConfig config;
    config.name = "PhysicsOverlapQueryWorld";
    config.autoInitializeSpatial = false;
    config.autoInitializePhysics = true;

    RVX::World world;
    world.Initialize(config);

    auto* physicsSubsystem = world.GetPhysics();
    ASSERT_NE(nullptr, physicsSubsystem);
    ASSERT_NE(nullptr, physicsSubsystem->GetPhysicsWorld());

    RVX::ActorSpawnParams targetParams;
    targetParams.localPosition = RVX::Vec3(0.0f, 0.0f, 0.0f);
    RVX::SceneEntity* targetEntity = world.SpawnActor(targetParams);
    ASSERT_NE(nullptr, targetEntity);

    auto* targetBody = targetEntity->AddComponent<RVX::RigidBodyComponent>();
    ASSERT_NE(nullptr, targetBody);
    targetBody->SetBodyType(RVX::RigidBodyType::Static);
    targetBody->SetCollisionLayer(3u);

    auto* targetCollider = targetEntity->AddComponent<RVX::ColliderComponent>();
    ASSERT_NE(nullptr, targetCollider);
    targetCollider->SetHalfExtents(RVX::Vec3(1.0f));

    RVX::ActorSpawnParams farParams;
    farParams.localPosition = RVX::Vec3(5.0f, 0.0f, 0.0f);
    RVX::SceneEntity* farEntity = world.SpawnActor(farParams);
    ASSERT_NE(nullptr, farEntity);

    auto* farBody = farEntity->AddComponent<RVX::RigidBodyComponent>();
    ASSERT_NE(nullptr, farBody);
    farBody->SetBodyType(RVX::RigidBodyType::Static);
    farBody->SetCollisionLayer(3u);

    auto* farCollider = farEntity->AddComponent<RVX::ColliderComponent>();
    ASSERT_NE(nullptr, farCollider);
    farCollider->SetHalfExtents(RVX::Vec3(0.5f));

    RVX::ActorSpawnParams diagonalParams;
    diagonalParams.localPosition = RVX::Vec3(1.2f, 1.2f, 0.0f);
    RVX::SceneEntity* diagonalEntity = world.SpawnActor(diagonalParams);
    ASSERT_NE(nullptr, diagonalEntity);

    auto* diagonalBody = diagonalEntity->AddComponent<RVX::RigidBodyComponent>();
    ASSERT_NE(nullptr, diagonalBody);
    diagonalBody->SetBodyType(RVX::RigidBodyType::Static);
    diagonalBody->SetCollisionLayer(5u);

    auto* diagonalCollider = diagonalEntity->AddComponent<RVX::ColliderComponent>();
    ASSERT_NE(nullptr, diagonalCollider);
    diagonalCollider->SetHalfExtents(RVX::Vec3(0.05f));

    RVX::ActorSpawnParams offAxisParams;
    offAxisParams.localPosition = RVX::Vec3(1.2f, -1.2f, 0.0f);
    RVX::SceneEntity* offAxisEntity = world.SpawnActor(offAxisParams);
    ASSERT_NE(nullptr, offAxisEntity);

    auto* offAxisBody = offAxisEntity->AddComponent<RVX::RigidBodyComponent>();
    ASSERT_NE(nullptr, offAxisBody);
    offAxisBody->SetBodyType(RVX::RigidBodyType::Static);
    offAxisBody->SetCollisionLayer(5u);

    auto* offAxisCollider = offAxisEntity->AddComponent<RVX::ColliderComponent>();
    ASSERT_NE(nullptr, offAxisCollider);
    offAxisCollider->SetHalfExtents(RVX::Vec3(0.05f));

    world.Tick(1.0f / 60.0f);
    ASSERT_NE(nullptr, targetBody->GetBody());
    ASSERT_NE(nullptr, farBody->GetBody());
    ASSERT_NE(nullptr, diagonalBody->GetBody());
    ASSERT_NE(nullptr, offAxisBody->GetBody());

    auto* physicsWorld = physicsSubsystem->GetPhysicsWorld();
    std::vector<RVX::Physics::BodyHandle> overlaps;
    constexpr uint32_t layer3Mask = 1u << 3u;
    constexpr uint32_t layer4Mask = 1u << 4u;
    constexpr uint32_t layer5Mask = 1u << 5u;

    EXPECT_EQ(1u, physicsWorld->OverlapBox(RVX::Vec3(0.0f), RVX::Vec3(1.5f), overlaps, layer3Mask));
    EXPECT_TRUE(ContainsBodyHandle(overlaps, targetBody->GetBody()->GetHandle()));
    EXPECT_FALSE(ContainsBodyHandle(overlaps, farBody->GetBody()->GetHandle()));

    EXPECT_EQ(0u, physicsWorld->OverlapBox(RVX::Vec3(0.0f), RVX::Vec3(1.5f), overlaps, layer4Mask));
    EXPECT_TRUE(overlaps.empty());

    EXPECT_EQ(1u, physicsWorld->OverlapCapsule(RVX::Vec3(-2.0f, 0.0f, 0.0f),
                                                RVX::Vec3(2.0f, 0.0f, 0.0f),
                                                0.25f,
                                                overlaps,
                                                layer3Mask));
    EXPECT_TRUE(ContainsBodyHandle(overlaps, targetBody->GetBody()->GetHandle()));
    EXPECT_FALSE(ContainsBodyHandle(overlaps, farBody->GetBody()->GetHandle()));

    EXPECT_EQ(1u, physicsWorld->OverlapBox(RVX::Vec3(5.0f, 0.0f, 0.0f),
                                           RVX::Vec3(0.25f),
                                           overlaps,
                                           layer3Mask));
    EXPECT_TRUE(ContainsBodyHandle(overlaps, farBody->GetBody()->GetHandle()));
    EXPECT_FALSE(ContainsBodyHandle(overlaps, targetBody->GetBody()->GetHandle()));

    const RVX::Quat rotated45AroundZ(0.9238795f, 0.0f, 0.0f, 0.38268343f);
    EXPECT_EQ(1u, physicsWorld->OverlapBox(RVX::Vec3(0.0f),
                                           RVX::Vec3(2.0f, 0.25f, 1.0f),
                                           rotated45AroundZ,
                                           overlaps,
                                           layer5Mask));
    EXPECT_TRUE(ContainsBodyHandle(overlaps, diagonalBody->GetBody()->GetHandle()));
    EXPECT_FALSE(ContainsBodyHandle(overlaps, offAxisBody->GetBody()->GetHandle()));

    world.Shutdown();
}

TEST(PhysicsWorldIntegrationValidation, AutoMassUsesColliderShapeDensityAndSize)
{
    RVX::WorldConfig config;
    config.name = "PhysicsAutoMassWorld";
    config.autoInitializeSpatial = false;
    config.autoInitializePhysics = true;

    RVX::World world;
    world.Initialize(config);

    auto* physicsSubsystem = world.GetPhysics();
    ASSERT_NE(nullptr, physicsSubsystem);
    ASSERT_NE(nullptr, physicsSubsystem->GetPhysicsWorld());

    RVX::SceneEntity* entity = world.SpawnActor();
    ASSERT_NE(nullptr, entity);

    auto* collider = entity->AddComponent<RVX::ColliderComponent>();
    ASSERT_NE(nullptr, collider);
    collider->SetHalfExtents(RVX::Vec3(0.5f));
    collider->SetDensity(2.0f);

    auto* rigidBody = entity->AddComponent<RVX::RigidBodyComponent>();
    ASSERT_NE(nullptr, rigidBody);
    rigidBody->SetBodyType(RVX::RigidBodyType::Dynamic);
    rigidBody->SetUseAutoMass(true);

    world.Tick(1.0f / 60.0f);
    ASSERT_NE(nullptr, rigidBody->GetBody());
    EXPECT_NEAR(2.0f, rigidBody->GetBody()->GetMass(), 0.0001f);
    EXPECT_NEAR(2.0f, rigidBody->GetMass(), 0.0001f);
    EXPECT_NEAR(0.5f, rigidBody->GetBody()->GetInverseMass(), 0.0001f);

    collider->SetDensity(4.0f);
    EXPECT_NEAR(4.0f, rigidBody->GetBody()->GetMass(), 0.0001f);
    EXPECT_NEAR(4.0f, rigidBody->GetMass(), 0.0001f);

    collider->SetHalfExtents(RVX::Vec3(1.0f));
    EXPECT_NEAR(32.0f, rigidBody->GetBody()->GetMass(), 0.0001f);
    EXPECT_NEAR(32.0f, rigidBody->GetMass(), 0.0001f);
    EXPECT_NEAR(1.0f / 32.0f, rigidBody->GetBody()->GetInverseMass(), 0.0001f);

    world.Shutdown();
}

TEST(PhysicsWorldIntegrationValidation, DynamicBodySleepsAfterIdleThresholdAndWakesOnImpulse)
{
    RVX::WorldConfig config;
    config.name = "PhysicsSleepWorld";
    config.autoInitializeSpatial = false;
    config.autoInitializePhysics = true;
    config.physics.gravity = RVX::Vec3(0.0f);
    config.physics.fixedTimeStep = 0.1f;
    config.physics.maxSubSteps = 1;
    config.physics.sleepVelocityThreshold = 0.01f;
    config.physics.sleepTimeThreshold = 0.2f;

    RVX::World world;
    world.Initialize(config);

    auto* physicsSubsystem = world.GetPhysics();
    ASSERT_NE(nullptr, physicsSubsystem);
    ASSERT_NE(nullptr, physicsSubsystem->GetPhysicsWorld());

    RVX::SceneEntity* entity = world.SpawnActor();
    ASSERT_NE(nullptr, entity);

    auto* rigidBody = entity->AddComponent<RVX::RigidBodyComponent>();
    ASSERT_NE(nullptr, rigidBody);
    rigidBody->SetBodyType(RVX::RigidBodyType::Dynamic);
    rigidBody->SetUseGravity(false);
    rigidBody->SetLinearDamping(0.0f);
    rigidBody->SetAngularDamping(0.0f);
    rigidBody->SetCanSleep(true);

    world.Tick(0.1f);
    ASSERT_NE(nullptr, rigidBody->GetBody());
    EXPECT_FALSE(rigidBody->GetBody()->IsSleeping());
    EXPECT_FALSE(rigidBody->IsSleeping());
    EXPECT_NEAR(0.1f, rigidBody->GetBody()->GetSleepTimer(), 0.0001f);

    world.Tick(0.1f);
    EXPECT_TRUE(rigidBody->GetBody()->IsSleeping());
    EXPECT_TRUE(rigidBody->IsSleeping());
    EXPECT_NEAR(0.0f, rigidBody->GetBody()->GetSleepTimer(), 0.0001f);

    rigidBody->ApplyImpulse(RVX::Vec3(1.0f, 0.0f, 0.0f));
    EXPECT_FALSE(rigidBody->GetBody()->IsSleeping());

    world.Tick(0.1f);
    EXPECT_FALSE(rigidBody->IsSleeping());
    EXPECT_GT(entity->GetWorldPosition().x, 0.0f);

    world.Shutdown();
}

TEST(PhysicsWorldIntegrationValidation, PositionAndRotationConstraintsAffectPhysicsStep)
{
    RVX::WorldConfig config;
    config.name = "PhysicsConstraintWorld";
    config.autoInitializeSpatial = false;
    config.autoInitializePhysics = true;
    config.physics.gravity = RVX::Vec3(0.0f, -10.0f, 0.0f);
    config.physics.fixedTimeStep = 0.1f;
    config.physics.maxSubSteps = 1;

    RVX::World world;
    world.Initialize(config);

    auto* physicsSubsystem = world.GetPhysics();
    ASSERT_NE(nullptr, physicsSubsystem);
    ASSERT_NE(nullptr, physicsSubsystem->GetPhysicsWorld());

    RVX::ActorSpawnParams spawnParams;
    spawnParams.localPosition = RVX::Vec3(0.0f, 5.0f, 0.0f);

    RVX::SceneEntity* entity = world.SpawnActor(spawnParams);
    ASSERT_NE(nullptr, entity);

    auto* rigidBody = entity->AddComponent<RVX::RigidBodyComponent>();
    ASSERT_NE(nullptr, rigidBody);
    rigidBody->SetBodyType(RVX::RigidBodyType::Dynamic);
    rigidBody->SetLinearDamping(0.0f);
    rigidBody->SetAngularDamping(0.0f);
    rigidBody->SetCanSleep(false);
    rigidBody->SetPositionConstraints(false, true, false);
    rigidBody->SetRotationConstraints(true, true, true);
    rigidBody->SetAngularVelocity(RVX::Vec3(5.0f, 7.0f, 9.0f));

    bool x = false;
    bool y = false;
    bool z = false;
    rigidBody->GetPositionConstraints(x, y, z);
    EXPECT_FALSE(x);
    EXPECT_TRUE(y);
    EXPECT_FALSE(z);
    rigidBody->GetRotationConstraints(x, y, z);
    EXPECT_TRUE(x);
    EXPECT_TRUE(y);
    EXPECT_TRUE(z);

    world.Tick(0.1f);
    ASSERT_NE(nullptr, rigidBody->GetBody());
    EXPECT_EQ(2u, rigidBody->GetBody()->GetPositionConstraints());
    EXPECT_EQ(7u, rigidBody->GetBody()->GetRotationConstraints());
    EXPECT_NEAR(5.0f, entity->GetWorldPosition().y, 0.0001f);
    EXPECT_NEAR(0.0f, rigidBody->GetBody()->GetLinearVelocity().y, 0.0001f);
    ExpectNearVec3(rigidBody->GetBody()->GetAngularVelocity(), RVX::Vec3(0.0f));

    rigidBody->SetPositionConstraints(false, false, false);
    world.Tick(0.1f);
    EXPECT_LT(entity->GetWorldPosition().y, 5.0f);
    EXPECT_LT(rigidBody->GetBody()->GetLinearVelocity().y, 0.0f);

    world.Shutdown();
}

TEST(PhysicsWorldIntegrationValidation, RemovingRigidBodyComponentUnregistersPhysicsBody)
{
    RVX::WorldConfig config;
    config.name = "PhysicsRemovalWorld";
    config.autoInitializeSpatial = false;
    config.autoInitializePhysics = true;

    RVX::World world;
    world.Initialize(config);

    auto* physicsSubsystem = world.GetPhysics();
    ASSERT_NE(nullptr, physicsSubsystem);
    ASSERT_NE(nullptr, physicsSubsystem->GetPhysicsWorld());

    RVX::SceneEntity* entity = world.SpawnActor();
    ASSERT_NE(nullptr, entity);

    auto* rigidBody = entity->AddComponent<RVX::RigidBodyComponent>();
    ASSERT_NE(nullptr, rigidBody);

    world.Tick(1.0f / 60.0f);
    EXPECT_EQ(1u, physicsSubsystem->GetPhysicsWorld()->GetBodyCount());

    ASSERT_TRUE(entity->RemoveComponent<RVX::RigidBodyComponent>());
    EXPECT_EQ(0u, physicsSubsystem->GetPhysicsWorld()->GetBodyCount());

    world.Shutdown();
}

TEST(PhysicsWorldIntegrationValidation, WorldUnloadClearsRegisteredPhysicsBodies)
{
    RVX::WorldConfig config;
    config.name = "PhysicsUnloadWorld";
    config.autoInitializeSpatial = false;
    config.autoInitializePhysics = true;

    RVX::World world;
    world.Initialize(config);

    auto* physicsSubsystem = world.GetPhysics();
    ASSERT_NE(nullptr, physicsSubsystem);
    ASSERT_NE(nullptr, physicsSubsystem->GetPhysicsWorld());

    RVX::SceneEntity* entity = world.SpawnActor();
    ASSERT_NE(nullptr, entity);
    ASSERT_NE(nullptr, entity->AddComponent<RVX::RigidBodyComponent>());

    world.Tick(1.0f / 60.0f);
    EXPECT_EQ(1u, physicsSubsystem->GetPhysicsWorld()->GetBodyCount());

    world.Unload();
    EXPECT_EQ(0u, physicsSubsystem->GetPhysicsWorld()->GetBodyCount());

    world.Shutdown();
}
