#include "Core/Log.h"
#include "Scene/SceneRuntime.h"

#include <gtest/gtest.h>

#include <typeindex>

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

    class LifetimeActor final : public RVX::Actor
    {
    public:
        using Actor::Actor;
        const char* GetClassName() const override { return "LifetimeActor"; }
    };

    class LifetimeComponent final : public RVX::ActorComponent
    {
    public:
        const char* GetClassName() const override { return "LifetimeComponent"; }
    };
} // namespace

TEST(SceneLifetimeValidation, PendingDestroyIsImmediatelyHiddenAndReleasedAtEndFrame)
{
    RVX::Scene scene;
    ASSERT_TRUE(scene.Initialize());
    auto* actor = scene.SpawnActor<LifetimeActor>({.name = "Victim"});
    ASSERT_NE(actor, nullptr);
    auto* component = actor->AddComponent<LifetimeComponent>();
    ASSERT_NE(component, nullptr);

    const RVX::Actor::Handle actorHandle = actor->GetHandle();
    const RVX::ComponentHandle componentHandle = component->GetComponentHandle();
    scene.ClearComponentChanges();

    bool destroyAccepted = false;
    bool actorHiddenDuringPhase = false;
    bool componentHiddenDuringPhase = false;
    bool registryEmptyDuringPhase = false;
    scene.RegisterSystem(
        "destroy-victim",
        RVX::SceneUpdatePhase::FeatureSystems,
        [&](float)
        {
            destroyAccepted = scene.DestroyActor(actor);
            actorHiddenDuringPhase = scene.ResolveActor(actorHandle) == nullptr;
            componentHiddenDuringPhase =
                scene.ResolveComponent(componentHandle) == nullptr;
            registryEmptyDuringPhase =
                scene.GetComponents<LifetimeComponent>().empty();
        });

    scene.Tick(1.0f / 60.0f);

    EXPECT_TRUE(destroyAccepted);
    EXPECT_TRUE(actorHiddenDuringPhase);
    EXPECT_TRUE(componentHiddenDuringPhase);
    EXPECT_TRUE(registryEmptyDuringPhase);
    EXPECT_EQ(scene.ResolveActor(actorHandle), nullptr);
    EXPECT_EQ(scene.ResolveComponent(componentHandle), nullptr);
    EXPECT_EQ(scene.GetActorCount(), 0u);

    const auto& changes = scene.GetComponentChanges();
    ASSERT_EQ(changes.size(), 1u);
    EXPECT_EQ(changes.front().kind,
              RVX::SceneComponentChangeKind::Unregistered);
    EXPECT_EQ(changes.front().actor, actorHandle);
    EXPECT_EQ(changes.front().component, componentHandle);
    EXPECT_EQ(changes.front().componentType,
              std::type_index(typeid(LifetimeComponent)));

    auto* replacement =
        scene.SpawnActor<LifetimeActor>({.name = "Replacement"});
    ASSERT_NE(replacement, nullptr);
    auto* replacementComponent =
        replacement->AddComponent<LifetimeComponent>();
    ASSERT_NE(replacementComponent, nullptr);
    EXPECT_EQ(replacement->GetHandle().GetIndex(), actorHandle.GetIndex());
    EXPECT_NE(replacement->GetHandle().GetGeneration(),
              actorHandle.GetGeneration());
    EXPECT_EQ(replacementComponent->GetComponentHandle().GetIndex(),
              componentHandle.GetIndex());
    EXPECT_NE(replacementComponent->GetComponentHandle().GetGeneration(),
              componentHandle.GetGeneration());
}

TEST(SceneLifetimeValidation, DestroyingSceneEntityRecursivelyHidesItsSubtree)
{
    RVX::Scene scene;
    ASSERT_TRUE(scene.Initialize());
    auto* parent = scene.SpawnActor({.name = "Parent"});
    ASSERT_NE(parent, nullptr);
    auto* child = scene.SpawnActor({.name = "Child", .parent = parent});
    ASSERT_NE(child, nullptr);
    auto* parentComponent = parent->AddComponent<LifetimeComponent>();
    auto* childComponent = child->AddComponent<LifetimeComponent>();
    ASSERT_NE(parentComponent, nullptr);
    ASSERT_NE(childComponent, nullptr);

    const auto parentHandle = parent->GetHandle();
    const auto childHandle = child->GetHandle();
    const auto parentComponentHandle = parentComponent->GetComponentHandle();
    const auto childComponentHandle = childComponent->GetComponentHandle();

    bool subtreeHiddenDuringPhase = false;
    scene.RegisterSystem(
        "destroy-subtree",
        RVX::SceneUpdatePhase::Gameplay,
        [&](float)
        {
            ASSERT_TRUE(scene.DestroyActor(parent));
            subtreeHiddenDuringPhase =
                scene.ResolveActor(parentHandle) == nullptr &&
                scene.ResolveActor(childHandle) == nullptr &&
                scene.ResolveComponent(parentComponentHandle) == nullptr &&
                scene.ResolveComponent(childComponentHandle) == nullptr &&
                scene.GetComponents<LifetimeComponent>().empty();
        });

    scene.Tick(1.0f / 60.0f);

    EXPECT_TRUE(subtreeHiddenDuringPhase);
    EXPECT_EQ(scene.GetActorCount(), 0u);
    EXPECT_EQ(scene.ResolveActor(parentHandle), nullptr);
    EXPECT_EQ(scene.ResolveActor(childHandle), nullptr);
}
