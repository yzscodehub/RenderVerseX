#include "Core/Core.h"
#include "Resource/Types/MaterialResource.h"
#include "Resource/Types/MeshResource.h"
#include "Resource/Types/ModelResource.h"
#include "Scene/Actor.h"
#include "Scene/Component.h"
#include "Scene/ComponentFactory.h"
#include "Scene/Components/MeshRendererComponent.h"
#include "Scene/Components/StaticMeshComponent.h"
#include "Scene/Mesh.h"
#include "Scene/Node.h"
#include "Scene/Prefab.h"
#include "Scene/SceneManager.h"
#include "TestFramework/TestRunner.h"

#include <algorithm>
#include <memory>
#include <string>

using namespace RVX;
using namespace RVX::Resource;
using namespace RVX::Test;

namespace
{
    class TestMeshResource : public MeshResource
    {
    public:
        void MarkLoaded() { SetState(ResourceState::Loaded); }
    };

    class TestMaterialResource : public MaterialResource
    {
    public:
        void MarkLoaded() { SetState(ResourceState::Loaded); }
    };

    ResourceHandle<MeshResource> MakeMeshResource(ResourceId id)
    {
        auto* resource = new TestMeshResource();
        resource->SetId(id);
        resource->SetName("TestMesh");
        resource->SetMesh(MeshFactory::CreateTriangle());
        resource->SetBounds(AABB(Vec3(-1.0f), Vec3(1.0f)));
        resource->MarkLoaded();
        return ResourceHandle<MeshResource>(resource);
    }

    ResourceHandle<MaterialResource> MakeMaterialResource(ResourceId id)
    {
        auto* resource = new TestMaterialResource();
        resource->SetId(id);
        resource->SetName("TestMaterial");
        resource->MarkLoaded();
        return ResourceHandle<MaterialResource>(resource);
    }

    void FillIndexedModel(ModelResource& model)
    {
        model.AddMesh(MakeMeshResource(1001));
        model.AddMaterial(MakeMaterialResource(2001));

        auto root = std::make_shared<Node>("RootNode");
        root->SetMeshIndex(0);
        root->SetMaterialIndices({0});
        root->GetLocalTransform().SetPosition(Vec3(3.0f, 0.0f, 0.0f));
        model.SetRootNode(root);
    }

    void FillHierarchicalIndexedModel(ModelResource& model)
    {
        model.AddMesh(MakeMeshResource(1101));
        model.AddMaterial(MakeMaterialResource(2101));

        auto root = std::make_shared<Node>("RootNode");
        root->GetLocalTransform().SetPosition(Vec3(1.0f, 2.0f, 3.0f));

        auto child = std::make_shared<Node>("ChildNode");
        child->SetMeshIndex(0);
        child->SetMaterialIndices({0});
        child->GetLocalTransform().SetPosition(Vec3(4.0f, 5.0f, 6.0f));
        root->AddChild(child);

        model.SetRootNode(root);
    }

    bool Test_ActorAddOwnedComponentUsesNormalLifecycle()
    {
        class OwnedLifecycleComponent : public ActorComponent
        {
        public:
            const char* GetClassName() const override { return "OwnedLifecycleComponent"; }
            void OnComponentCreated() override { ++created; }
            void OnRegister() override { ++registered; }
            void InitializeComponent() override { ++initialized; }

            int created = 0;
            int registered = 0;
            int initialized = 0;
        };

        SceneManager sceneManager;
        sceneManager.Initialize();

        const auto handle = sceneManager.CreateEntity("OwnedComponentEntity");
        auto* entity = sceneManager.GetEntity(handle);
        auto component = std::make_unique<OwnedLifecycleComponent>();
        auto* raw = component.get();

        ActorComponent* inserted = static_cast<Actor*>(entity)->AddOwnedComponent(std::move(component));

        TEST_ASSERT_EQ(raw, inserted);
        TEST_ASSERT_EQ(static_cast<Actor*>(entity), raw->GetOwner());
        TEST_ASSERT_TRUE(raw->IsRegistered());
        TEST_ASSERT_TRUE(raw->IsInitialized());
        TEST_ASSERT_EQ(1, raw->created);
        TEST_ASSERT_EQ(1, raw->registered);
        TEST_ASSERT_EQ(1, raw->initialized);

        sceneManager.Shutdown();
        return true;
    }

    bool Test_AddOwnedComponentRejectsLegacyComponentToAvoidContainerSplit()
    {
        class OwnedLegacyComponent : public Component
        {
        public:
            const char* GetTypeName() const override { return "OwnedLegacyComponent"; }
        };

        SceneManager sceneManager;
        sceneManager.Initialize();

        const auto handle = sceneManager.CreateEntity("LegacyOwnedEntity");
        auto* entity = sceneManager.GetEntity(handle);
        auto* actor = static_cast<Actor*>(entity);
        const size_t initialActorCount = actor->GetActorComponentCount();
        const size_t initialLegacyCount = entity->GetComponentCount();

        ActorComponent* inserted = actor->AddOwnedComponent(std::make_unique<OwnedLegacyComponent>());

        TEST_ASSERT_EQ(nullptr, inserted);
        TEST_ASSERT_EQ(initialActorCount, actor->GetActorComponentCount());
        TEST_ASSERT_EQ(initialLegacyCount, entity->GetComponentCount());
        TEST_ASSERT_FALSE(entity->HasComponent<OwnedLegacyComponent>());

        sceneManager.Shutdown();
        return true;
    }

    bool Test_ModelResourceInstantiateActorUsesStaticMeshComponent()
    {
        ComponentFactory::RegisterDefaults();

        SceneManager sceneManager;
        sceneManager.Initialize();

        ModelResource model;
        FillIndexedModel(model);
        Actor* actor = model.InstantiateActor(&sceneManager);
        TEST_ASSERT_NOT_NULL(actor);

        auto* entity = dynamic_cast<SceneEntity*>(actor);
        TEST_ASSERT_NOT_NULL(entity);

        auto* primitive = actor->GetComponent<StaticMeshComponent>();
        TEST_ASSERT_NOT_NULL(primitive);
        TEST_ASSERT_TRUE(primitive->IsRegistered());
        TEST_ASSERT_EQ(actor, primitive->GetOwner());
        TEST_ASSERT_EQ(primitive, entity->GetComponent<StaticMeshComponent>());
        TEST_ASSERT_TRUE(entity->HasComponent<StaticMeshComponent>());
        TEST_ASSERT_FALSE(entity->HasComponent<MeshRendererComponent>());
        TEST_ASSERT_EQ(entity->GetRootComponent(), primitive->GetAttachParent());
        TEST_ASSERT_TRUE(primitive->HasValidMesh());
        TEST_ASSERT_EQ(static_cast<ResourceId>(1001), primitive->GetMesh().GetId());
        TEST_ASSERT_EQ(static_cast<ResourceId>(2001), primitive->GetMaterial(0).GetId());
        TEST_ASSERT_EQ(Vec3(3.0f, 0.0f, 0.0f), entity->GetPosition());
        TEST_ASSERT_TRUE(std::find(sceneManager.GetPrimitives().begin(),
                                   sceneManager.GetPrimitives().end(),
                                   primitive) != sceneManager.GetPrimitives().end());

        sceneManager.Shutdown();
        return true;
    }

    bool Test_LegacyInstantiateDelegatesToActorPath()
    {
        ComponentFactory::RegisterDefaults();

        SceneManager sceneManager;
        sceneManager.Initialize();

        ModelResource model;
        FillIndexedModel(model);
        SceneEntity* entity = model.Instantiate(&sceneManager);
        TEST_ASSERT_NOT_NULL(entity);
        TEST_ASSERT_NOT_NULL(static_cast<Actor*>(entity)->GetComponent<StaticMeshComponent>());
        TEST_ASSERT_TRUE(entity->HasComponent<StaticMeshComponent>());
        TEST_ASSERT_FALSE(entity->HasComponent<MeshRendererComponent>());

        sceneManager.Shutdown();
        return true;
    }

    bool Test_ModelResourceInstantiateActorBuildsSpawnedHierarchy()
    {
        ComponentFactory::RegisterDefaults();

        SceneManager sceneManager;
        sceneManager.Initialize();

        ModelResource model;
        FillHierarchicalIndexedModel(model);

        Actor* actor = model.InstantiateActor(&sceneManager);
        TEST_ASSERT_NOT_NULL(actor);

        auto* root = dynamic_cast<SceneEntity*>(actor);
        TEST_ASSERT_NOT_NULL(root);
        TEST_ASSERT_EQ(std::string("RootNode"), root->GetName());
        TEST_ASSERT_EQ(Vec3(1.0f, 2.0f, 3.0f), root->GetPosition());
        TEST_ASSERT_EQ(static_cast<size_t>(1), root->GetChildren().size());
        TEST_ASSERT_EQ(static_cast<size_t>(2), sceneManager.GetEntityCount());

        auto* child = root->GetChildren()[0];
        TEST_ASSERT_NOT_NULL(child);
        TEST_ASSERT_EQ(std::string("ChildNode"), child->GetName());
        TEST_ASSERT_EQ(root, child->GetParent());
        TEST_ASSERT_EQ(Vec3(4.0f, 5.0f, 6.0f), child->GetPosition());
        TEST_ASSERT_NOT_NULL(static_cast<Actor*>(child)->GetComponent<StaticMeshComponent>());

        sceneManager.Shutdown();
        return true;
    }

    bool Test_ComponentFactoryDefaultCreatesStaticMeshComponent()
    {
        ComponentFactory::ClearAll();
        ComponentFactory::RegisterDefaults();

        SceneManager sceneManager;
        sceneManager.Initialize();

        const auto handle = sceneManager.CreateEntity("FactoryEntity");
        auto* entity = sceneManager.GetEntity(handle);
        ModelResource model;
        FillIndexedModel(model);

        ActorComponent* component = ComponentFactory::CreateComponent("StaticMesh",
                                                                      entity,
                                                                      model.GetRootNode().get(),
                                                                      &model);
        TEST_ASSERT_NOT_NULL(component);
        auto* primitive = dynamic_cast<StaticMeshComponent*>(component);
        TEST_ASSERT_NOT_NULL(primitive);
        TEST_ASSERT_EQ(entity->GetRootComponent(), primitive->GetAttachParent());
        TEST_ASSERT_EQ(static_cast<ResourceId>(1001), primitive->GetMesh().GetId());

        sceneManager.Shutdown();
        return true;
    }

    bool Test_ComponentFactoryRegisterDefaultsPreservesCustomCreators()
    {
        ComponentFactory::ClearAll();
        ComponentFactory::Register("CustomNoop",
                                   [](SceneEntity*, const Node*, const ModelResource*) -> ActorComponent* {
                                       return nullptr;
                                   });

        ComponentFactory::RegisterDefaults();

        TEST_ASSERT_TRUE(ComponentFactory::IsRegistered("CustomNoop"));
        TEST_ASSERT_TRUE(ComponentFactory::IsRegistered("StaticMesh"));

        ComponentFactory::ClearAll();
        return true;
    }

    bool Test_PrefabSerializesActorComponentClassNames()
    {
        SceneManager sceneManager;
        sceneManager.Initialize();

        const auto handle = sceneManager.CreateEntity("PrefabSource");
        auto* entity = sceneManager.GetEntity(handle);
        auto* primitive = static_cast<Actor*>(entity)->AddComponent<StaticMeshComponent>();
        TEST_ASSERT_NOT_NULL(primitive);
        auto* legacyRenderer = entity->AddComponent<MeshRendererComponent>();
        TEST_ASSERT_NOT_NULL(legacyRenderer);

        auto prefab = Prefab::CreateFromEntity(entity);
        TEST_ASSERT_NOT_NULL(prefab.get());
        const auto* data = prefab->GetRootData();
        TEST_ASSERT_NOT_NULL(data);
        bool foundStaticMesh = false;
        bool foundLegacyRenderer = false;
        for (const auto& componentData : data->actorComponentData)
        {
            foundStaticMesh = foundStaticMesh || componentData.className == "StaticMeshComponent";
            foundLegacyRenderer = foundLegacyRenderer || componentData.className == "MeshRenderer";
        }
        TEST_ASSERT_TRUE(foundStaticMesh);
        TEST_ASSERT_FALSE(foundLegacyRenderer);
        TEST_ASSERT_TRUE(data->componentData.find("MeshRenderer") != data->componentData.end());

        sceneManager.Shutdown();
        return true;
    }

    bool Test_PrefabInstantiatesRegisteredActorComponentClasses()
    {
        ComponentFactory::ClearComponentClasses();
        ComponentFactory::RegisterComponentClass<StaticMeshComponent>("StaticMeshComponent");

        PrefabEntityData data;
        data.name = "PrefabActor";
        data.actorComponentData.push_back({"StaticMeshComponent", ""});
        data.actorComponentData.push_back({"StaticMeshComponent", ""});

        auto prefab = Prefab::CreateFromData({data});

        SceneManager sceneManager;
        sceneManager.Initialize();

        SceneEntity* instance = prefab->Instantiate(sceneManager);
        TEST_ASSERT_NOT_NULL(instance);
        auto* firstPrimitive = static_cast<Actor*>(instance)->GetComponent<StaticMeshComponent>();
        TEST_ASSERT_NOT_NULL(firstPrimitive);
        TEST_ASSERT_TRUE(firstPrimitive->IsRegistered());

        size_t staticMeshCount = 0;
        for (const auto& component : static_cast<Actor*>(instance)->GetActorComponents())
        {
            if (component && std::string(component->GetClassName()) == "StaticMeshComponent")
            {
                ++staticMeshCount;
            }
        }
        TEST_ASSERT_EQ(static_cast<size_t>(2), staticMeshCount);
        TEST_ASSERT_EQ(static_cast<size_t>(2), sceneManager.GetPrimitives().size());

        sceneManager.Shutdown();
        return true;
    }

    bool Test_PrefabInstantiateAsChildBuildsSpawnedHierarchy()
    {
        ComponentFactory::ClearComponentClasses();
        ComponentFactory::RegisterComponentClass<StaticMeshComponent>("StaticMeshComponent");

        PrefabEntityData rootData;
        rootData.name = "PrefabRoot";
        rootData.position = Vec3(1.0f, 0.0f, 0.0f);
        rootData.actorComponentData.push_back({"StaticMeshComponent", ""});

        PrefabEntityData childData;
        childData.name = "PrefabChild";
        childData.parentIndex = 0;
        childData.position = Vec3(0.0f, 2.0f, 0.0f);

        auto prefab = Prefab::CreateFromData({rootData, childData});

        SceneManager sceneManager;
        sceneManager.Initialize();

        ActorSpawnParams parentParams;
        parentParams.name = "PrefabParent";
        auto* parent = sceneManager.SpawnActor(parentParams);
        TEST_ASSERT_NOT_NULL(parent);

        SceneEntity* root = prefab->InstantiateAsChild(parent);
        TEST_ASSERT_NOT_NULL(root);
        TEST_ASSERT_EQ(parent, root->GetParent());
        TEST_ASSERT_EQ(static_cast<size_t>(1), root->GetChildren().size());
        TEST_ASSERT_EQ(static_cast<size_t>(3), sceneManager.GetEntityCount());
        TEST_ASSERT_NOT_NULL(static_cast<Actor*>(root)->GetComponent<StaticMeshComponent>());

        auto* child = root->GetChildren()[0];
        TEST_ASSERT_NOT_NULL(child);
        TEST_ASSERT_EQ(std::string("PrefabChild"), child->GetName());
        TEST_ASSERT_EQ(root, child->GetParent());
        TEST_ASSERT_EQ(Vec3(0.0f, 2.0f, 0.0f), child->GetPosition());

        sceneManager.Shutdown();
        return true;
    }
} // namespace

int main()
{
    Log::Initialize();
    RVX_CORE_INFO("Resource Instantiation Validation Tests");

    TestSuite suite;
    suite.AddTest("ModelResourceInstantiateActorUsesStaticMeshComponent",
                  Test_ModelResourceInstantiateActorUsesStaticMeshComponent);
    suite.AddTest("ActorAddOwnedComponentUsesNormalLifecycle",
                  Test_ActorAddOwnedComponentUsesNormalLifecycle);
    suite.AddTest("AddOwnedComponentRejectsLegacyComponentToAvoidContainerSplit",
                  Test_AddOwnedComponentRejectsLegacyComponentToAvoidContainerSplit);
    suite.AddTest("LegacyInstantiateDelegatesToActorPath",
                  Test_LegacyInstantiateDelegatesToActorPath);
    suite.AddTest("ModelResourceInstantiateActorBuildsSpawnedHierarchy",
                  Test_ModelResourceInstantiateActorBuildsSpawnedHierarchy);
    suite.AddTest("ComponentFactoryDefaultCreatesStaticMeshComponent",
                  Test_ComponentFactoryDefaultCreatesStaticMeshComponent);
    suite.AddTest("ComponentFactoryRegisterDefaultsPreservesCustomCreators",
                  Test_ComponentFactoryRegisterDefaultsPreservesCustomCreators);
    suite.AddTest("PrefabSerializesActorComponentClassNames",
                  Test_PrefabSerializesActorComponentClassNames);
    suite.AddTest("PrefabInstantiatesRegisteredActorComponentClasses",
                  Test_PrefabInstantiatesRegisteredActorComponentClasses);
    suite.AddTest("PrefabInstantiateAsChildBuildsSpawnedHierarchy",
                  Test_PrefabInstantiateAsChildBuildsSpawnedHierarchy);

    auto results = suite.Run();
    suite.PrintResults(results);

    Log::Shutdown();

    for (const auto& result : results)
    {
        if (!result.passed)
            return 1;
    }
    return 0;
}
