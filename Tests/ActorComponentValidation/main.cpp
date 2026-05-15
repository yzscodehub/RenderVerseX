#include "Core/Core.h"
#include "Scene/Actor.h"
#include "Scene/ActorComponent.h"
#include "Scene/ActorFactory.h"
#include "Scene/Component.h"
#include "Scene/ComponentFactory.h"
#include "Scene/Components/StaticMeshComponent.h"
#include "Scene/PrimitiveComponent.h"
#include "Scene/SceneComponent.h"
#include "Scene/SceneEntity.h"
#include "Scene/SceneManager.h"
#include "TestFramework/TestRunner.h"

#include <memory>
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

    bool Test_StaticMeshComponentIsPrimitiveSceneComponent()
    {
        TEST_ASSERT_TRUE((std::is_base_of_v<RVX::PrimitiveComponent, RVX::StaticMeshComponent>));

        RVX::StaticMeshComponent component;
        TEST_ASSERT_EQ(std::string("StaticMeshComponent"), std::string(component.GetClassName()));
        TEST_ASSERT_FALSE(component.HasRenderData());
        TEST_ASSERT_FALSE(component.HasValidMesh());
        TEST_ASSERT_TRUE(component.IsVisible());
        return true;
    }

    bool Test_StaticMeshComponentRegistersWithSceneManager()
    {
        RVX::SceneManager sceneManager;
        sceneManager.Initialize();

        auto entity = std::make_shared<RVX::SceneEntity>("PreOwnedPrimitive");
        auto* primitive = static_cast<RVX::Actor*>(entity.get())->AddComponent<RVX::StaticMeshComponent>();
        TEST_ASSERT_NOT_NULL(primitive);
        TEST_ASSERT_FALSE(primitive->IsRegistered());

        const auto handle = entity->GetHandle();
        sceneManager.AddEntity(entity);

        TEST_ASSERT_TRUE(primitive->IsRegistered());
        TEST_ASSERT_EQ(static_cast<size_t>(1), sceneManager.GetPrimitives().size());
        TEST_ASSERT_EQ(primitive, sceneManager.GetPrimitives()[0]);

        sceneManager.DestroyEntity(handle);

        TEST_ASSERT_FALSE(primitive->IsRegistered());
        TEST_ASSERT_TRUE(sceneManager.GetPrimitives().empty());
        sceneManager.Shutdown();
        return true;
    }

    bool Test_RuntimeStaticMeshComponentRemovalUnregistersPrimitive()
    {
        RVX::SceneManager sceneManager;
        sceneManager.Initialize();

        const auto handle = sceneManager.CreateEntity("RuntimePrimitive");
        auto* entity = sceneManager.GetEntity(handle);
        TEST_ASSERT_NOT_NULL(entity);

        auto* primitive = static_cast<RVX::Actor*>(entity)->AddComponent<RVX::StaticMeshComponent>();
        TEST_ASSERT_NOT_NULL(primitive);
        TEST_ASSERT_TRUE(primitive->IsRegistered());
        TEST_ASSERT_EQ(static_cast<size_t>(1), sceneManager.GetPrimitives().size());

        TEST_ASSERT_TRUE(static_cast<RVX::Actor*>(entity)->RemoveComponent<RVX::StaticMeshComponent>());
        TEST_ASSERT_TRUE(sceneManager.GetPrimitives().empty());

        sceneManager.Shutdown();
        return true;
    }

    bool Test_SceneEntityAddComponentAutoRegistersStaticMeshPrimitive()
    {
        RVX::SceneManager sceneManager;
        sceneManager.Initialize();

        const auto handle = sceneManager.CreateEntity("LegacyAddComponentPrimitive");
        auto* entity = sceneManager.GetEntity(handle);
        TEST_ASSERT_NOT_NULL(entity);

        auto* primitive = entity->AddComponent<RVX::StaticMeshComponent>();
        TEST_ASSERT_NOT_NULL(primitive);
        TEST_ASSERT_TRUE(primitive->IsRegistered());
        TEST_ASSERT_EQ(static_cast<size_t>(1), sceneManager.GetPrimitives().size());
        TEST_ASSERT_EQ(primitive, sceneManager.GetPrimitives()[0]);

        sceneManager.Shutdown();
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

    bool Test_ActorTransformForwardsToRootComponent()
    {
        RVX::Actor actor("RootTransformActor");
        auto* root = actor.AddComponent<RVX::SceneComponent>();
        actor.SetRootComponent(root);

        actor.SetPosition(RVX::Vec3(3.0f, 4.0f, 5.0f));
        actor.SetScale(RVX::Vec3(2.0f, 3.0f, 4.0f));

        TEST_ASSERT_EQ(RVX::Vec3(3.0f, 4.0f, 5.0f), root->GetRelativeLocation());
        TEST_ASSERT_EQ(RVX::Vec3(3.0f, 4.0f, 5.0f), actor.GetWorldPosition());
        TEST_ASSERT_EQ(RVX::Vec3(2.0f, 3.0f, 4.0f), root->GetRelativeScale3D());
        TEST_ASSERT_EQ(RVX::Vec3(2.0f, 3.0f, 4.0f), actor.GetWorldScale());
        TEST_ASSERT_EQ(root->GetWorldTransform(), actor.GetWorldMatrix());
        return true;
    }

    bool Test_SceneEntityCreatesRootAndForwardsTransform()
    {
        RVX::SceneEntity entity("CompatTransformEntity");
        auto* root = entity.GetRootComponent();
        TEST_ASSERT_NOT_NULL(root);

        entity.SetPosition(RVX::Vec3(7.0f, 8.0f, 9.0f));
        entity.SetScale(RVX::Vec3(2.0f, 2.0f, 2.0f));

        TEST_ASSERT_EQ(RVX::Vec3(7.0f, 8.0f, 9.0f), entity.GetPosition());
        TEST_ASSERT_EQ(RVX::Vec3(7.0f, 8.0f, 9.0f), root->GetRelativeLocation());
        TEST_ASSERT_EQ(RVX::Vec3(7.0f, 8.0f, 9.0f), entity.GetWorldPosition());
        TEST_ASSERT_EQ(RVX::Vec3(2.0f, 2.0f, 2.0f), entity.GetScale());
        TEST_ASSERT_EQ(root->GetWorldTransform(), entity.GetWorldMatrix());
        return true;
    }

    bool Test_SceneEntityHierarchyAttachesRootComponents()
    {
        RVX::SceneEntity parent("ParentEntity");
        RVX::SceneEntity child("ChildEntity");

        parent.SetPosition(RVX::Vec3(10.0f, 0.0f, 0.0f));
        child.SetPosition(RVX::Vec3(2.0f, 0.0f, 0.0f));

        parent.AddChild(&child);

        TEST_ASSERT_EQ(parent.GetRootComponent(), child.GetRootComponent()->GetAttachParent());
        TEST_ASSERT_EQ(RVX::Vec3(12.0f, 0.0f, 0.0f), child.GetWorldPosition());

        parent.SetPosition(RVX::Vec3(20.0f, 0.0f, 0.0f));
        TEST_ASSERT_EQ(RVX::Vec3(22.0f, 0.0f, 0.0f), child.GetWorldPosition());

        TEST_ASSERT_TRUE(parent.RemoveChild(&child));
        TEST_ASSERT_EQ(nullptr, child.GetRootComponent()->GetAttachParent());
        TEST_ASSERT_EQ(RVX::Vec3(2.0f, 0.0f, 0.0f), child.GetWorldPosition());
        return true;
    }

    bool Test_SceneEntityTransformStaysSyncedThroughActorAndRootPaths()
    {
        RVX::SceneEntity entity("SyncEntity");
        RVX::Actor* actor = &entity;

        actor->SetPosition(RVX::Vec3(3.0f, 0.0f, 0.0f));
        entity.Translate(RVX::Vec3(2.0f, 0.0f, 0.0f));
        TEST_ASSERT_EQ(RVX::Vec3(5.0f, 0.0f, 0.0f), entity.GetPosition());

        entity.GetRootComponent()->SetRelativeLocation(RVX::Vec3(4.0f, 0.0f, 0.0f));
        entity.Translate(RVX::Vec3(1.0f, 0.0f, 0.0f));
        TEST_ASSERT_EQ(RVX::Vec3(5.0f, 0.0f, 0.0f), entity.GetPosition());

        entity.GetRootComponent()->SetRelativeLocation(RVX::Vec3(9.0f, 0.0f, 0.0f));
        entity.SetPosition(RVX::Vec3(5.0f, 0.0f, 0.0f));
        TEST_ASSERT_EQ(RVX::Vec3(5.0f, 0.0f, 0.0f), entity.GetRootComponent()->GetRelativeLocation());
        return true;
    }

    bool Test_SceneEntityDestructionMaintainsHierarchy()
    {
        RVX::SceneEntity parent("PersistentParent");
        {
            auto child = std::make_unique<RVX::SceneEntity>("TemporaryChild");
            parent.AddChild(child.get());
            TEST_ASSERT_EQ(static_cast<size_t>(1), parent.GetChildCount());
        }
        TEST_ASSERT_EQ(static_cast<size_t>(0), parent.GetChildCount());

        auto child = std::make_unique<RVX::SceneEntity>("PersistentChild");
        {
            auto temporaryParent = std::make_unique<RVX::SceneEntity>("TemporaryParent");
            temporaryParent->AddChild(child.get());
            TEST_ASSERT_EQ(temporaryParent.get(), child->GetParent());
        }

        TEST_ASSERT_EQ(nullptr, child->GetParent());
        TEST_ASSERT_EQ(nullptr, child->GetRootComponent()->GetAttachParent());
        return true;
    }

    bool Test_SceneEntityCompatRootCannotBeRemovedThroughActorAPI()
    {
        RVX::SceneEntity entity("RootProtectedEntity");
        RVX::Actor* actor = &entity;
        auto* root = entity.GetRootComponent();

        TEST_ASSERT_NOT_NULL(root);
        TEST_ASSERT_FALSE(actor->RemoveComponent<RVX::SceneComponent>());
        TEST_ASSERT_EQ(root, entity.GetRootComponent());

        entity.SetPosition(RVX::Vec3(6.0f, 0.0f, 0.0f));
        TEST_ASSERT_EQ(RVX::Vec3(6.0f, 0.0f, 0.0f), entity.GetWorldPosition());
        return true;
    }

    bool Test_SceneEntityRootReplacementKeepsCompatibility()
    {
        RVX::SceneEntity parent("ReplacementParent");
        RVX::SceneEntity child("ReplacementChild");
        RVX::Actor* childActor = &child;

        parent.SetPosition(RVX::Vec3(10.0f, 0.0f, 0.0f));
        child.SetPosition(RVX::Vec3(2.0f, 0.0f, 0.0f));
        parent.AddChild(&child);

        auto* originalRoot = child.GetRootComponent();
        childActor->SetRootComponent(nullptr);
        TEST_ASSERT_EQ(originalRoot, child.GetRootComponent());

        auto* replacementRoot = childActor->AddComponent<RVX::SceneComponent>();
        replacementRoot->SetRelativeLocation(RVX::Vec3(3.0f, 0.0f, 0.0f));

        childActor->SetRootComponent(replacementRoot);

        TEST_ASSERT_EQ(replacementRoot, child.GetRootComponent());
        TEST_ASSERT_EQ(parent.GetRootComponent(), replacementRoot->GetAttachParent());
        TEST_ASSERT_EQ(RVX::Vec3(3.0f, 0.0f, 0.0f), child.GetPosition());
        TEST_ASSERT_EQ(RVX::Vec3(13.0f, 0.0f, 0.0f), child.GetWorldPosition());
        return true;
    }

    bool Test_RootComponentTransformMarksSceneEntityDirty()
    {
        RVX::SceneEntity entity("RootDirtyEntity");
        entity.SetLocalBounds(RVX::AABB(RVX::Vec3(-1.0f), RVX::Vec3(1.0f)));

        auto initialBounds = entity.GetWorldBounds();
        TEST_ASSERT_EQ(RVX::Vec3(0.0f), initialBounds.GetCenter());

        entity.ClearSpatialDirty();
        TEST_ASSERT_FALSE(entity.IsSpatialDirty());

        entity.GetRootComponent()->SetRelativeLocation(RVX::Vec3(10.0f, 0.0f, 0.0f));

        TEST_ASSERT_TRUE(entity.IsSpatialDirty());
        auto updatedBounds = entity.GetWorldBounds();
        TEST_ASSERT_EQ(RVX::Vec3(10.0f, 0.0f, 0.0f), updatedBounds.GetCenter());
        return true;
    }

    bool Test_SceneEntityRootReplacementRejectsInvalidAttachment()
    {
        RVX::SceneEntity parent("CycleParent");
        RVX::SceneEntity child("CycleChild");
        RVX::Actor* childActor = &child;

        parent.AddChild(&child);
        auto* originalRoot = child.GetRootComponent();
        auto* replacementRoot = childActor->AddComponent<RVX::SceneComponent>();

        TEST_ASSERT_TRUE(parent.GetRootComponent()->AttachToComponent(replacementRoot));

        childActor->SetRootComponent(replacementRoot);

        TEST_ASSERT_EQ(originalRoot, child.GetRootComponent());
        TEST_ASSERT_EQ(parent.GetRootComponent(), originalRoot->GetAttachParent());

        parent.GetRootComponent()->DetachFromComponent();
        return true;
    }

    bool Test_SceneEntityRootReplacementDetachesExternalParentWhenRootEntity()
    {
        RVX::SceneEntity externalParent("ExternalParent");
        RVX::SceneEntity entity("RootReplacementEntity");
        RVX::Actor* actor = &entity;

        externalParent.SetPosition(RVX::Vec3(100.0f, 0.0f, 0.0f));
        auto* replacementRoot = actor->AddComponent<RVX::SceneComponent>();
        replacementRoot->SetRelativeLocation(RVX::Vec3(5.0f, 0.0f, 0.0f));

        TEST_ASSERT_TRUE(replacementRoot->AttachToComponent(externalParent.GetRootComponent()));

        actor->SetRootComponent(replacementRoot);

        TEST_ASSERT_EQ(replacementRoot, entity.GetRootComponent());
        TEST_ASSERT_EQ(nullptr, replacementRoot->GetAttachParent());
        TEST_ASSERT_EQ(RVX::Vec3(5.0f, 0.0f, 0.0f), entity.GetWorldPosition());
        return true;
    }

    bool Test_SceneEntityAddChildRejectsInconsistentComponentCycle()
    {
        RVX::SceneEntity parent("CycleAttachmentParent");
        RVX::SceneEntity child("CycleAttachmentChild");

        TEST_ASSERT_TRUE(parent.GetRootComponent()->AttachToComponent(child.GetRootComponent()));

        parent.AddChild(&child);

        TEST_ASSERT_EQ(nullptr, child.GetParent());
        TEST_ASSERT_EQ(static_cast<size_t>(0), parent.GetChildCount());
        TEST_ASSERT_EQ(child.GetRootComponent(), parent.GetRootComponent()->GetAttachParent());

        parent.GetRootComponent()->DetachFromComponent();
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
    suite.AddTest("StaticMeshComponentIsPrimitiveSceneComponent",
                  Test_StaticMeshComponentIsPrimitiveSceneComponent);
    suite.AddTest("StaticMeshComponentRegistersWithSceneManager",
                  Test_StaticMeshComponentRegistersWithSceneManager);
    suite.AddTest("RuntimeStaticMeshComponentRemovalUnregistersPrimitive",
                  Test_RuntimeStaticMeshComponentRemovalUnregistersPrimitive);
    suite.AddTest("SceneEntityAddComponentAutoRegistersStaticMeshPrimitive",
                  Test_SceneEntityAddComponentAutoRegistersStaticMeshPrimitive);
    suite.AddTest("SceneEntityAndComponentAreActorCompatible",
                  Test_SceneEntityAndComponentAreActorCompatible);
    suite.AddTest("ActorAndComponentFactoriesCreateRegisteredTypes",
                  Test_ActorAndComponentFactoriesCreateRegisteredTypes);
    suite.AddTest("ActorTransformForwardsToRootComponent",
                  Test_ActorTransformForwardsToRootComponent);
    suite.AddTest("SceneEntityCreatesRootAndForwardsTransform",
                  Test_SceneEntityCreatesRootAndForwardsTransform);
    suite.AddTest("SceneEntityHierarchyAttachesRootComponents",
                  Test_SceneEntityHierarchyAttachesRootComponents);
    suite.AddTest("SceneEntityTransformStaysSyncedThroughActorAndRootPaths",
                  Test_SceneEntityTransformStaysSyncedThroughActorAndRootPaths);
    suite.AddTest("SceneEntityDestructionMaintainsHierarchy",
                  Test_SceneEntityDestructionMaintainsHierarchy);
    suite.AddTest("SceneEntityCompatRootCannotBeRemovedThroughActorAPI",
                  Test_SceneEntityCompatRootCannotBeRemovedThroughActorAPI);
    suite.AddTest("SceneEntityRootReplacementKeepsCompatibility",
                  Test_SceneEntityRootReplacementKeepsCompatibility);
    suite.AddTest("RootComponentTransformMarksSceneEntityDirty",
                  Test_RootComponentTransformMarksSceneEntityDirty);
    suite.AddTest("SceneEntityRootReplacementRejectsInvalidAttachment",
                  Test_SceneEntityRootReplacementRejectsInvalidAttachment);
    suite.AddTest("SceneEntityRootReplacementDetachesExternalParentWhenRootEntity",
                  Test_SceneEntityRootReplacementDetachesExternalParentWhenRootEntity);
    suite.AddTest("SceneEntityAddChildRejectsInconsistentComponentCycle",
                  Test_SceneEntityAddChildRejectsInconsistentComponentCycle);

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
