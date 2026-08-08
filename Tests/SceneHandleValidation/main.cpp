#include "Core/Log.h"
#include "Scene/Components/CameraComponent.h"
#include "Scene/SceneRuntime.h"
#include "World/World.h"

#include <gtest/gtest.h>

namespace
{
    class LogEnvironment final : public ::testing::Environment
    {
    public:
        void SetUp() override { RVX::Log::Initialize(); }
        void TearDown() override { RVX::Log::Shutdown(); }
    };

    [[maybe_unused]] ::testing::Environment* const g_logEnvironment =
        ::testing::AddGlobalTestEnvironment(new LogEnvironment());

    class TestPureActor final : public RVX::Actor
    {
    public:
        using Actor::Actor;
        const char* GetClassName() const override { return "TestPureActor"; }
    };

    class TestRuntimeComponent final : public RVX::ActorComponent
    {
    public:
        const char* GetClassName() const override { return "TestRuntimeComponent"; }
    };

    class TickMutationActor final : public RVX::Actor
    {
    public:
        using Actor::Actor;
        const char* GetClassName() const override { return "TickMutationActor"; }

        void Tick(float) override
        {
            if (mutated)
                return;
            mutated = true;
            spawned = GetScene()->SpawnActor<TestPureActor>({.name = "Deferred"});
            component = AddComponent<TestRuntimeComponent>();
        }

        TestPureActor* spawned = nullptr;
        TestRuntimeComponent* component = nullptr;
        bool mutated = false;
    };

    class TickCancellationActor final : public RVX::Actor
    {
    public:
        using Actor::Actor;
        const char* GetClassName() const override { return "TickCancellationActor"; }

        void Tick(float) override
        {
            if (mutated)
                return;
            mutated = true;
            TestPureActor* spawned =
                GetScene()->SpawnActor<TestPureActor>({.name = "Cancelled"});
            spawnCancelled = GetScene()->DestroyActor(spawned);
            AddComponent<TestRuntimeComponent>();
            componentCancelled = RemoveComponent<TestRuntimeComponent>();
        }

        bool mutated = false;
        bool spawnCancelled = false;
        bool componentCancelled = false;
    };
}

TEST(SceneHandleValidation, ActorIdentityIsAssignedBySceneOwnership)
{
    RVX::Actor standaloneActor("Standalone");
    RVX::SceneEntity standaloneEntity("StandaloneEntity");
    EXPECT_FALSE(standaloneActor.GetHandle().IsValid());
    EXPECT_FALSE(standaloneEntity.GetHandle().IsValid());

    RVX::Scene scene;
    ASSERT_TRUE(scene.Initialize());
    auto* actor = scene.SpawnActor<TestPureActor>({});
    auto* entity = scene.SpawnActor({});
    ASSERT_NE(actor, nullptr);
    ASSERT_NE(entity, nullptr);
    EXPECT_TRUE(actor->GetHandle().IsValid());
    EXPECT_TRUE(entity->GetHandle().IsValid());
    EXPECT_NE(actor->GetHandle(), entity->GetHandle());
    EXPECT_EQ(scene.ResolveActor(actor->GetHandle()), actor);
    EXPECT_EQ(scene.ResolveActor(entity->GetHandle()), entity);
}

TEST(SceneHandleValidation, ReusedActorSlotInvalidatesStaleGeneration)
{
    RVX::Scene scene;
    ASSERT_TRUE(scene.Initialize());

    auto* first = scene.SpawnActor<TestPureActor>({});
    ASSERT_NE(first, nullptr);
    const RVX::Actor::Handle staleHandle = first->GetHandle();
    ASSERT_TRUE(scene.DestroyActor(first));
    EXPECT_EQ(scene.ResolveActor(staleHandle), nullptr);

    auto* replacement = scene.SpawnActor<TestPureActor>({});
    ASSERT_NE(replacement, nullptr);
    EXPECT_EQ(replacement->GetHandle().GetIndex(), staleHandle.GetIndex());
    EXPECT_NE(replacement->GetHandle().GetGeneration(), staleHandle.GetGeneration());
    EXPECT_EQ(scene.ResolveActor(staleHandle), nullptr);
    EXPECT_EQ(scene.ResolveActor(replacement->GetHandle()), replacement);
}

TEST(SceneHandleValidation, ComponentRuntimeIdentityIsSeparateFromPersistentId)
{
    RVX::Scene scene;
    ASSERT_TRUE(scene.Initialize());
    auto* actor = scene.SpawnActor<TestPureActor>({});
    ASSERT_NE(actor, nullptr);

    auto* first = actor->AddComponent<TestRuntimeComponent>();
    ASSERT_NE(first, nullptr);
    const RVX::PersistentComponentId persistentId = first->GetComponentId();
    const RVX::ComponentHandle staleHandle = first->GetComponentHandle();
    EXPECT_TRUE(staleHandle.IsValid());
    EXPECT_EQ(scene.ResolveComponent(staleHandle), first);

    ASSERT_TRUE(actor->RemoveComponent<TestRuntimeComponent>());
    EXPECT_EQ(scene.ResolveComponent(staleHandle), nullptr);

    auto* replacement = actor->AddComponent<TestRuntimeComponent>();
    ASSERT_NE(replacement, nullptr);
    EXPECT_NE(replacement->GetComponentId(), persistentId);
    EXPECT_EQ(replacement->GetComponentHandle().GetIndex(), staleHandle.GetIndex());
    EXPECT_NE(replacement->GetComponentHandle().GetGeneration(), staleHandle.GetGeneration());
}

TEST(SceneHandleValidation, TypedRegistryAndChangeFeedTrackDynamicComponents)
{
    RVX::Scene scene;
    ASSERT_TRUE(scene.Initialize());
    auto* actor = scene.SpawnActor<TestPureActor>({});
    ASSERT_NE(actor, nullptr);
    scene.ClearComponentChanges();

    auto* component = actor->AddComponent<TestRuntimeComponent>();
    ASSERT_NE(component, nullptr);
    auto typed = scene.GetComponents<TestRuntimeComponent>();
    ASSERT_EQ(typed.size(), 1u);
    EXPECT_EQ(typed.front(), component);
    ASSERT_EQ(scene.GetComponentChanges().size(), 1u);
    EXPECT_EQ(scene.GetComponentChanges().front().kind,
              RVX::SceneComponentChangeKind::Registered);

    ASSERT_TRUE(actor->RemoveComponent<TestRuntimeComponent>());
    EXPECT_TRUE(scene.GetComponents<TestRuntimeComponent>().empty());
    ASSERT_EQ(scene.GetComponentChanges().size(), 2u);
    EXPECT_EQ(scene.GetComponentChanges().back().kind,
              RVX::SceneComponentChangeKind::Unregistered);
}

TEST(SceneHandleValidation, ComponentValueChangesAdvanceSceneRevision)
{
    RVX::Scene scene;
    ASSERT_TRUE(scene.Initialize());
    auto* actor = scene.SpawnActor<TestPureActor>({});
    ASSERT_NE(actor, nullptr);
    auto* component = actor->AddComponent<TestRuntimeComponent>();
    ASSERT_NE(component, nullptr);
    scene.ClearComponentChanges();
    const RVX::uint64 before = scene.GetRevision();

    component->SetEnabled(false);
    EXPECT_GT(scene.GetRevision(), before);
    ASSERT_EQ(scene.GetComponentChanges().size(), 1u);
    EXPECT_EQ(scene.GetComponentChanges().front().kind,
              RVX::SceneComponentChangeKind::Updated);
    EXPECT_EQ(scene.GetComponentChanges().front().component,
              component->GetComponentHandle());

    const RVX::uint64 unchanged = scene.GetRevision();
    component->SetEnabled(false);
    EXPECT_EQ(scene.GetRevision(), unchanged);
    EXPECT_EQ(scene.GetComponentChanges().size(), 1u);
}

TEST(SceneHandleValidation, WorldOwnsExactlyOneAuthoritativeScene)
{
    RVX::World world;
    ASSERT_TRUE(world.Initialize());
    ASSERT_NE(world.GetScene(), nullptr);
    EXPECT_EQ(world.GetSceneManager(), world.GetScene()->GetSceneManager());

    auto* pureActor = world.SpawnActor<TestPureActor>({});
    auto* sceneActor = world.SpawnActor({});
    ASSERT_NE(pureActor, nullptr);
    ASSERT_NE(sceneActor, nullptr);
    EXPECT_EQ(world.GetActor(pureActor->GetHandle()), pureActor);
    EXPECT_EQ(world.GetActor(sceneActor->GetHandle()), sceneActor);
    EXPECT_NE(pureActor->GetHandle(), sceneActor->GetHandle());
}

TEST(SceneHandleValidation, TransformStoreIsAuthoritativeForSceneComponents)
{
    RVX::Scene scene;
    ASSERT_TRUE(scene.Initialize());
    auto* parentActor = scene.SpawnActor<TestPureActor>({});
    auto* childActor = scene.SpawnActor<TestPureActor>({});
    ASSERT_NE(parentActor, nullptr);
    ASSERT_NE(childActor, nullptr);

    auto* parent = parentActor->AddComponent<RVX::SceneComponent>();
    auto* child = childActor->AddComponent<RVX::SceneComponent>();
    ASSERT_NE(parent, nullptr);
    ASSERT_NE(child, nullptr);
    parentActor->SetRootComponent(parent);
    childActor->SetRootComponent(child);

    parent->SetRelativeLocation(RVX::Vec3(10.0f, 0.0f, 0.0f));
    child->SetRelativeLocation(RVX::Vec3(5.0f, 0.0f, 0.0f));
    EXPECT_TRUE(child->AttachToComponent(parent, RVX::AttachmentTransformRule::KeepWorld));
    EXPECT_FLOAT_EQ(child->GetWorldLocation().x, 5.0f);
    EXPECT_FLOAT_EQ(child->GetRelativeLocation().x, -5.0f);
    EXPECT_FALSE(parent->AttachToComponent(child));

    scene.GetTransformStore().BeginFrame();
    parent->SetRelativeLocation(RVX::Vec3(20.0f, 0.0f, 0.0f));
    scene.GetTransformStore().Resolve();
    EXPECT_FLOAT_EQ(child->GetWorldLocation().x, 15.0f);
    EXPECT_FLOAT_EQ(child->GetPreviousWorldTransform()[3].x, 5.0f);
    EXPECT_GT(child->GetWorldTransformRevision(), 0u);
}

TEST(SceneHandleValidation, ActiveCameraUsesGenerationSafeComponentHandle)
{
    RVX::Scene scene;
    ASSERT_TRUE(scene.Initialize());
    auto* actor = scene.SpawnActor({.name = "Camera"});
    ASSERT_NE(actor, nullptr);
    auto* camera = actor->AddComponent<RVX::CameraComponent>();
    ASSERT_NE(camera, nullptr);

    const RVX::ComponentHandle staleHandle = camera->GetComponentHandle();
    ASSERT_TRUE(scene.SetActiveCamera(staleHandle));
    EXPECT_EQ(scene.GetActiveCameraHandle(), staleHandle);
    EXPECT_EQ(scene.GetActiveCameraComponent(), camera);

    ASSERT_TRUE(actor->RemoveComponent<RVX::CameraComponent>());
    EXPECT_FALSE(scene.GetActiveCameraHandle().IsValid());
    EXPECT_EQ(scene.GetActiveCameraComponent(), nullptr);

    auto* replacement = actor->AddComponent<RVX::CameraComponent>();
    ASSERT_NE(replacement, nullptr);
    EXPECT_EQ(replacement->GetComponentHandle().GetIndex(),
              staleHandle.GetIndex());
    EXPECT_NE(replacement->GetComponentHandle().GetGeneration(),
              staleHandle.GetGeneration());
    EXPECT_FALSE(scene.SetActiveCamera(staleHandle));
    EXPECT_TRUE(scene.SetActiveCamera(replacement->GetComponentHandle()));
}

TEST(SceneHandleValidation, TickCreationsBecomeVisibleAtNextBeginFrame)
{
    RVX::Scene scene;
    ASSERT_TRUE(scene.Initialize());
    auto* mutator = scene.SpawnActor<TickMutationActor>({.name = "Mutator"});
    ASSERT_NE(mutator, nullptr);
    scene.ClearComponentChanges();

    scene.Tick(1.0f / 60.0f);
    ASSERT_NE(mutator->spawned, nullptr);
    ASSERT_NE(mutator->component, nullptr);
    EXPECT_FALSE(mutator->spawned->GetHandle().IsValid());
    EXPECT_FALSE(mutator->component->GetComponentHandle().IsValid());
    EXPECT_EQ(scene.GetActorCount(), 1u);
    EXPECT_TRUE(scene.GetComponents<TestRuntimeComponent>().empty());
    EXPECT_TRUE(scene.GetComponentChanges().empty());

    scene.Tick(1.0f / 60.0f);
    EXPECT_TRUE(mutator->spawned->GetHandle().IsValid());
    EXPECT_EQ(scene.ResolveActor(mutator->spawned->GetHandle()),
              mutator->spawned);
    EXPECT_TRUE(mutator->component->GetComponentHandle().IsValid());
    EXPECT_EQ(scene.GetActorCount(), 2u);
    ASSERT_EQ(scene.GetComponents<TestRuntimeComponent>().size(), 1u);
    EXPECT_EQ(scene.GetComponents<TestRuntimeComponent>().front(),
              mutator->component);
}

TEST(SceneHandleValidation, CreateThenRemoveDuringTickCoalescesToNoOp)
{
    RVX::Scene scene;
    ASSERT_TRUE(scene.Initialize());
    auto* mutator = scene.SpawnActor<TickCancellationActor>({.name = "Mutator"});
    ASSERT_NE(mutator, nullptr);
    scene.ClearComponentChanges();

    scene.Tick(1.0f / 60.0f);
    EXPECT_TRUE(mutator->spawnCancelled);
    EXPECT_TRUE(mutator->componentCancelled);
    EXPECT_EQ(scene.GetActorCount(), 1u);
    EXPECT_TRUE(scene.GetComponents<TestRuntimeComponent>().empty());
    EXPECT_TRUE(scene.GetComponentChanges().empty());

    scene.Tick(1.0f / 60.0f);
    EXPECT_EQ(scene.GetActorCount(), 1u);
    EXPECT_TRUE(scene.GetComponents<TestRuntimeComponent>().empty());
    EXPECT_TRUE(scene.GetComponentChanges().empty());
}
