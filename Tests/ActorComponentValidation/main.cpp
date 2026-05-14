#include "Core/Core.h"
#include "Scene/Actor.h"
#include "Scene/ActorComponent.h"
#include "Scene/ActorFactory.h"
#include "Scene/Component.h"
#include "Scene/ComponentFactory.h"
#include "Scene/PrimitiveComponent.h"
#include "Scene/SceneComponent.h"
#include "Scene/SceneEntity.h"
#include "TestFramework/TestRunner.h"

#include <string>
#include <type_traits>

using namespace RVX::Test;

namespace
{
    class TestComponent : public RVX::ActorComponent
    {
    public:
        const char* GetClassName() const override { return "TestComponent"; }

        void OnRegister() override { registered = true; }
        void InitializeComponent() override { initialized = true; }
        void BeginPlay() override { beganPlay = true; }
        void TickComponent(float deltaTime) override
        {
            ticked = true;
            lastDeltaTime = deltaTime;
        }
        void EndPlay() override { endedPlay = true; }
        void OnUnregister() override { unregistered = true; }

        bool registered = false;
        bool initialized = false;
        bool beganPlay = false;
        bool ticked = false;
        bool endedPlay = false;
        bool unregistered = false;
        float lastDeltaTime = 0.0f;
    };

    bool Test_ActorComponentLifecycleDefaults()
    {
        TestComponent component;
        TEST_ASSERT_TRUE(component.IsEnabled());
        TEST_ASSERT_FALSE(component.IsRegistered());
        TEST_ASSERT_FALSE(component.CanEverTick());
        TEST_ASSERT_FALSE(component.IsTickEnabled());

        component.SetCanEverTick(true);
        component.SetTickEnabled(true);
        TEST_ASSERT_TRUE(component.CanEverTick());
        TEST_ASSERT_TRUE(component.IsTickEnabled());
        TEST_ASSERT_EQ(std::string("TestComponent"), std::string(component.GetClassName()));
        return true;
    }

    class CountingComponent : public RVX::ActorComponent
    {
    public:
        const char* GetClassName() const override { return "CountingComponent"; }

        void OnComponentCreated() override { ++created; }
        void OnRegister() override { ++registered; }
        void InitializeComponent() override { ++initialized; }
        void BeginPlay() override { ++beganPlay; }
        void TickComponent(float) override { ++ticked; }
        void EndPlay() override { ++endedPlay; }
        void OnUnregister() override { ++unregistered; }
        void OnComponentDestroyed() override { ++destroyed; }

        int created = 0;
        int registered = 0;
        int initialized = 0;
        int beganPlay = 0;
        int ticked = 0;
        int endedPlay = 0;
        int unregistered = 0;
        int destroyed = 0;
    };

    bool Test_ActorOwnsComponentsAndDispatchesLifecycle()
    {
        RVX::Actor actor("LifecycleActor");
        auto* component = actor.AddComponent<CountingComponent>();
        component->SetCanEverTick(true);
        component->SetTickEnabled(true);

        TEST_ASSERT_EQ(&actor, component->GetOwner());
        TEST_ASSERT_EQ(1, component->created);

        actor.RegisterAllComponents();
        TEST_ASSERT_TRUE(component->IsRegistered());
        TEST_ASSERT_EQ(1, component->registered);
        TEST_ASSERT_EQ(1, component->initialized);

        actor.BeginPlay();
        actor.Tick(0.25f);
        actor.EndPlay();
        actor.UnregisterAllComponents();

        TEST_ASSERT_EQ(1, component->beganPlay);
        TEST_ASSERT_EQ(1, component->ticked);
        TEST_ASSERT_EQ(1, component->endedPlay);
        TEST_ASSERT_EQ(1, component->unregistered);
        return true;
    }

    bool Test_SceneComponentAttachmentComputesWorldTransform()
    {
        RVX::Actor actor("TransformActor");
        auto* root = actor.AddComponent<RVX::SceneComponent>();
        auto* child = actor.AddComponent<RVX::SceneComponent>();

        actor.SetRootComponent(root);
        root->SetRelativeLocation(RVX::Vec3(10.0f, 0.0f, 0.0f));
        child->SetRelativeLocation(RVX::Vec3(2.0f, 0.0f, 0.0f));
        TEST_ASSERT_TRUE(child->AttachToComponent(root));

        TEST_ASSERT_EQ(RVX::Vec3(12.0f, 0.0f, 0.0f), child->GetWorldLocation());
        TEST_ASSERT_EQ(root, child->GetAttachParent());
        TEST_ASSERT_EQ(static_cast<size_t>(1), root->GetAttachChildren().size());
        return true;
    }

    bool Test_PrimitiveComponentTracksVisibilityLayerAndBounds()
    {
        RVX::PrimitiveComponent primitive;
        primitive.SetVisible(false);
        primitive.SetLayerMask(0x04);
        primitive.SetLocalBounds(RVX::AABB(RVX::Vec3(-1.0f), RVX::Vec3(1.0f)));

        TEST_ASSERT_FALSE(primitive.IsVisible());
        TEST_ASSERT_EQ(static_cast<RVX::uint32>(0x04), primitive.GetLayerMask());
        TEST_ASSERT_TRUE(primitive.GetLocalBounds().IsValid());
        TEST_ASSERT_TRUE(primitive.GetWorldBounds().IsValid());
        return true;
    }

    class TestLegacyComponent : public RVX::Component
    {
    public:
        const char* GetTypeName() const override { return "TestLegacyComponent"; }
    };

    bool Test_SceneEntityAndComponentAreActorCompatible()
    {
        TEST_ASSERT_TRUE((std::is_base_of_v<RVX::Actor, RVX::SceneEntity>));
        TEST_ASSERT_TRUE((std::is_base_of_v<RVX::ActorComponent, RVX::Component>));

        RVX::SceneEntity entity("CompatEntity");
        auto* component = entity.AddComponent<TestLegacyComponent>();
        auto* actorComponent = static_cast<RVX::ActorComponent*>(component);

        TEST_ASSERT_NOT_NULL(component);
        TEST_ASSERT_EQ(&entity, component->GetOwner());
        TEST_ASSERT_EQ(static_cast<RVX::Actor*>(&entity), actorComponent->GetOwner());
        return true;
    }

    bool Test_ActorAndComponentFactoriesCreateRegisteredTypes()
    {
        RVX::ActorFactory::ClearAll();
        RVX::ActorFactory::Register<RVX::Actor>("Actor");
        auto actor = RVX::ActorFactory::Create("Actor");
        TEST_ASSERT_NOT_NULL(actor.get());
        TEST_ASSERT_EQ(std::string("Actor"), std::string(actor->GetClassName()));

        RVX::ComponentFactory::ClearComponentClasses();
        RVX::ComponentFactory::RegisterComponentClass<RVX::SceneComponent>("SceneComponent");
        auto component = RVX::ComponentFactory::CreateComponentByClassName("SceneComponent");
        TEST_ASSERT_NOT_NULL(component.get());
        TEST_ASSERT_EQ(std::string("SceneComponent"), std::string(component->GetClassName()));
        return true;
    }
} // namespace

int main()
{
    RVX::Log::Initialize();
    RVX_CORE_INFO("Actor Component Validation Tests");

    TestSuite suite;
    suite.AddTest("ActorComponentLifecycleDefaults", Test_ActorComponentLifecycleDefaults);
    suite.AddTest("ActorOwnsComponentsAndDispatchesLifecycle",
                  Test_ActorOwnsComponentsAndDispatchesLifecycle);
    suite.AddTest("SceneComponentAttachmentComputesWorldTransform",
                  Test_SceneComponentAttachmentComputesWorldTransform);
    suite.AddTest("PrimitiveComponentTracksVisibilityLayerAndBounds",
                  Test_PrimitiveComponentTracksVisibilityLayerAndBounds);
    suite.AddTest("SceneEntityAndComponentAreActorCompatible",
                  Test_SceneEntityAndComponentAreActorCompatible);
    suite.AddTest("ActorAndComponentFactoriesCreateRegisteredTypes",
                  Test_ActorAndComponentFactoriesCreateRegisteredTypes);

    auto results = suite.Run();
    suite.PrintResults(results);

    RVX::Log::Shutdown();

    for (const auto& result : results)
    {
        if (!result.passed)
            return 1;
    }

    return 0;
}
