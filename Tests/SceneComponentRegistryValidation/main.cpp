#include "Core/Log.h"
#include "Scene/SceneRuntime.h"

#include <gtest/gtest.h>

#include <algorithm>
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

    class RegistryActor final : public RVX::Actor
    {
    public:
        using Actor::Actor;
        const char* GetClassName() const override { return "RegistryActor"; }
    };

    class RegistryBaseComponent : public RVX::ActorComponent
    {
    public:
        const char* GetClassName() const override
        {
            return "RegistryBaseComponent";
        }
    };

    class RegistryComponentA final : public RegistryBaseComponent
    {
    public:
        const char* GetClassName() const override { return "RegistryComponentA"; }
    };

    class RegistryComponentB final : public RegistryBaseComponent
    {
    public:
        const char* GetClassName() const override { return "RegistryComponentB"; }
    };

    class DeferredRegistryActor final : public RVX::Actor
    {
    public:
        using Actor::Actor;
        const char* GetClassName() const override { return "DeferredRegistryActor"; }

        void Tick(float) override
        {
            if (!component)
                component = AddComponent<RegistryComponentA>();
        }

        RegistryComponentA* component = nullptr;
    };
} // namespace

TEST(SceneComponentRegistryValidation, ExactAndInterfaceQueriesUseOneRegistry)
{
    RVX::Scene scene;
    ASSERT_TRUE(scene.Initialize());
    auto* firstActor = scene.SpawnActor<RegistryActor>({.name = "First"});
    auto* secondActor = scene.SpawnActor<RegistryActor>({.name = "Second"});
    ASSERT_NE(firstActor, nullptr);
    ASSERT_NE(secondActor, nullptr);
    scene.ClearComponentChanges();

    auto* componentB = firstActor->AddComponent<RegistryComponentB>();
    auto* componentA = secondActor->AddComponent<RegistryComponentA>();
    ASSERT_NE(componentA, nullptr);
    ASSERT_NE(componentB, nullptr);

    EXPECT_TRUE(scene.GetComponents<RegistryBaseComponent>().empty());
    ASSERT_EQ(scene.GetComponents<RegistryComponentA>().size(), 1u);
    ASSERT_EQ(scene.GetComponents<RegistryComponentB>().size(), 1u);

    const auto implementing =
        scene.GetComponentsImplementing<RegistryBaseComponent>();
    ASSERT_EQ(implementing.size(), 2u);
    EXPECT_TRUE(std::is_sorted(
        implementing.begin(), implementing.end(),
        [](const RegistryBaseComponent* left,
           const RegistryBaseComponent* right)
        {
            return left->GetComponentHandle() < right->GetComponentHandle();
        }));

    const auto& registeredChanges = scene.GetComponentChanges();
    ASSERT_EQ(registeredChanges.size(), 2u);
    EXPECT_EQ(registeredChanges[0].kind,
              RVX::SceneComponentChangeKind::Registered);
    EXPECT_EQ(registeredChanges[1].kind,
              RVX::SceneComponentChangeKind::Registered);
    EXPECT_LT(registeredChanges[0].sceneRevision,
              registeredChanges[1].sceneRevision);

    componentA->SetEnabled(false);
    ASSERT_TRUE(firstActor->RemoveComponent<RegistryComponentB>());

    const auto& allChanges = scene.GetComponentChanges();
    ASSERT_EQ(allChanges.size(), 4u);
    EXPECT_EQ(allChanges[2].kind, RVX::SceneComponentChangeKind::Updated);
    EXPECT_EQ(allChanges[2].component, componentA->GetComponentHandle());
    EXPECT_EQ(allChanges[3].kind,
              RVX::SceneComponentChangeKind::Unregistered);
    EXPECT_EQ(allChanges[3].componentType,
              std::type_index(typeid(RegistryComponentB)));
    EXPECT_LT(allChanges[2].sceneRevision, allChanges[3].sceneRevision);
    EXPECT_TRUE(scene.GetComponents<RegistryComponentB>().empty());
    ASSERT_EQ(scene.GetComponentsImplementing<RegistryBaseComponent>().size(),
              1u);
}

TEST(SceneComponentRegistryValidation, RegistrationRequestedDuringTickIsDeferred)
{
    RVX::Scene scene;
    ASSERT_TRUE(scene.Initialize());
    auto* actor =
        scene.SpawnActor<DeferredRegistryActor>({.name = "Deferred"});
    ASSERT_NE(actor, nullptr);
    scene.ClearComponentChanges();

    scene.Tick(1.0f / 60.0f);
    ASSERT_NE(actor->component, nullptr);
    EXPECT_FALSE(actor->component->GetComponentHandle().IsValid());
    EXPECT_TRUE(scene.GetComponents<RegistryComponentA>().empty());
    EXPECT_TRUE(scene.GetComponentChanges().empty());

    scene.Tick(1.0f / 60.0f);
    EXPECT_TRUE(actor->component->GetComponentHandle().IsValid());
    ASSERT_EQ(scene.GetComponents<RegistryComponentA>().size(), 1u);
    EXPECT_EQ(scene.GetComponents<RegistryComponentA>().front(),
              actor->component);
    ASSERT_EQ(scene.GetComponentChanges().size(), 1u);
    EXPECT_EQ(scene.GetComponentChanges().front().kind,
              RVX::SceneComponentChangeKind::Registered);
}
