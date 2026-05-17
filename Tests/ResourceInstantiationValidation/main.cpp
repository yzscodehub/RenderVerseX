#include "Core/Core.h"
#include "Resource/ResourceManager.h"
#include "Resource/Types/MaterialResource.h"
#include "Resource/Types/MeshResource.h"
#include "Resource/Types/ModelResource.h"
#include "Scene/Actor.h"
#include "Scene/ActorFactory.h"
#include "Scene/Component.h"
#include "Scene/ComponentFactory.h"
#include "Scene/Components/MeshRendererComponent.h"
#include "Scene/Components/StaticMeshComponent.h"
#include "Scene/Mesh.h"
#include "Scene/Node.h"
#include "Scene/Prefab.h"
#include "Scene/SceneManager.h"
#include "TestFramework/TestRunner.h"
#include "World/World.h"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

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

    class PrefabCustomSceneActor : public SceneEntity
    {
    public:
        explicit PrefabCustomSceneActor(const std::string& name = "PrefabCustomSceneActorDefault")
            : SceneEntity(name)
        {
        }

        const char* GetClassName() const override { return "PrefabCustomSceneActor"; }
    };

    class PrefabPayloadComponent : public ActorComponent
    {
    public:
        const char* GetClassName() const override { return "PrefabPayloadComponent"; }

        std::string SerializePrefabData() const override
        {
            return payload;
        }

        void DeserializePrefabData(const std::string& data) override
        {
            payload = data;
        }

        void OnRegister() override
        {
            payloadObservedOnRegister = payload;
        }

        void InitializeComponent() override
        {
            payloadObservedOnInitialize = payload;
        }

        std::string payload;
        std::string payloadObservedOnRegister;
        std::string payloadObservedOnInitialize;
    };

    class PrefabNameCollisionActor : public SceneEntity
    {
    public:
        PrefabNameCollisionActor()
            : SceneEntity("PrefabNameCollisionActorDefault")
        {
            auto* component = AddComponent<PrefabPayloadComponent>();
            component->SetName("PrimaryPayload");
            component->payload = "constructor";
        }

        const char* GetClassName() const override { return "PrefabNameCollisionActor"; }
    };

    class PrefabLegacyPayloadComponent : public Component
    {
    public:
        const char* GetTypeName() const override { return "PrefabLegacyPayloadComponent"; }

        std::string SerializePrefabData() const override
        {
            return payload;
        }

        void DeserializePrefabData(const std::string& data) override
        {
            payload = data;
        }

        void OnAttach() override
        {
            payloadObservedOnAttach = payload;
        }

        std::string payload;
        std::string payloadObservedOnAttach;
    };

    class WorldLoadModelLoader : public IResourceLoader
    {
    public:
        ResourceType GetResourceType() const override { return ResourceType::Model; }

        std::vector<std::string> GetSupportedExtensions() const override
        {
            return {".model"};
        }

        IResource* Load(const std::string&) override
        {
            auto* model = new ModelResource();

            auto root = std::make_shared<Node>("LoadedWorldRoot");
            root->GetLocalTransform().SetPosition(Vec3(1.0f, 2.0f, 3.0f));

            auto child = std::make_shared<Node>("LoadedWorldChild");
            child->GetLocalTransform().SetPosition(Vec3(4.0f, 5.0f, 6.0f));
            root->AddChild(child);

            model->SetRootNode(root);
            return model;
        }
    };

    class WorldLoadEmptyModelLoader : public IResourceLoader
    {
    public:
        ResourceType GetResourceType() const override { return ResourceType::Model; }

        std::vector<std::string> GetSupportedExtensions() const override
        {
            return {".model"};
        }

        IResource* Load(const std::string&) override
        {
            return new ModelResource();
        }
    };

    class ResourceManagerTestGuard
    {
    public:
        explicit ResourceManagerTestGuard(bool initialize)
        {
            auto& resourceManager = ResourceManager::Get();
            if (resourceManager.IsInitialized())
            {
                resourceManager.Shutdown();
            }

            if (initialize)
            {
                ResourceManagerConfig config;
                config.asyncThreadCount = 0;
                resourceManager.Initialize(config);
            }
        }

        ~ResourceManagerTestGuard()
        {
            auto& resourceManager = ResourceManager::Get();
            if (resourceManager.IsInitialized())
            {
                resourceManager.Shutdown();
            }
        }
    };

    class ComponentFactoryClassGuard
    {
    public:
        ComponentFactoryClassGuard()
        {
            ComponentFactory::ClearComponentClasses();
        }

        ~ComponentFactoryClassGuard()
        {
            ComponentFactory::ClearComponentClasses();
        }
    };

    SceneEntity* FindEntityByName(SceneManager& sceneManager, const std::string& name)
    {
        SceneEntity* found = nullptr;
        sceneManager.ForEachEntity([&](SceneEntity* entity) {
            if (entity && entity->GetName() == name)
            {
                found = entity;
            }
        });
        return found;
    }

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

    PrefabPayloadComponent* FindPrefabPayloadComponentByName(Actor& actor, const std::string& name)
    {
        for (const auto& component : actor.GetActorComponents())
        {
            auto* payloadComponent = dynamic_cast<PrefabPayloadComponent*>(component.get());
            if (payloadComponent && payloadComponent->GetName() == name)
            {
                return payloadComponent;
            }
        }

        return nullptr;
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

    bool Test_PrefabSerializesActorComponentPayloads()
    {
        SceneManager sceneManager;
        sceneManager.Initialize();

        ActorSpawnParams params;
        params.name = "PayloadSource";
        SceneEntity* entity = sceneManager.SpawnActor(params);
        TEST_ASSERT_NOT_NULL(entity);

        auto* component = static_cast<Actor*>(entity)->AddComponent<PrefabPayloadComponent>();
        TEST_ASSERT_NOT_NULL(component);
        component->payload = "health=75;tag=elite";

        auto prefab = Prefab::CreateFromEntity(entity);
        TEST_ASSERT_NOT_NULL(prefab);
        const PrefabEntityData* data = prefab->GetRootData();
        TEST_ASSERT_NOT_NULL(data);
        TEST_ASSERT_EQ(static_cast<size_t>(1), data->actorComponentData.size());
        TEST_ASSERT_EQ(std::string("PrefabPayloadComponent"),
                       data->actorComponentData[0].className);
        TEST_ASSERT_EQ(std::string("health=75;tag=elite"),
                       data->actorComponentData[0].serializedData);

        sceneManager.Shutdown();
        return true;
    }

    bool Test_PrefabSerializesActorComponentNames()
    {
        SceneManager sceneManager;
        sceneManager.Initialize();

        ActorSpawnParams params;
        params.name = "NamedPayloadSource";
        SceneEntity* entity = sceneManager.SpawnActor(params);
        TEST_ASSERT_NOT_NULL(entity);

        auto* component = static_cast<Actor*>(entity)->AddComponent<PrefabPayloadComponent>();
        TEST_ASSERT_NOT_NULL(component);
        component->SetName("PrimaryPayload");
        component->payload = "health=75;tag=elite";

        auto prefab = Prefab::CreateFromEntity(entity);
        TEST_ASSERT_NOT_NULL(prefab);
        const PrefabEntityData* data = prefab->GetRootData();
        TEST_ASSERT_NOT_NULL(data);
        TEST_ASSERT_EQ(static_cast<size_t>(1), data->actorComponentData.size());
        TEST_ASSERT_EQ(std::string("PrimaryPayload"), data->actorComponentData[0].name);

        sceneManager.Shutdown();
        return true;
    }

    bool Test_PrefabSerializesLegacyComponentPayloads()
    {
        SceneManager sceneManager;
        sceneManager.Initialize();

        const auto handle = sceneManager.CreateEntity("LegacyPayloadSource");
        SceneEntity* entity = sceneManager.GetEntity(handle);
        TEST_ASSERT_NOT_NULL(entity);

        auto* component = entity->AddComponent<PrefabLegacyPayloadComponent>();
        TEST_ASSERT_NOT_NULL(component);
        component->payload = "legacy=enabled";

        auto prefab = Prefab::CreateFromEntity(entity);
        TEST_ASSERT_NOT_NULL(prefab);

        const PrefabEntityData* rootData = prefab->GetRootData();
        TEST_ASSERT_NOT_NULL(rootData);
        auto it = rootData->componentData.find("PrefabLegacyPayloadComponent");
        TEST_ASSERT_TRUE(it != rootData->componentData.end());
        TEST_ASSERT_EQ(std::string("legacy=enabled"), it->second);

        sceneManager.Shutdown();
        return true;
    }

    bool Test_PrefabSerializesActorClassNames()
    {
        SceneManager sceneManager;
        sceneManager.Initialize();

        ActorSpawnParams rootParams;
        rootParams.name = "CustomPrefabRoot";
        auto* root = sceneManager.SpawnActor<PrefabCustomSceneActor>(rootParams);
        TEST_ASSERT_NOT_NULL(root);

        ActorSpawnParams childParams;
        childParams.name = "CustomPrefabChild";
        childParams.parent = root;
        auto* child = sceneManager.SpawnActor<PrefabCustomSceneActor>(childParams);
        TEST_ASSERT_NOT_NULL(child);

        auto prefab = Prefab::CreateFromEntity(root);
        TEST_ASSERT_NOT_NULL(prefab.get());
        TEST_ASSERT_EQ(static_cast<size_t>(2), prefab->GetEntityCount());

        const auto* rootData = prefab->GetEntityData(0);
        const auto* childData = prefab->GetEntityData(1);
        TEST_ASSERT_NOT_NULL(rootData);
        TEST_ASSERT_NOT_NULL(childData);
        TEST_ASSERT_EQ(std::string("PrefabCustomSceneActor"), rootData->actorClassName);
        TEST_ASSERT_EQ(std::string("PrefabCustomSceneActor"), childData->actorClassName);

        sceneManager.Shutdown();
        return true;
    }

    bool Test_PrefabInstantiatesRegisteredActorClasses()
    {
        ActorFactory::ClearAll();
        ActorFactory::Register<PrefabCustomSceneActor>("PrefabCustomSceneActor");

        PrefabEntityData rootData;
        rootData.name = "RegisteredPrefabRoot";
        rootData.actorClassName = "PrefabCustomSceneActor";
        rootData.position = Vec3(1.0f, 2.0f, 3.0f);

        PrefabEntityData childData;
        childData.name = "RegisteredPrefabChild";
        childData.actorClassName = "PrefabCustomSceneActor";
        childData.parentIndex = 0;
        childData.position = Vec3(4.0f, 5.0f, 6.0f);

        auto prefab = Prefab::CreateFromData({rootData, childData});

        SceneManager sceneManager;
        sceneManager.Initialize();

        SceneEntity* root = prefab->Instantiate(sceneManager);
        TEST_ASSERT_NOT_NULL(root);
        TEST_ASSERT_NOT_NULL(dynamic_cast<PrefabCustomSceneActor*>(root));
        TEST_ASSERT_EQ(std::string("RegisteredPrefabRoot"), root->GetName());
        TEST_ASSERT_EQ(Vec3(1.0f, 2.0f, 3.0f), root->GetPosition());
        TEST_ASSERT_EQ(static_cast<size_t>(1), root->GetChildren().size());
        TEST_ASSERT_EQ(static_cast<size_t>(2), sceneManager.GetEntityCount());

        SceneEntity* child = root->GetChildren()[0];
        TEST_ASSERT_NOT_NULL(child);
        TEST_ASSERT_NOT_NULL(dynamic_cast<PrefabCustomSceneActor*>(child));
        TEST_ASSERT_EQ(root, child->GetParent());
        TEST_ASSERT_EQ(std::string("RegisteredPrefabChild"), child->GetName());
        TEST_ASSERT_EQ(Vec3(4.0f, 5.0f, 6.0f), child->GetPosition());

        sceneManager.Shutdown();
        ActorFactory::ClearAll();
        return true;
    }

    bool Test_PrefabInstantiateFailsForMissingActorClassAndCleansUp()
    {
        ActorFactory::ClearAll();

        PrefabEntityData rootData;
        rootData.name = "PlainRootBeforeMissingChild";

        PrefabEntityData childData;
        childData.name = "MissingClassChild";
        childData.actorClassName = "MissingPrefabActorClass";
        childData.parentIndex = 0;

        auto prefab = Prefab::CreateFromData({rootData, childData});

        SceneManager sceneManager;
        sceneManager.Initialize();

        SceneEntity* root = prefab->Instantiate(sceneManager);
        TEST_ASSERT_EQ(nullptr, root);
        TEST_ASSERT_EQ(static_cast<size_t>(0), sceneManager.GetEntityCount());

        sceneManager.Shutdown();
        return true;
    }

    bool Test_PrefabInstantiateFailsForMissingActorComponentClassAndCleansUp()
    {
        ComponentFactoryClassGuard componentFactoryGuard;

        PrefabEntityData data;
        data.name = "MissingComponentActor";
        data.actorComponentData.push_back({"MissingActorComponentClass", ""});

        auto prefab = Prefab::CreateFromData({data});

        SceneManager sceneManager;
        sceneManager.Initialize();

        SceneEntity* instance = prefab->Instantiate(sceneManager);
        TEST_ASSERT_EQ(nullptr, instance);
        TEST_ASSERT_EQ(static_cast<size_t>(0), sceneManager.GetEntityCount());

        sceneManager.Shutdown();
        return true;
    }

    bool Test_PrefabInstantiateFailsForMissingLegacyComponentClassAndCleansUp()
    {
        ComponentFactoryClassGuard componentFactoryGuard;

        PrefabEntityData data;
        data.name = "MissingLegacyComponentActor";
        data.componentData["MissingLegacyComponentClass"] = "";

        auto prefab = Prefab::CreateFromData({data});

        SceneManager sceneManager;
        sceneManager.Initialize();

        SceneEntity* instance = prefab->Instantiate(sceneManager);
        TEST_ASSERT_EQ(nullptr, instance);
        TEST_ASSERT_EQ(static_cast<size_t>(0), sceneManager.GetEntityCount());

        sceneManager.Shutdown();
        return true;
    }

    bool Test_PrefabInstantiateFailsForActorOnlyComponentDataAndCleansUp()
    {
        ComponentFactoryClassGuard componentFactoryGuard;
        ComponentFactory::RegisterComponentClass<PrefabPayloadComponent>(
            "PrefabPayloadComponent");

        PrefabEntityData data;
        data.name = "ActorOnlyComponentData";
        data.componentData["PrefabPayloadComponent"] = "";

        auto prefab = Prefab::CreateFromData({data});

        SceneManager sceneManager;
        sceneManager.Initialize();

        SceneEntity* instance = prefab->Instantiate(sceneManager);
        TEST_ASSERT_EQ(nullptr, instance);
        TEST_ASSERT_EQ(static_cast<size_t>(0), sceneManager.GetEntityCount());

        sceneManager.Shutdown();
        return true;
    }

    bool Test_PrefabInstantiateFailsForRejectedActorComponentAndCleansUp()
    {
        ComponentFactoryClassGuard componentFactoryGuard;
        ComponentFactory::RegisterComponentClass<PrefabLegacyPayloadComponent>(
            "PrefabLegacyPayloadComponent");

        PrefabEntityData data;
        data.name = "RejectedComponentActor";
        data.actorComponentData.push_back({"PrefabLegacyPayloadComponent", ""});

        auto prefab = Prefab::CreateFromData({data});

        SceneManager sceneManager;
        sceneManager.Initialize();

        SceneEntity* instance = prefab->Instantiate(sceneManager);
        TEST_ASSERT_EQ(nullptr, instance);
        TEST_ASSERT_EQ(static_cast<size_t>(0), sceneManager.GetEntityCount());

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

    bool Test_PrefabInstantiatesActorComponentPayloads()
    {
        ComponentFactory::ClearComponentClasses();
        ComponentFactory::RegisterComponentClass<PrefabPayloadComponent>("PrefabPayloadComponent");

        PrefabEntityData data;
        data.name = "PayloadPrefab";
        data.actorComponentData.push_back({"PrefabPayloadComponent", "ammo=12;mode=burst"});

        auto prefab = Prefab::CreateFromData({data});

        SceneManager sceneManager;
        sceneManager.Initialize();

        SceneEntity* instance = prefab->Instantiate(sceneManager);
        TEST_ASSERT_NOT_NULL(instance);

        auto* component = static_cast<Actor*>(instance)->GetComponent<PrefabPayloadComponent>();
        TEST_ASSERT_NOT_NULL(component);
        TEST_ASSERT_EQ(std::string("ammo=12;mode=burst"), component->payload);
        TEST_ASSERT_EQ(std::string("ammo=12;mode=burst"),
                       component->payloadObservedOnRegister);
        TEST_ASSERT_EQ(std::string("ammo=12;mode=burst"),
                       component->payloadObservedOnInitialize);
        TEST_ASSERT_TRUE(component->IsRegistered());
        TEST_ASSERT_TRUE(component->IsInitialized());

        sceneManager.Shutdown();
        ComponentFactory::ClearComponentClasses();
        return true;
    }

    bool Test_PrefabInstantiatesActorComponentNames()
    {
        ComponentFactoryClassGuard componentFactoryGuard;
        ComponentFactory::RegisterComponentClass<PrefabPayloadComponent>(
            "PrefabPayloadComponent");

        PrefabEntityData data;
        data.name = "NamedComponentPrefabRoot";
        data.actorComponentData.push_back({"PrefabPayloadComponent", "ammo=12", "PrimaryPayload"});

        auto prefab = Prefab::CreateFromData({data});

        SceneManager sceneManager;
        sceneManager.Initialize();

        SceneEntity* instance = prefab->Instantiate(sceneManager);
        TEST_ASSERT_NOT_NULL(instance);

        auto* component = static_cast<Actor*>(instance)->GetComponent<PrefabPayloadComponent>();
        TEST_ASSERT_NOT_NULL(component);
        TEST_ASSERT_EQ(std::string("PrimaryPayload"), component->GetName());
        TEST_ASSERT_EQ(std::string("ammo=12"), component->payload);

        sceneManager.Shutdown();
        return true;
    }

    bool Test_PrefabInstantiatesRegisteredLegacyComponentPayloads()
    {
        ComponentFactoryClassGuard componentFactoryGuard;
        ComponentFactory::RegisterComponentClass<PrefabLegacyPayloadComponent>(
            "PrefabLegacyPayloadComponent");

        PrefabEntityData data;
        data.name = "LegacyPayloadPrefab";
        data.componentData["PrefabLegacyPayloadComponent"] = "legacy=enabled";

        auto prefab = Prefab::CreateFromData({data});

        SceneManager sceneManager;
        sceneManager.Initialize();

        SceneEntity* instance = prefab->Instantiate(sceneManager);
        TEST_ASSERT_NOT_NULL(instance);

        auto* component = instance->GetComponent<PrefabLegacyPayloadComponent>();
        TEST_ASSERT_NOT_NULL(component);
        TEST_ASSERT_EQ(std::string("legacy=enabled"), component->payload);
        TEST_ASSERT_EQ(std::string("legacy=enabled"), component->payloadObservedOnAttach);

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

    bool Test_PrefabInstanceRevertAllRestoresRootEntityState()
    {
        PrefabEntityData data;
        data.name = "RevertAllRoot";
        data.position = Vec3(1.0f, 2.0f, 3.0f);
        data.rotation = Quat(0.7071068f, 0.0f, 0.7071068f, 0.0f);
        data.scale = Vec3(2.0f, 3.0f, 4.0f);
        data.layerMask = 0x5u;
        data.isActive = false;

        auto prefab = Prefab::CreateFromData({data});

        SceneManager sceneManager;
        sceneManager.Initialize();

        SceneEntity* instance = prefab->Instantiate(sceneManager);
        TEST_ASSERT_NOT_NULL(instance);
        auto* prefabInstance = instance->GetComponent<PrefabInstance>();
        TEST_ASSERT_NOT_NULL(prefabInstance);

        instance->SetPosition(Vec3(9.0f, 9.0f, 9.0f));
        instance->SetRotation(Quat(1.0f, 0.0f, 0.0f, 0.0f));
        instance->SetScale(Vec3(8.0f, 8.0f, 8.0f));
        instance->SetLayerMask(0xFu);
        instance->SetActive(true);
        prefabInstance->AddOverride({"SceneEntity", "position", Vec3(9.0f, 9.0f, 9.0f)});
        prefabInstance->AddOverride({"SceneEntity", "scale", Vec3(8.0f, 8.0f, 8.0f)});

        prefabInstance->RevertAll();

        TEST_ASSERT_EQ(Vec3(1.0f, 2.0f, 3.0f), instance->GetPosition());
        TEST_ASSERT_EQ(Quat(0.7071068f, 0.0f, 0.7071068f, 0.0f), instance->GetRotation());
        TEST_ASSERT_EQ(Vec3(2.0f, 3.0f, 4.0f), instance->GetScale());
        TEST_ASSERT_EQ(static_cast<uint32_t>(0x5u), instance->GetLayerMask());
        TEST_ASSERT_FALSE(instance->IsActive());
        TEST_ASSERT_TRUE(prefabInstance->GetOverrides().empty());

        sceneManager.Shutdown();
        return true;
    }

    bool Test_PrefabInstanceRevertAllRestoresActorComponentPayload()
    {
        ComponentFactoryClassGuard componentFactoryGuard;
        ComponentFactory::RegisterComponentClass<PrefabPayloadComponent>(
            "PrefabPayloadComponent");

        PrefabEntityData data;
        data.name = "RevertActorPayloadRoot";
        data.actorComponentData.push_back({"PrefabPayloadComponent", "ammo=12;mode=burst"});

        auto prefab = Prefab::CreateFromData({data});

        SceneManager sceneManager;
        sceneManager.Initialize();

        SceneEntity* instance = prefab->Instantiate(sceneManager);
        TEST_ASSERT_NOT_NULL(instance);
        auto* prefabInstance = instance->GetComponent<PrefabInstance>();
        TEST_ASSERT_NOT_NULL(prefabInstance);

        auto* component = static_cast<Actor*>(instance)->GetComponent<PrefabPayloadComponent>();
        TEST_ASSERT_NOT_NULL(component);
        component->payload = "ammo=1;mode=single";
        prefabInstance->AddOverride({"PrefabPayloadComponent", "serializedData", component->payload});

        prefabInstance->RevertAll();

        TEST_ASSERT_EQ(std::string("ammo=12;mode=burst"), component->payload);
        TEST_ASSERT_TRUE(prefabInstance->GetOverrides().empty());

        sceneManager.Shutdown();
        return true;
    }

    bool Test_PrefabInstanceRevertAllRestoresNamedDuplicateActorComponentPayloads()
    {
        ComponentFactoryClassGuard componentFactoryGuard;
        ComponentFactory::RegisterComponentClass<PrefabPayloadComponent>(
            "PrefabPayloadComponent");

        PrefabEntityData data;
        data.name = "NamedDuplicatePayloadRoot";
        data.actorComponentData.push_back({"PrefabPayloadComponent", "slot=primary", "PrimaryPayload"});
        data.actorComponentData.push_back({"PrefabPayloadComponent", "slot=secondary", "SecondaryPayload"});

        auto prefab = Prefab::CreateFromData({data});

        SceneManager sceneManager;
        sceneManager.Initialize();

        SceneEntity* instance = prefab->Instantiate(sceneManager);
        TEST_ASSERT_NOT_NULL(instance);
        auto* prefabInstance = instance->GetComponent<PrefabInstance>();
        TEST_ASSERT_NOT_NULL(prefabInstance);

        auto* primary = FindPrefabPayloadComponentByName(*static_cast<Actor*>(instance),
                                                         "PrimaryPayload");
        auto* secondary = FindPrefabPayloadComponentByName(*static_cast<Actor*>(instance),
                                                           "SecondaryPayload");
        TEST_ASSERT_NOT_NULL(primary);
        TEST_ASSERT_NOT_NULL(secondary);

        primary->SetName("TemporaryPayloadName");
        secondary->SetName("PrimaryPayload");
        primary->SetName("SecondaryPayload");
        primary->payload = "dirty-secondary";
        secondary->payload = "dirty-primary";
        // Named single-property targets are intentionally deferred; this phase
        // only guarantees name-aware full payload restoration through RevertAll.
        prefabInstance->AddOverride({"PrefabPayloadComponent", "serializedData", std::string("dirty")});

        prefabInstance->RevertAll();

        TEST_ASSERT_EQ(std::string("slot=secondary"), primary->payload);
        TEST_ASSERT_EQ(std::string("slot=primary"), secondary->payload);
        TEST_ASSERT_TRUE(prefabInstance->GetOverrides().empty());

        sceneManager.Shutdown();
        return true;
    }

    bool Test_PrefabInstanceRevertAllUsesRuntimeNameBindingAfterUniquify()
    {
        ActorFactory::ClearAll();
        ActorFactory::Register<PrefabNameCollisionActor>("PrefabNameCollisionActor");

        ComponentFactoryClassGuard componentFactoryGuard;
        ComponentFactory::RegisterComponentClass<PrefabPayloadComponent>(
            "PrefabPayloadComponent");

        PrefabEntityData data;
        data.name = "RuntimeBindingPayloadRoot";
        data.actorClassName = "PrefabNameCollisionActor";
        data.actorComponentData.push_back({"PrefabPayloadComponent", "slot=prefab", "PrimaryPayload"});

        auto prefab = Prefab::CreateFromData({data});

        SceneManager sceneManager;
        sceneManager.Initialize();

        SceneEntity* instance = prefab->Instantiate(sceneManager);
        TEST_ASSERT_NOT_NULL(instance);
        auto* prefabInstance = instance->GetComponent<PrefabInstance>();
        TEST_ASSERT_NOT_NULL(prefabInstance);

        auto* actor = static_cast<Actor*>(instance);
        auto* constructorComponent = FindPrefabPayloadComponentByName(*actor, "PrimaryPayload");
        auto* prefabComponent = FindPrefabPayloadComponentByName(*actor, "PrimaryPayload_1");
        TEST_ASSERT_NOT_NULL(constructorComponent);
        TEST_ASSERT_NOT_NULL(prefabComponent);
        TEST_ASSERT_EQ(std::string("constructor"), constructorComponent->payload);
        TEST_ASSERT_EQ(std::string("slot=prefab"), prefabComponent->payload);

        constructorComponent->payload = "constructor-dirty";
        prefabComponent->payload = "prefab-dirty";
        prefabInstance->AddOverride({"PrefabPayloadComponent", "serializedData", std::string("prefab-dirty")});

        prefabInstance->RevertAll();

        TEST_ASSERT_EQ(std::string("constructor-dirty"), constructorComponent->payload);
        TEST_ASSERT_EQ(std::string("slot=prefab"), prefabComponent->payload);
        TEST_ASSERT_TRUE(prefabInstance->GetOverrides().empty());

        sceneManager.Shutdown();
        ActorFactory::ClearAll();
        return true;
    }

    bool Test_PrefabInstanceRevertAllRestoresChildEntityStateAndPayload()
    {
        ComponentFactoryClassGuard componentFactoryGuard;
        ComponentFactory::RegisterComponentClass<PrefabPayloadComponent>(
            "PrefabPayloadComponent");

        PrefabEntityData rootData;
        rootData.name = "HierarchyRevertRoot";

        PrefabEntityData childData;
        childData.name = "Child";
        childData.parentIndex = 0;
        childData.position = Vec3(1.0f, 2.0f, 3.0f);
        childData.actorComponentData.push_back(
            {"PrefabPayloadComponent", "slot=child", "ChildPayload"});

        auto prefab = Prefab::CreateFromData({rootData, childData});

        SceneManager sceneManager;
        sceneManager.Initialize();

        SceneEntity* root = prefab->Instantiate(sceneManager);
        TEST_ASSERT_NOT_NULL(root);
        auto* prefabInstance = root->GetComponent<PrefabInstance>();
        TEST_ASSERT_NOT_NULL(prefabInstance);
        TEST_ASSERT_EQ(static_cast<size_t>(1), root->GetChildren().size());

        SceneEntity* child = root->GetChildren()[0];
        TEST_ASSERT_NOT_NULL(child);
        auto* childPayload = FindPrefabPayloadComponentByName(
            *static_cast<Actor*>(child), "ChildPayload");
        TEST_ASSERT_NOT_NULL(childPayload);

        child->SetPosition(Vec3(9.0f, 9.0f, 9.0f));
        childPayload->payload = "dirty-child";
        prefabInstance->AddOverride(
            {"SceneEntity", "position", child->GetPosition(), "", "Child"});
        prefabInstance->AddOverride(
            {"PrefabPayloadComponent", "serializedData", childPayload->payload, "ChildPayload", "Child"});

        prefabInstance->RevertAll();

        TEST_ASSERT_EQ(Vec3(1.0f, 2.0f, 3.0f), child->GetPosition());
        TEST_ASSERT_EQ(std::string("slot=child"), childPayload->payload);
        TEST_ASSERT_TRUE(prefabInstance->GetOverrides().empty());

        sceneManager.Shutdown();
        return true;
    }

    bool Test_PrefabInstanceRevertAllRestoresLegacyComponentPayload()
    {
        ComponentFactoryClassGuard componentFactoryGuard;
        ComponentFactory::RegisterComponentClass<PrefabLegacyPayloadComponent>(
            "PrefabLegacyPayloadComponent");

        PrefabEntityData data;
        data.name = "RevertLegacyPayloadRoot";
        data.componentData["PrefabLegacyPayloadComponent"] = "legacy=enabled";

        auto prefab = Prefab::CreateFromData({data});

        SceneManager sceneManager;
        sceneManager.Initialize();

        SceneEntity* instance = prefab->Instantiate(sceneManager);
        TEST_ASSERT_NOT_NULL(instance);
        auto* prefabInstance = instance->GetComponent<PrefabInstance>();
        TEST_ASSERT_NOT_NULL(prefabInstance);

        auto* component = instance->GetComponent<PrefabLegacyPayloadComponent>();
        TEST_ASSERT_NOT_NULL(component);
        component->payload = "legacy=disabled";
        prefabInstance->AddOverride({"PrefabLegacyPayloadComponent", "serializedData", component->payload});

        prefabInstance->RevertAll();

        TEST_ASSERT_EQ(std::string("legacy=enabled"), component->payload);
        TEST_ASSERT_TRUE(prefabInstance->GetOverrides().empty());

        sceneManager.Shutdown();
        return true;
    }

    bool Test_PrefabInstanceRevertPropertyRestoresOnlyRequestedEntityProperty()
    {
        PrefabEntityData data;
        data.name = "RevertPropertyRoot";
        data.position = Vec3(1.0f, 2.0f, 3.0f);
        data.scale = Vec3(2.0f, 2.0f, 2.0f);

        auto prefab = Prefab::CreateFromData({data});

        SceneManager sceneManager;
        sceneManager.Initialize();

        SceneEntity* instance = prefab->Instantiate(sceneManager);
        TEST_ASSERT_NOT_NULL(instance);
        auto* prefabInstance = instance->GetComponent<PrefabInstance>();
        TEST_ASSERT_NOT_NULL(prefabInstance);

        instance->SetPosition(Vec3(9.0f, 9.0f, 9.0f));
        instance->SetScale(Vec3(8.0f, 8.0f, 8.0f));
        prefabInstance->AddOverride({"SceneEntity", "position", Vec3(9.0f, 9.0f, 9.0f)});
        prefabInstance->AddOverride({"SceneEntity", "scale", Vec3(8.0f, 8.0f, 8.0f)});

        prefabInstance->RevertProperty("SceneEntity", "position");

        TEST_ASSERT_EQ(Vec3(1.0f, 2.0f, 3.0f), instance->GetPosition());
        TEST_ASSERT_EQ(Vec3(8.0f, 8.0f, 8.0f), instance->GetScale());
        TEST_ASSERT_FALSE(prefabInstance->IsOverridden("SceneEntity", "position"));
        TEST_ASSERT_TRUE(prefabInstance->IsOverridden("SceneEntity", "scale"));
        TEST_ASSERT_EQ(static_cast<size_t>(1), prefabInstance->GetOverrides().size());

        sceneManager.Shutdown();
        return true;
    }

    bool Test_PrefabInstanceRevertPropertyRestoresActorComponentPayload()
    {
        ComponentFactoryClassGuard componentFactoryGuard;
        ComponentFactory::RegisterComponentClass<PrefabPayloadComponent>(
            "PrefabPayloadComponent");
        ComponentFactory::RegisterComponentClass<PrefabLegacyPayloadComponent>(
            "PrefabLegacyPayloadComponent");

        PrefabEntityData data;
        data.name = "RevertPropertyActorPayloadRoot";
        data.actorComponentData.push_back({"PrefabPayloadComponent", "ammo=12;mode=burst"});
        data.componentData["PrefabLegacyPayloadComponent"] = "legacy=enabled";

        auto prefab = Prefab::CreateFromData({data});

        SceneManager sceneManager;
        sceneManager.Initialize();

        SceneEntity* instance = prefab->Instantiate(sceneManager);
        TEST_ASSERT_NOT_NULL(instance);
        auto* prefabInstance = instance->GetComponent<PrefabInstance>();
        TEST_ASSERT_NOT_NULL(prefabInstance);

        auto* actorComponent = static_cast<Actor*>(instance)->GetComponent<PrefabPayloadComponent>();
        TEST_ASSERT_NOT_NULL(actorComponent);
        auto* legacyComponent = instance->GetComponent<PrefabLegacyPayloadComponent>();
        TEST_ASSERT_NOT_NULL(legacyComponent);

        actorComponent->payload = "ammo=1;mode=single";
        legacyComponent->payload = "legacy=disabled";
        prefabInstance->AddOverride({"PrefabPayloadComponent", "serializedData", actorComponent->payload});
        prefabInstance->AddOverride({"PrefabLegacyPayloadComponent", "serializedData", legacyComponent->payload});

        prefabInstance->RevertProperty("PrefabPayloadComponent", "serializedData");

        TEST_ASSERT_EQ(std::string("ammo=12;mode=burst"), actorComponent->payload);
        TEST_ASSERT_EQ(std::string("legacy=disabled"), legacyComponent->payload);
        TEST_ASSERT_FALSE(prefabInstance->IsOverridden("PrefabPayloadComponent", "serializedData"));
        TEST_ASSERT_TRUE(prefabInstance->IsOverridden("PrefabLegacyPayloadComponent", "serializedData"));
        TEST_ASSERT_EQ(static_cast<size_t>(1), prefabInstance->GetOverrides().size());

        sceneManager.Shutdown();
        return true;
    }

    bool Test_PrefabInstanceRootEntityPathNormalizesForOverrideIdentity()
    {
        PrefabInstance prefabInstance;
        prefabInstance.AddOverride({"SceneEntity", "position", Vec3(1.0f), "", "."});

        TEST_ASSERT_TRUE(prefabInstance.IsOverridden("SceneEntity", "position"));
        TEST_ASSERT_TRUE(prefabInstance.IsOverridden("SceneEntity", "position", "", "."));

        prefabInstance.RemoveOverride("SceneEntity", "position");

        TEST_ASSERT_FALSE(prefabInstance.IsOverridden("SceneEntity", "position", "", "."));
        TEST_ASSERT_TRUE(prefabInstance.GetOverrides().empty());
        return true;
    }

    bool Test_PrefabInstanceRevertPropertyRestoresChildEntityProperty()
    {
        PrefabEntityData rootData;
        rootData.name = "ChildPropertyRoot";
        rootData.position = Vec3(1.0f, 0.0f, 0.0f);

        PrefabEntityData childData;
        childData.name = "Child";
        childData.parentIndex = 0;
        childData.position = Vec3(0.0f, 2.0f, 0.0f);

        auto prefab = Prefab::CreateFromData({rootData, childData});

        SceneManager sceneManager;
        sceneManager.Initialize();

        SceneEntity* root = prefab->Instantiate(sceneManager);
        TEST_ASSERT_NOT_NULL(root);
        auto* prefabInstance = root->GetComponent<PrefabInstance>();
        TEST_ASSERT_NOT_NULL(prefabInstance);
        TEST_ASSERT_EQ(static_cast<size_t>(1), root->GetChildren().size());
        SceneEntity* child = root->GetChildren()[0];

        root->SetPosition(Vec3(7.0f, 0.0f, 0.0f));
        child->SetPosition(Vec3(0.0f, 9.0f, 0.0f));
        prefabInstance->AddOverride({"SceneEntity", "position", root->GetPosition()});
        prefabInstance->AddOverride(
            {"SceneEntity", "position", child->GetPosition(), "", "Child"});

        prefabInstance->RevertProperty("SceneEntity", "position", "", "Child");

        TEST_ASSERT_EQ(Vec3(7.0f, 0.0f, 0.0f), root->GetPosition());
        TEST_ASSERT_EQ(Vec3(0.0f, 2.0f, 0.0f), child->GetPosition());
        TEST_ASSERT_TRUE(prefabInstance->IsOverridden("SceneEntity", "position"));
        TEST_ASSERT_FALSE(prefabInstance->IsOverridden("SceneEntity", "position", "", "Child"));

        sceneManager.Shutdown();
        return true;
    }

    bool Test_PrefabInstanceRevertPropertyUsesSiblingOrdinalEntityPath()
    {
        PrefabEntityData rootData;
        rootData.name = "DuplicateChildRoot";

        PrefabEntityData firstChildData;
        firstChildData.name = "Child";
        firstChildData.parentIndex = 0;
        firstChildData.position = Vec3(1.0f, 0.0f, 0.0f);

        PrefabEntityData secondChildData;
        secondChildData.name = "Child";
        secondChildData.parentIndex = 0;
        secondChildData.position = Vec3(2.0f, 0.0f, 0.0f);

        auto prefab = Prefab::CreateFromData({rootData, firstChildData, secondChildData});

        SceneManager sceneManager;
        sceneManager.Initialize();

        SceneEntity* root = prefab->Instantiate(sceneManager);
        TEST_ASSERT_NOT_NULL(root);
        auto* prefabInstance = root->GetComponent<PrefabInstance>();
        TEST_ASSERT_NOT_NULL(prefabInstance);
        TEST_ASSERT_EQ(static_cast<size_t>(2), root->GetChildren().size());

        SceneEntity* firstChild = root->GetChildren()[0];
        SceneEntity* secondChild = root->GetChildren()[1];
        firstChild->SetPosition(Vec3(9.0f, 0.0f, 0.0f));
        secondChild->SetPosition(Vec3(8.0f, 0.0f, 0.0f));
        prefabInstance->AddOverride(
            {"SceneEntity", "position", secondChild->GetPosition(), "", "Child[1]"});

        prefabInstance->RevertProperty("SceneEntity", "position", "", "Child[1]");

        TEST_ASSERT_EQ(Vec3(9.0f, 0.0f, 0.0f), firstChild->GetPosition());
        TEST_ASSERT_EQ(Vec3(2.0f, 0.0f, 0.0f), secondChild->GetPosition());
        TEST_ASSERT_FALSE(prefabInstance->IsOverridden("SceneEntity", "position", "", "Child[1]"));

        sceneManager.Shutdown();
        return true;
    }

    bool Test_PrefabInstanceRevertPropertyKeepsAmbiguousDuplicateSiblingPath()
    {
        PrefabEntityData rootData;
        rootData.name = "AmbiguousDuplicateRoot";

        PrefabEntityData firstChildData;
        firstChildData.name = "Child";
        firstChildData.parentIndex = 0;
        firstChildData.position = Vec3(1.0f, 0.0f, 0.0f);

        PrefabEntityData secondChildData;
        secondChildData.name = "Child";
        secondChildData.parentIndex = 0;
        secondChildData.position = Vec3(2.0f, 0.0f, 0.0f);

        auto prefab = Prefab::CreateFromData({rootData, firstChildData, secondChildData});

        SceneManager sceneManager;
        sceneManager.Initialize();

        SceneEntity* root = prefab->Instantiate(sceneManager);
        TEST_ASSERT_NOT_NULL(root);
        auto* prefabInstance = root->GetComponent<PrefabInstance>();
        TEST_ASSERT_NOT_NULL(prefabInstance);
        TEST_ASSERT_EQ(static_cast<size_t>(2), root->GetChildren().size());

        SceneEntity* firstChild = root->GetChildren()[0];
        SceneEntity* secondChild = root->GetChildren()[1];
        firstChild->SetPosition(Vec3(9.0f, 0.0f, 0.0f));
        secondChild->SetPosition(Vec3(8.0f, 0.0f, 0.0f));
        prefabInstance->AddOverride(
            {"SceneEntity", "position", secondChild->GetPosition(), "", "Child"});

        prefabInstance->RevertProperty("SceneEntity", "position", "", "Child");

        TEST_ASSERT_EQ(Vec3(9.0f, 0.0f, 0.0f), firstChild->GetPosition());
        TEST_ASSERT_EQ(Vec3(8.0f, 0.0f, 0.0f), secondChild->GetPosition());
        TEST_ASSERT_TRUE(prefabInstance->IsOverridden("SceneEntity", "position", "", "Child"));

        sceneManager.Shutdown();
        return true;
    }

    bool Test_PrefabInstanceRevertPropertyRestoresNamedDuplicateActorComponentPayload()
    {
        ComponentFactoryClassGuard componentFactoryGuard;
        ComponentFactory::RegisterComponentClass<PrefabPayloadComponent>(
            "PrefabPayloadComponent");

        PrefabEntityData data;
        data.name = "RevertNamedDuplicateActorPayloadRoot";
        data.actorComponentData.push_back({"PrefabPayloadComponent", "slot=primary", "PrimaryPayload"});
        data.actorComponentData.push_back({"PrefabPayloadComponent", "slot=secondary", "SecondaryPayload"});

        auto prefab = Prefab::CreateFromData({data});

        SceneManager sceneManager;
        sceneManager.Initialize();

        SceneEntity* instance = prefab->Instantiate(sceneManager);
        TEST_ASSERT_NOT_NULL(instance);
        auto* prefabInstance = instance->GetComponent<PrefabInstance>();
        TEST_ASSERT_NOT_NULL(prefabInstance);

        auto* primary = FindPrefabPayloadComponentByName(*static_cast<Actor*>(instance),
                                                         "PrimaryPayload");
        auto* secondary = FindPrefabPayloadComponentByName(*static_cast<Actor*>(instance),
                                                           "SecondaryPayload");
        TEST_ASSERT_NOT_NULL(primary);
        TEST_ASSERT_NOT_NULL(secondary);

        primary->payload = "dirty-primary";
        secondary->payload = "dirty-secondary";
        prefabInstance->AddOverride(
            {"PrefabPayloadComponent", "serializedData", primary->payload, "PrimaryPayload"});
        prefabInstance->AddOverride(
            {"PrefabPayloadComponent", "serializedData", secondary->payload, "SecondaryPayload"});

        prefabInstance->RevertProperty("PrefabPayloadComponent", "serializedData", "SecondaryPayload");

        TEST_ASSERT_EQ(std::string("dirty-primary"), primary->payload);
        TEST_ASSERT_EQ(std::string("slot=secondary"), secondary->payload);
        TEST_ASSERT_TRUE(
            prefabInstance->IsOverridden("PrefabPayloadComponent", "serializedData", "PrimaryPayload"));
        TEST_ASSERT_FALSE(
            prefabInstance->IsOverridden("PrefabPayloadComponent", "serializedData", "SecondaryPayload"));
        TEST_ASSERT_EQ(static_cast<size_t>(1), prefabInstance->GetOverrides().size());

        sceneManager.Shutdown();
        return true;
    }

    bool Test_PrefabInstanceRemoveOverrideTargetsComponentName()
    {
        PrefabInstance prefabInstance;
        prefabInstance.AddOverride(
            {"PrefabPayloadComponent", "serializedData", std::string("primary"), "PrimaryPayload"});
        prefabInstance.AddOverride(
            {"PrefabPayloadComponent", "serializedData", std::string("secondary"), "SecondaryPayload"});

        prefabInstance.RemoveOverride("PrefabPayloadComponent", "serializedData", "SecondaryPayload");

        TEST_ASSERT_TRUE(
            prefabInstance.IsOverridden("PrefabPayloadComponent", "serializedData", "PrimaryPayload"));
        TEST_ASSERT_FALSE(
            prefabInstance.IsOverridden("PrefabPayloadComponent", "serializedData", "SecondaryPayload"));
        TEST_ASSERT_EQ(static_cast<size_t>(1), prefabInstance.GetOverrides().size());

        return true;
    }

    bool Test_PrefabInstanceRevertPropertyUsesRuntimeNameBindingAfterUniquify()
    {
        ActorFactory::ClearAll();
        ActorFactory::Register<PrefabNameCollisionActor>("PrefabNameCollisionActor");

        ComponentFactoryClassGuard componentFactoryGuard;
        ComponentFactory::RegisterComponentClass<PrefabPayloadComponent>(
            "PrefabPayloadComponent");

        PrefabEntityData data;
        data.name = "RevertNamedBindingPayloadRoot";
        data.actorClassName = "PrefabNameCollisionActor";
        data.actorComponentData.push_back({"PrefabPayloadComponent", "slot=prefab", "PrimaryPayload"});

        auto prefab = Prefab::CreateFromData({data});

        SceneManager sceneManager;
        sceneManager.Initialize();

        SceneEntity* instance = prefab->Instantiate(sceneManager);
        TEST_ASSERT_NOT_NULL(instance);
        auto* prefabInstance = instance->GetComponent<PrefabInstance>();
        TEST_ASSERT_NOT_NULL(prefabInstance);

        auto* actor = static_cast<Actor*>(instance);
        auto* constructorComponent = FindPrefabPayloadComponentByName(*actor, "PrimaryPayload");
        auto* prefabComponent = FindPrefabPayloadComponentByName(*actor, "PrimaryPayload_1");
        TEST_ASSERT_NOT_NULL(constructorComponent);
        TEST_ASSERT_NOT_NULL(prefabComponent);

        constructorComponent->payload = "constructor-dirty";
        prefabComponent->payload = "prefab-dirty";
        prefabInstance->AddOverride(
            {"PrefabPayloadComponent", "serializedData", prefabComponent->payload, "PrimaryPayload_1"});

        prefabInstance->RevertProperty("PrefabPayloadComponent", "serializedData", "PrimaryPayload_1");

        TEST_ASSERT_EQ(std::string("constructor-dirty"), constructorComponent->payload);
        TEST_ASSERT_EQ(std::string("slot=prefab"), prefabComponent->payload);
        TEST_ASSERT_FALSE(
            prefabInstance->IsOverridden("PrefabPayloadComponent", "serializedData", "PrimaryPayload_1"));

        sceneManager.Shutdown();
        ActorFactory::ClearAll();
        return true;
    }

    bool Test_PrefabInstanceRevertPropertyRestoresChildNamedComponentPayload()
    {
        ComponentFactoryClassGuard componentFactoryGuard;
        ComponentFactory::RegisterComponentClass<PrefabPayloadComponent>(
            "PrefabPayloadComponent");

        PrefabEntityData rootData;
        rootData.name = "ChildPayloadRoot";

        PrefabEntityData childData;
        childData.name = "Child";
        childData.parentIndex = 0;
        childData.actorComponentData.push_back(
            {"PrefabPayloadComponent", "slot=primary", "PrimaryPayload"});
        childData.actorComponentData.push_back(
            {"PrefabPayloadComponent", "slot=secondary", "SecondaryPayload"});

        auto prefab = Prefab::CreateFromData({rootData, childData});

        SceneManager sceneManager;
        sceneManager.Initialize();

        SceneEntity* root = prefab->Instantiate(sceneManager);
        TEST_ASSERT_NOT_NULL(root);
        auto* prefabInstance = root->GetComponent<PrefabInstance>();
        TEST_ASSERT_NOT_NULL(prefabInstance);
        TEST_ASSERT_EQ(static_cast<size_t>(1), root->GetChildren().size());
        SceneEntity* child = root->GetChildren()[0];

        auto* primary = FindPrefabPayloadComponentByName(
            *static_cast<Actor*>(child), "PrimaryPayload");
        auto* secondary = FindPrefabPayloadComponentByName(
            *static_cast<Actor*>(child), "SecondaryPayload");
        TEST_ASSERT_NOT_NULL(primary);
        TEST_ASSERT_NOT_NULL(secondary);

        primary->payload = "dirty-primary";
        secondary->payload = "dirty-secondary";
        prefabInstance->AddOverride(
            {"PrefabPayloadComponent", "serializedData", primary->payload, "PrimaryPayload", "Child"});
        prefabInstance->AddOverride(
            {"PrefabPayloadComponent", "serializedData", secondary->payload, "SecondaryPayload", "Child"});

        prefabInstance->RevertProperty(
            "PrefabPayloadComponent", "serializedData", "SecondaryPayload", "Child");

        TEST_ASSERT_EQ(std::string("dirty-primary"), primary->payload);
        TEST_ASSERT_EQ(std::string("slot=secondary"), secondary->payload);
        TEST_ASSERT_TRUE(prefabInstance->IsOverridden(
            "PrefabPayloadComponent", "serializedData", "PrimaryPayload", "Child"));
        TEST_ASSERT_FALSE(prefabInstance->IsOverridden(
            "PrefabPayloadComponent", "serializedData", "SecondaryPayload", "Child"));

        sceneManager.Shutdown();
        return true;
    }

    bool Test_PrefabInstanceRevertPropertyScopesComponentBindingsByEntityPath()
    {
        ActorFactory::ClearAll();
        ActorFactory::Register<PrefabNameCollisionActor>("PrefabNameCollisionActor");

        ComponentFactoryClassGuard componentFactoryGuard;
        ComponentFactory::RegisterComponentClass<PrefabPayloadComponent>(
            "PrefabPayloadComponent");

        PrefabEntityData rootData;
        rootData.name = "BindingScopeRoot";
        rootData.actorComponentData.push_back(
            {"PrefabPayloadComponent", "slot=root", "PrimaryPayload_1"});

        PrefabEntityData childData;
        childData.name = "Child";
        childData.parentIndex = 0;
        childData.actorClassName = "PrefabNameCollisionActor";
        childData.actorComponentData.push_back(
            {"PrefabPayloadComponent", "slot=child", "PrimaryPayload"});

        auto prefab = Prefab::CreateFromData({rootData, childData});

        SceneManager sceneManager;
        sceneManager.Initialize();

        SceneEntity* root = prefab->Instantiate(sceneManager);
        TEST_ASSERT_NOT_NULL(root);
        auto* prefabInstance = root->GetComponent<PrefabInstance>();
        TEST_ASSERT_NOT_NULL(prefabInstance);
        TEST_ASSERT_EQ(static_cast<size_t>(1), root->GetChildren().size());
        SceneEntity* child = root->GetChildren()[0];

        auto* rootPayload = FindPrefabPayloadComponentByName(
            *static_cast<Actor*>(root), "PrimaryPayload_1");
        auto* childPayload = FindPrefabPayloadComponentByName(
            *static_cast<Actor*>(child), "PrimaryPayload_1");
        TEST_ASSERT_NOT_NULL(rootPayload);
        TEST_ASSERT_NOT_NULL(childPayload);

        rootPayload->payload = "dirty-root";
        childPayload->payload = "dirty-child";
        prefabInstance->AddOverride(
            {"PrefabPayloadComponent", "serializedData", childPayload->payload, "PrimaryPayload_1", "Child"});

        prefabInstance->RevertProperty(
            "PrefabPayloadComponent", "serializedData", "PrimaryPayload_1", "Child");

        TEST_ASSERT_EQ(std::string("dirty-root"), rootPayload->payload);
        TEST_ASSERT_EQ(std::string("slot=child"), childPayload->payload);

        sceneManager.Shutdown();
        ActorFactory::ClearAll();
        return true;
    }

    bool Test_PrefabInstanceRevertPropertyRestoresLegacyComponentPayload()
    {
        ComponentFactoryClassGuard componentFactoryGuard;
        ComponentFactory::RegisterComponentClass<PrefabPayloadComponent>(
            "PrefabPayloadComponent");
        ComponentFactory::RegisterComponentClass<PrefabLegacyPayloadComponent>(
            "PrefabLegacyPayloadComponent");

        PrefabEntityData data;
        data.name = "RevertPropertyLegacyPayloadRoot";
        data.actorComponentData.push_back({"PrefabPayloadComponent", "ammo=12"});
        data.componentData["PrefabLegacyPayloadComponent"] = "legacy=enabled";

        auto prefab = Prefab::CreateFromData({data});

        SceneManager sceneManager;
        sceneManager.Initialize();

        SceneEntity* instance = prefab->Instantiate(sceneManager);
        TEST_ASSERT_NOT_NULL(instance);
        auto* prefabInstance = instance->GetComponent<PrefabInstance>();
        TEST_ASSERT_NOT_NULL(prefabInstance);

        auto* actorComponent = static_cast<Actor*>(instance)->GetComponent<PrefabPayloadComponent>();
        TEST_ASSERT_NOT_NULL(actorComponent);
        auto* legacyComponent = instance->GetComponent<PrefabLegacyPayloadComponent>();
        TEST_ASSERT_NOT_NULL(legacyComponent);

        actorComponent->payload = "ammo=99";
        legacyComponent->payload = "legacy=disabled";
        prefabInstance->AddOverride({"PrefabPayloadComponent", "serializedData", actorComponent->payload});
        prefabInstance->AddOverride({"PrefabLegacyPayloadComponent", "serializedData", legacyComponent->payload});

        prefabInstance->RevertProperty("PrefabLegacyPayloadComponent", "serializedData");

        TEST_ASSERT_EQ(std::string("ammo=99"), actorComponent->payload);
        TEST_ASSERT_EQ(std::string("legacy=enabled"), legacyComponent->payload);
        TEST_ASSERT_TRUE(prefabInstance->IsOverridden("PrefabPayloadComponent", "serializedData"));
        TEST_ASSERT_FALSE(prefabInstance->IsOverridden("PrefabLegacyPayloadComponent", "serializedData"));
        TEST_ASSERT_EQ(static_cast<size_t>(1), prefabInstance->GetOverrides().size());

        sceneManager.Shutdown();
        return true;
    }

    bool Test_PrefabInstanceRevertPropertyKeepsPayloadOverrideWhenComponentMissing()
    {
        ComponentFactoryClassGuard componentFactoryGuard;
        ComponentFactory::RegisterComponentClass<PrefabPayloadComponent>(
            "PrefabPayloadComponent");

        PrefabEntityData data;
        data.name = "MissingPayloadComponentRoot";
        data.actorComponentData.push_back({"PrefabPayloadComponent", "ammo=12"});

        auto prefab = Prefab::CreateFromData({data});

        SceneManager sceneManager;
        sceneManager.Initialize();

        SceneEntity* instance = prefab->Instantiate(sceneManager);
        TEST_ASSERT_NOT_NULL(instance);
        auto* prefabInstance = instance->GetComponent<PrefabInstance>();
        TEST_ASSERT_NOT_NULL(prefabInstance);

        TEST_ASSERT_TRUE(static_cast<Actor*>(instance)->RemoveComponent<PrefabPayloadComponent>());
        TEST_ASSERT_TRUE(static_cast<Actor*>(instance)->GetComponent<PrefabPayloadComponent>() == nullptr);
        prefabInstance->AddOverride({"PrefabPayloadComponent", "serializedData", std::string("ammo=99")});

        prefabInstance->RevertProperty("PrefabPayloadComponent", "serializedData");

        TEST_ASSERT_TRUE(static_cast<Actor*>(instance)->GetComponent<PrefabPayloadComponent>() == nullptr);
        TEST_ASSERT_TRUE(prefabInstance->IsOverridden("PrefabPayloadComponent", "serializedData"));
        TEST_ASSERT_EQ(static_cast<size_t>(1), prefabInstance->GetOverrides().size());

        sceneManager.Shutdown();
        return true;
    }

    bool Test_PrefabInstanceRevertPropertyAcceptsEmptyRootTarget()
    {
        PrefabEntityData data;
        data.name = "EmptyRootTarget";
        data.position = Vec3(1.0f, 2.0f, 3.0f);

        auto prefab = Prefab::CreateFromData({data});

        SceneManager sceneManager;
        sceneManager.Initialize();

        SceneEntity* instance = prefab->Instantiate(sceneManager);
        TEST_ASSERT_NOT_NULL(instance);
        auto* prefabInstance = instance->GetComponent<PrefabInstance>();
        TEST_ASSERT_NOT_NULL(prefabInstance);

        instance->SetPosition(Vec3(9.0f, 9.0f, 9.0f));
        prefabInstance->AddOverride({"", "position", Vec3(9.0f, 9.0f, 9.0f)});

        prefabInstance->RevertProperty("", "position");

        TEST_ASSERT_EQ(Vec3(1.0f, 2.0f, 3.0f), instance->GetPosition());
        TEST_ASSERT_FALSE(prefabInstance->IsOverridden("", "position"));
        TEST_ASSERT_TRUE(prefabInstance->GetOverrides().empty());

        sceneManager.Shutdown();
        return true;
    }

    bool Test_PrefabInstanceRevertPropertyClearsRootTargetAliases()
    {
        PrefabEntityData data;
        data.name = "AliasRootTarget";
        data.position = Vec3(1.0f, 2.0f, 3.0f);

        auto prefab = Prefab::CreateFromData({data});

        SceneManager sceneManager;
        sceneManager.Initialize();

        SceneEntity* instance = prefab->Instantiate(sceneManager);
        TEST_ASSERT_NOT_NULL(instance);
        auto* prefabInstance = instance->GetComponent<PrefabInstance>();
        TEST_ASSERT_NOT_NULL(prefabInstance);

        instance->SetPosition(Vec3(9.0f, 9.0f, 9.0f));
        prefabInstance->AddOverride({"SceneEntity", "position", Vec3(9.0f, 9.0f, 9.0f)});

        prefabInstance->RevertProperty("Actor", "position");

        TEST_ASSERT_EQ(Vec3(1.0f, 2.0f, 3.0f), instance->GetPosition());
        TEST_ASSERT_FALSE(prefabInstance->IsOverridden("SceneEntity", "position"));
        TEST_ASSERT_TRUE(prefabInstance->GetOverrides().empty());

        sceneManager.Shutdown();
        return true;
    }

    bool Test_PrefabInstanceRevertPropertyKeepsUnsupportedOverride()
    {
        PrefabEntityData data;
        data.name = "UnsupportedOverrideRoot";

        auto prefab = Prefab::CreateFromData({data});

        SceneManager sceneManager;
        sceneManager.Initialize();

        SceneEntity* instance = prefab->Instantiate(sceneManager);
        TEST_ASSERT_NOT_NULL(instance);
        auto* prefabInstance = instance->GetComponent<PrefabInstance>();
        TEST_ASSERT_NOT_NULL(prefabInstance);

        prefabInstance->AddOverride({"UnknownComponent", "payload", std::string("custom")});

        prefabInstance->RevertProperty("UnknownComponent", "payload");

        TEST_ASSERT_TRUE(prefabInstance->IsOverridden("UnknownComponent", "payload"));
        TEST_ASSERT_EQ(static_cast<size_t>(1), prefabInstance->GetOverrides().size());

        sceneManager.Shutdown();
        return true;
    }

    bool Test_PrefabInstanceApplyToPrefabWritesRootEntityState()
    {
        PrefabEntityData data;
        data.name = "ApplyRoot";
        data.position = Vec3(1.0f, 2.0f, 3.0f);
        data.rotation = Quat(1.0f, 0.0f, 0.0f, 0.0f);
        data.scale = Vec3(1.0f, 1.0f, 1.0f);
        data.layerMask = 0x1u;
        data.isActive = true;

        auto prefab = Prefab::CreateFromData({data});

        SceneManager sceneManager;
        sceneManager.Initialize();

        SceneEntity* instance = prefab->Instantiate(sceneManager);
        TEST_ASSERT_NOT_NULL(instance);
        auto* prefabInstance = instance->GetComponent<PrefabInstance>();
        TEST_ASSERT_NOT_NULL(prefabInstance);

        instance->SetPosition(Vec3(9.0f, 8.0f, 7.0f));
        instance->SetRotation(Quat(0.7071068f, 0.0f, 0.7071068f, 0.0f));
        instance->SetScale(Vec3(3.0f, 4.0f, 5.0f));
        instance->SetLayerMask(0xAu);
        instance->SetActive(false);
        prefabInstance->AddOverride({"SceneEntity", "position", Vec3(9.0f, 8.0f, 7.0f)});
        prefabInstance->AddOverride({"SceneEntity", "scale", Vec3(3.0f, 4.0f, 5.0f)});

        prefabInstance->ApplyToPrefab();

        const PrefabEntityData* rootData = prefab->GetRootData();
        TEST_ASSERT_NOT_NULL(rootData);
        TEST_ASSERT_EQ(Vec3(9.0f, 8.0f, 7.0f), rootData->position);
        TEST_ASSERT_EQ(Quat(0.7071068f, 0.0f, 0.7071068f, 0.0f), rootData->rotation);
        TEST_ASSERT_EQ(Vec3(3.0f, 4.0f, 5.0f), rootData->scale);
        TEST_ASSERT_EQ(static_cast<uint32_t>(0xAu), rootData->layerMask);
        TEST_ASSERT_FALSE(rootData->isActive);
        TEST_ASSERT_TRUE(prefabInstance->GetOverrides().empty());

        sceneManager.Shutdown();
        return true;
    }

    bool Test_PrefabInstanceApplyToPrefabWritesRootComponentPayloads()
    {
        ComponentFactoryClassGuard componentFactoryGuard;
        ComponentFactory::RegisterComponentClass<PrefabPayloadComponent>(
            "PrefabPayloadComponent");
        ComponentFactory::RegisterComponentClass<PrefabLegacyPayloadComponent>(
            "PrefabLegacyPayloadComponent");

        PrefabEntityData data;
        data.name = "ApplyComponentPayloadsRoot";
        data.actorComponentData.push_back({"PrefabPayloadComponent", "ammo=12"});
        data.componentData["PrefabLegacyPayloadComponent"] = "legacy=old";

        auto prefab = Prefab::CreateFromData({data});

        SceneManager sceneManager;
        sceneManager.Initialize();

        SceneEntity* instance = prefab->Instantiate(sceneManager);
        TEST_ASSERT_NOT_NULL(instance);
        auto* prefabInstance = instance->GetComponent<PrefabInstance>();
        TEST_ASSERT_NOT_NULL(prefabInstance);

        auto* actorComponent = static_cast<Actor*>(instance)->GetComponent<PrefabPayloadComponent>();
        TEST_ASSERT_NOT_NULL(actorComponent);
        actorComponent->payload = "ammo=99;mode=auto";

        auto* legacyComponent = instance->GetComponent<PrefabLegacyPayloadComponent>();
        TEST_ASSERT_NOT_NULL(legacyComponent);
        legacyComponent->payload = "legacy=new";

        prefabInstance->AddOverride({"PrefabPayloadComponent", "serializedData", actorComponent->payload});
        prefabInstance->AddOverride({"PrefabLegacyPayloadComponent", "serializedData", legacyComponent->payload});
        prefabInstance->AddOverride({"UnknownComponent", "serializedData", std::string("keep")});

        prefabInstance->ApplyToPrefab();

        const PrefabEntityData* rootData = prefab->GetRootData();
        TEST_ASSERT_NOT_NULL(rootData);
        TEST_ASSERT_EQ(static_cast<size_t>(1), rootData->actorComponentData.size());
        TEST_ASSERT_EQ(std::string("PrefabPayloadComponent"),
                       rootData->actorComponentData[0].className);
        TEST_ASSERT_EQ(std::string("ammo=99;mode=auto"),
                       rootData->actorComponentData[0].serializedData);

        auto it = rootData->componentData.find("PrefabLegacyPayloadComponent");
        TEST_ASSERT_TRUE(it != rootData->componentData.end());
        TEST_ASSERT_EQ(std::string("legacy=new"), it->second);

        TEST_ASSERT_FALSE(prefabInstance->IsOverridden("PrefabPayloadComponent", "serializedData"));
        TEST_ASSERT_FALSE(prefabInstance->IsOverridden("PrefabLegacyPayloadComponent", "serializedData"));
        TEST_ASSERT_TRUE(prefabInstance->IsOverridden("UnknownComponent", "serializedData"));

        sceneManager.Shutdown();
        return true;
    }

    bool Test_PrefabInstanceApplyToPrefabClearsOnlyMatchingNamedPayloadOverride()
    {
        ComponentFactoryClassGuard componentFactoryGuard;
        ComponentFactory::RegisterComponentClass<PrefabPayloadComponent>(
            "PrefabPayloadComponent");

        PrefabEntityData data;
        data.name = "ApplyNamedComponentPayloadOverrideRoot";
        data.actorComponentData.push_back({"PrefabPayloadComponent", "slot=primary", "PrimaryPayload"});
        data.actorComponentData.push_back({"PrefabPayloadComponent", "slot=secondary", "SecondaryPayload"});

        auto prefab = Prefab::CreateFromData({data});

        SceneManager sceneManager;
        sceneManager.Initialize();

        SceneEntity* instance = prefab->Instantiate(sceneManager);
        TEST_ASSERT_NOT_NULL(instance);
        auto* prefabInstance = instance->GetComponent<PrefabInstance>();
        TEST_ASSERT_NOT_NULL(prefabInstance);

        auto* primary = FindPrefabPayloadComponentByName(*static_cast<Actor*>(instance),
                                                         "PrimaryPayload");
        auto* secondary = FindPrefabPayloadComponentByName(*static_cast<Actor*>(instance),
                                                           "SecondaryPayload");
        TEST_ASSERT_NOT_NULL(primary);
        TEST_ASSERT_NOT_NULL(secondary);

        primary->payload = "slot=primary-applied";
        prefabInstance->AddOverride(
            {"PrefabPayloadComponent", "serializedData", primary->payload, "PrimaryPayload"});
        prefabInstance->AddOverride(
            {"PrefabPayloadComponent", "serializedData", std::string("stale"), "DetachedPayload"});

        prefabInstance->ApplyToPrefab();

        const PrefabEntityData* rootData = prefab->GetRootData();
        TEST_ASSERT_NOT_NULL(rootData);
        TEST_ASSERT_EQ(static_cast<size_t>(2), rootData->actorComponentData.size());
        TEST_ASSERT_FALSE(
            prefabInstance->IsOverridden("PrefabPayloadComponent", "serializedData", "PrimaryPayload"));
        TEST_ASSERT_TRUE(
            prefabInstance->IsOverridden("PrefabPayloadComponent", "serializedData", "DetachedPayload"));
        TEST_ASSERT_EQ(static_cast<size_t>(1), prefabInstance->GetOverrides().size());

        sceneManager.Shutdown();
        return true;
    }

    bool Test_PrefabInstanceApplyToPrefabWritesChildEntityStateAndPayload()
    {
        ComponentFactoryClassGuard componentFactoryGuard;
        ComponentFactory::RegisterComponentClass<PrefabPayloadComponent>(
            "PrefabPayloadComponent");

        PrefabEntityData rootData;
        rootData.name = "ApplyChildRoot";

        PrefabEntityData childData;
        childData.name = "Child";
        childData.parentIndex = 0;
        childData.position = Vec3(0.0f, 2.0f, 0.0f);
        childData.actorComponentData.push_back(
            {"PrefabPayloadComponent", "slot=old", "ChildPayload"});

        auto prefab = Prefab::CreateFromData({rootData, childData});

        SceneManager sceneManager;
        sceneManager.Initialize();

        SceneEntity* root = prefab->Instantiate(sceneManager);
        TEST_ASSERT_NOT_NULL(root);
        auto* prefabInstance = root->GetComponent<PrefabInstance>();
        TEST_ASSERT_NOT_NULL(prefabInstance);
        TEST_ASSERT_EQ(static_cast<size_t>(1), root->GetChildren().size());
        SceneEntity* child = root->GetChildren()[0];
        auto* childPayload = FindPrefabPayloadComponentByName(
            *static_cast<Actor*>(child), "ChildPayload");
        TEST_ASSERT_NOT_NULL(childPayload);

        child->SetPosition(Vec3(0.0f, 7.0f, 0.0f));
        childPayload->payload = "slot=new";
        prefabInstance->AddOverride(
            {"SceneEntity", "position", child->GetPosition(), "", "Child"});
        prefabInstance->AddOverride(
            {"PrefabPayloadComponent", "serializedData", childPayload->payload, "ChildPayload", "Child"});

        prefabInstance->ApplyToPrefab();

        const PrefabEntityData* updatedChildData = prefab->GetEntityData(1);
        TEST_ASSERT_NOT_NULL(updatedChildData);
        TEST_ASSERT_EQ(Vec3(0.0f, 7.0f, 0.0f), updatedChildData->position);
        TEST_ASSERT_EQ(static_cast<size_t>(1), updatedChildData->actorComponentData.size());
        TEST_ASSERT_EQ(std::string("slot=new"),
                       updatedChildData->actorComponentData[0].serializedData);
        TEST_ASSERT_FALSE(prefabInstance->IsOverridden("SceneEntity", "position", "", "Child"));
        TEST_ASSERT_FALSE(prefabInstance->IsOverridden(
            "PrefabPayloadComponent", "serializedData", "ChildPayload", "Child"));

        sceneManager.Shutdown();
        return true;
    }

    bool Test_PrefabInstanceApplyToPrefabAddsLiveChildEntity()
    {
        ComponentFactoryClassGuard componentFactoryGuard;
        ComponentFactory::RegisterComponentClass<PrefabPayloadComponent>(
            "PrefabPayloadComponent");

        PrefabEntityData rootData;
        rootData.name = "ApplyAddsChildRoot";

        auto prefab = Prefab::CreateFromData({rootData});

        SceneManager sceneManager;
        sceneManager.Initialize();

        SceneEntity* root = prefab->Instantiate(sceneManager);
        TEST_ASSERT_NOT_NULL(root);
        auto* prefabInstance = root->GetComponent<PrefabInstance>();
        TEST_ASSERT_NOT_NULL(prefabInstance);

        ActorSpawnParams childParams;
        childParams.name = "AddedChild";
        childParams.parent = root;
        SceneEntity* child = sceneManager.SpawnActor(childParams);
        TEST_ASSERT_NOT_NULL(child);
        child->SetPosition(Vec3(4.0f, 5.0f, 6.0f));
        auto* payload = child->AddComponent<PrefabPayloadComponent>();
        TEST_ASSERT_NOT_NULL(payload);
        payload->SetName("AddedPayload");
        payload->payload = "slot=added";

        prefabInstance->ApplyToPrefab();

        TEST_ASSERT_EQ(static_cast<size_t>(2), prefab->GetEntityCount());
        const PrefabEntityData* addedData = prefab->GetEntityData(1);
        TEST_ASSERT_NOT_NULL(addedData);
        TEST_ASSERT_EQ(std::string("AddedChild"), addedData->name);
        TEST_ASSERT_EQ(static_cast<int32_t>(0), addedData->parentIndex);
        TEST_ASSERT_EQ(Vec3(4.0f, 5.0f, 6.0f), addedData->position);
        TEST_ASSERT_EQ(static_cast<size_t>(1), addedData->actorComponentData.size());
        TEST_ASSERT_EQ(std::string("PrefabPayloadComponent"),
                       addedData->actorComponentData[0].className);
        TEST_ASSERT_EQ(std::string("AddedPayload"),
                       addedData->actorComponentData[0].name);
        TEST_ASSERT_EQ(std::string("slot=added"),
                       addedData->actorComponentData[0].serializedData);

        sceneManager.Shutdown();
        return true;
    }

    bool Test_PrefabInstanceApplyToPrefabRemovesDeletedChildEntityAndOverrides()
    {
        PrefabEntityData rootData;
        rootData.name = "ApplyRemovesChildRoot";

        PrefabEntityData childData;
        childData.name = "Child";
        childData.parentIndex = 0;
        childData.position = Vec3(1.0f, 2.0f, 3.0f);

        auto prefab = Prefab::CreateFromData({rootData, childData});

        SceneManager sceneManager;
        sceneManager.Initialize();

        SceneEntity* root = prefab->Instantiate(sceneManager);
        TEST_ASSERT_NOT_NULL(root);
        auto* prefabInstance = root->GetComponent<PrefabInstance>();
        TEST_ASSERT_NOT_NULL(prefabInstance);
        TEST_ASSERT_EQ(static_cast<size_t>(1), root->GetChildren().size());

        SceneEntity* child = root->GetChildren()[0];
        prefabInstance->AddOverride(
            {"SceneEntity", "position", Vec3(9.0f, 9.0f, 9.0f), "", "Child"});
        sceneManager.DestroyEntity(child->GetHandle());

        prefabInstance->ApplyToPrefab();

        TEST_ASSERT_EQ(static_cast<size_t>(1), prefab->GetEntityCount());
        TEST_ASSERT_FALSE(prefabInstance->IsOverridden("SceneEntity", "position", "", "Child"));

        sceneManager.Shutdown();
        return true;
    }

    bool Test_PrefabInstanceApplyToPrefabPreservesNestedParentIndices()
    {
        PrefabEntityData rootData;
        rootData.name = "ApplyNestedRoot";

        auto prefab = Prefab::CreateFromData({rootData});

        SceneManager sceneManager;
        sceneManager.Initialize();

        SceneEntity* root = prefab->Instantiate(sceneManager);
        TEST_ASSERT_NOT_NULL(root);
        auto* prefabInstance = root->GetComponent<PrefabInstance>();
        TEST_ASSERT_NOT_NULL(prefabInstance);

        ActorSpawnParams branchParams;
        branchParams.name = "Branch";
        branchParams.parent = root;
        SceneEntity* branch = sceneManager.SpawnActor(branchParams);
        TEST_ASSERT_NOT_NULL(branch);

        ActorSpawnParams leafParams;
        leafParams.name = "Leaf";
        leafParams.parent = branch;
        SceneEntity* leaf = sceneManager.SpawnActor(leafParams);
        TEST_ASSERT_NOT_NULL(leaf);
        leaf->SetScale(Vec3(2.0f, 3.0f, 4.0f));

        prefabInstance->ApplyToPrefab();

        TEST_ASSERT_EQ(static_cast<size_t>(3), prefab->GetEntityCount());
        const PrefabEntityData* branchData = prefab->GetEntityData(1);
        const PrefabEntityData* leafData = prefab->GetEntityData(2);
        TEST_ASSERT_NOT_NULL(branchData);
        TEST_ASSERT_NOT_NULL(leafData);
        TEST_ASSERT_EQ(std::string("Branch"), branchData->name);
        TEST_ASSERT_EQ(static_cast<int32_t>(0), branchData->parentIndex);
        TEST_ASSERT_EQ(std::string("Leaf"), leafData->name);
        TEST_ASSERT_EQ(static_cast<int32_t>(1), leafData->parentIndex);
        TEST_ASSERT_EQ(Vec3(2.0f, 3.0f, 4.0f), leafData->scale);

        sceneManager.Shutdown();
        return true;
    }

    bool Test_PrefabInstanceApplyToPrefabPreservesUnsupportedOverrideOnExistingChild()
    {
        PrefabEntityData rootData;
        rootData.name = "ApplyKeepsUnsupportedRoot";

        PrefabEntityData childData;
        childData.name = "Child";
        childData.parentIndex = 0;

        auto prefab = Prefab::CreateFromData({rootData, childData});

        SceneManager sceneManager;
        sceneManager.Initialize();

        SceneEntity* root = prefab->Instantiate(sceneManager);
        TEST_ASSERT_NOT_NULL(root);
        auto* prefabInstance = root->GetComponent<PrefabInstance>();
        TEST_ASSERT_NOT_NULL(prefabInstance);

        prefabInstance->AddOverride(
            {"CustomComponent", "customField", std::string("value"), "", "Child"});

        prefabInstance->ApplyToPrefab();

        TEST_ASSERT_TRUE(prefabInstance->IsOverridden(
            "CustomComponent", "customField", "", "Child"));

        sceneManager.Shutdown();
        return true;
    }

    bool Test_PrefabInstanceApplyToPrefabClearsRootAliasOverrides()
    {
        PrefabEntityData data;
        data.name = "ApplyAliases";
        data.position = Vec3(1.0f, 2.0f, 3.0f);
        data.scale = Vec3(1.0f, 1.0f, 1.0f);

        auto prefab = Prefab::CreateFromData({data});

        SceneManager sceneManager;
        sceneManager.Initialize();

        SceneEntity* instance = prefab->Instantiate(sceneManager);
        TEST_ASSERT_NOT_NULL(instance);
        auto* prefabInstance = instance->GetComponent<PrefabInstance>();
        TEST_ASSERT_NOT_NULL(prefabInstance);

        instance->SetPosition(Vec3(5.0f, 6.0f, 7.0f));
        instance->SetScale(Vec3(2.0f, 2.0f, 2.0f));
        prefabInstance->AddOverride({"", "position", Vec3(5.0f, 6.0f, 7.0f)});
        prefabInstance->AddOverride({"Actor", "scale", Vec3(2.0f, 2.0f, 2.0f)});

        prefabInstance->ApplyToPrefab();

        TEST_ASSERT_FALSE(prefabInstance->IsOverridden("", "position"));
        TEST_ASSERT_FALSE(prefabInstance->IsOverridden("Actor", "scale"));
        TEST_ASSERT_TRUE(prefabInstance->GetOverrides().empty());

        const PrefabEntityData* rootData = prefab->GetRootData();
        TEST_ASSERT_NOT_NULL(rootData);
        TEST_ASSERT_EQ(Vec3(5.0f, 6.0f, 7.0f), rootData->position);
        TEST_ASSERT_EQ(Vec3(2.0f, 2.0f, 2.0f), rootData->scale);

        sceneManager.Shutdown();
        return true;
    }

    bool Test_PrefabInstanceApplyToPrefabPreservesUnsupportedOverrides()
    {
        PrefabEntityData data;
        data.name = "ApplyUnsupported";
        data.position = Vec3(1.0f, 2.0f, 3.0f);

        auto prefab = Prefab::CreateFromData({data});

        SceneManager sceneManager;
        sceneManager.Initialize();

        SceneEntity* instance = prefab->Instantiate(sceneManager);
        TEST_ASSERT_NOT_NULL(instance);
        auto* prefabInstance = instance->GetComponent<PrefabInstance>();
        TEST_ASSERT_NOT_NULL(prefabInstance);

        instance->SetPosition(Vec3(4.0f, 5.0f, 6.0f));
        prefabInstance->AddOverride({"SceneEntity", "position", Vec3(4.0f, 5.0f, 6.0f)});
        prefabInstance->AddOverride({"SceneEntity", "custom", std::string("root-custom")});
        prefabInstance->AddOverride({"UnknownComponent", "payload", std::string("component-custom")});

        prefabInstance->ApplyToPrefab();

        TEST_ASSERT_FALSE(prefabInstance->IsOverridden("SceneEntity", "position"));
        TEST_ASSERT_TRUE(prefabInstance->IsOverridden("SceneEntity", "custom"));
        TEST_ASSERT_TRUE(prefabInstance->IsOverridden("UnknownComponent", "payload"));
        TEST_ASSERT_EQ(static_cast<size_t>(2), prefabInstance->GetOverrides().size());

        const PrefabEntityData* rootData = prefab->GetRootData();
        TEST_ASSERT_NOT_NULL(rootData);
        TEST_ASSERT_EQ(Vec3(4.0f, 5.0f, 6.0f), rootData->position);

        sceneManager.Shutdown();
        return true;
    }

    bool Test_PrefabInstanceApplyToPrefabWithoutPrefabKeepsOverrides()
    {
        SceneManager sceneManager;
        sceneManager.Initialize();

        ActorSpawnParams params;
        params.name = "NoPrefabActor";
        SceneEntity* entity = sceneManager.SpawnActor(params);
        TEST_ASSERT_NOT_NULL(entity);

        auto* prefabInstance = entity->AddComponent<PrefabInstance>();
        TEST_ASSERT_NOT_NULL(prefabInstance);
        prefabInstance->AddOverride({"SceneEntity", "position", Vec3(1.0f, 2.0f, 3.0f)});

        prefabInstance->ApplyToPrefab();

        TEST_ASSERT_TRUE(prefabInstance->IsOverridden("SceneEntity", "position"));
        TEST_ASSERT_EQ(static_cast<size_t>(1), prefabInstance->GetOverrides().size());

        sceneManager.Shutdown();
        return true;
    }

    bool Test_WorldLoadModelResourceReplacesSceneContent()
    {
        ResourceManagerTestGuard resourceGuard(true);
        auto& resourceManager = ResourceManager::Get();
        resourceManager.RegisterLoader(ResourceType::Model, std::make_unique<WorldLoadModelLoader>());

        World world;
        world.Initialize();

        ActorSpawnParams existingParams;
        existingParams.name = "ExistingActor";
        TEST_ASSERT_NOT_NULL(world.SpawnActor(existingParams));
        TEST_ASSERT_EQ(static_cast<size_t>(1), world.GetSceneManager()->GetEntityCount());

        world.Load("fixture.model");

        SceneManager* sceneManager = world.GetSceneManager();
        TEST_ASSERT_NOT_NULL(sceneManager);
        TEST_ASSERT_EQ(static_cast<size_t>(2), sceneManager->GetEntityCount());

        auto* root = FindEntityByName(*sceneManager, "LoadedWorldRoot");
        auto* child = FindEntityByName(*sceneManager, "LoadedWorldChild");
        TEST_ASSERT_NOT_NULL(root);
        TEST_ASSERT_NOT_NULL(child);
        TEST_ASSERT_EQ(nullptr, FindEntityByName(*sceneManager, "ExistingActor"));
        TEST_ASSERT_EQ(Vec3(1.0f, 2.0f, 3.0f), root->GetPosition());
        TEST_ASSERT_EQ(root, child->GetParent());
        TEST_ASSERT_EQ(Vec3(4.0f, 5.0f, 6.0f), child->GetPosition());
        TEST_ASSERT_EQ(static_cast<Actor*>(root), world.GetActor(root->GetHandle()));

        world.Shutdown();
        return true;
    }

    bool Test_WorldLoadInvalidRequestKeepsExistingContent()
    {
        ResourceManagerTestGuard resourceGuard(false);

        World world;
        world.Initialize();

        ActorSpawnParams params;
        params.name = "PersistentActor";
        SceneEntity* existing = world.SpawnActor(params);
        TEST_ASSERT_NOT_NULL(existing);

        world.Load("missing.model");

        TEST_ASSERT_EQ(static_cast<size_t>(1), world.GetSceneManager()->GetEntityCount());
        TEST_ASSERT_EQ(existing, FindEntityByName(*world.GetSceneManager(), "PersistentActor"));

        world.Shutdown();
        return true;
    }

    bool Test_WorldLoadEmptyModelKeepsExistingContent()
    {
        ResourceManagerTestGuard resourceGuard(true);
        auto& resourceManager = ResourceManager::Get();
        resourceManager.RegisterLoader(ResourceType::Model, std::make_unique<WorldLoadEmptyModelLoader>());

        World world;
        world.Initialize();

        ActorSpawnParams params;
        params.name = "PersistentActor";
        SceneEntity* existing = world.SpawnActor(params);
        TEST_ASSERT_NOT_NULL(existing);

        world.Load("empty.model");

        TEST_ASSERT_EQ(static_cast<size_t>(1), world.GetSceneManager()->GetEntityCount());
        TEST_ASSERT_EQ(existing, FindEntityByName(*world.GetSceneManager(), "PersistentActor"));

        world.Shutdown();
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
    suite.AddTest("PrefabSerializesActorComponentPayloads",
                  Test_PrefabSerializesActorComponentPayloads);
    suite.AddTest("PrefabSerializesActorComponentNames",
                  Test_PrefabSerializesActorComponentNames);
    suite.AddTest("PrefabSerializesLegacyComponentPayloads",
                  Test_PrefabSerializesLegacyComponentPayloads);
    suite.AddTest("PrefabSerializesActorClassNames",
                  Test_PrefabSerializesActorClassNames);
    suite.AddTest("PrefabInstantiatesRegisteredActorClasses",
                  Test_PrefabInstantiatesRegisteredActorClasses);
    suite.AddTest("PrefabInstantiateFailsForMissingActorClassAndCleansUp",
                  Test_PrefabInstantiateFailsForMissingActorClassAndCleansUp);
    suite.AddTest("PrefabInstantiateFailsForMissingActorComponentClassAndCleansUp",
                  Test_PrefabInstantiateFailsForMissingActorComponentClassAndCleansUp);
    suite.AddTest("PrefabInstantiateFailsForMissingLegacyComponentClassAndCleansUp",
                  Test_PrefabInstantiateFailsForMissingLegacyComponentClassAndCleansUp);
    suite.AddTest("PrefabInstantiateFailsForActorOnlyComponentDataAndCleansUp",
                  Test_PrefabInstantiateFailsForActorOnlyComponentDataAndCleansUp);
    suite.AddTest("PrefabInstantiateFailsForRejectedActorComponentAndCleansUp",
                  Test_PrefabInstantiateFailsForRejectedActorComponentAndCleansUp);
    suite.AddTest("PrefabInstantiatesRegisteredActorComponentClasses",
                  Test_PrefabInstantiatesRegisteredActorComponentClasses);
    suite.AddTest("PrefabInstantiatesActorComponentPayloads",
                  Test_PrefabInstantiatesActorComponentPayloads);
    suite.AddTest("PrefabInstantiatesActorComponentNames",
                  Test_PrefabInstantiatesActorComponentNames);
    suite.AddTest("PrefabInstantiatesRegisteredLegacyComponentPayloads",
                  Test_PrefabInstantiatesRegisteredLegacyComponentPayloads);
    suite.AddTest("PrefabInstantiateAsChildBuildsSpawnedHierarchy",
                  Test_PrefabInstantiateAsChildBuildsSpawnedHierarchy);
    suite.AddTest("PrefabInstanceRevertAllRestoresRootEntityState",
                  Test_PrefabInstanceRevertAllRestoresRootEntityState);
    suite.AddTest("PrefabInstanceRevertAllRestoresActorComponentPayload",
                  Test_PrefabInstanceRevertAllRestoresActorComponentPayload);
    suite.AddTest("PrefabInstanceRevertAllRestoresNamedDuplicateActorComponentPayloads",
                  Test_PrefabInstanceRevertAllRestoresNamedDuplicateActorComponentPayloads);
    suite.AddTest("PrefabInstanceRevertAllUsesRuntimeNameBindingAfterUniquify",
                  Test_PrefabInstanceRevertAllUsesRuntimeNameBindingAfterUniquify);
    suite.AddTest("PrefabInstanceRevertAllRestoresChildEntityStateAndPayload",
                  Test_PrefabInstanceRevertAllRestoresChildEntityStateAndPayload);
    suite.AddTest("PrefabInstanceRevertAllRestoresLegacyComponentPayload",
                  Test_PrefabInstanceRevertAllRestoresLegacyComponentPayload);
    suite.AddTest("PrefabInstanceRevertPropertyRestoresOnlyRequestedEntityProperty",
                  Test_PrefabInstanceRevertPropertyRestoresOnlyRequestedEntityProperty);
    suite.AddTest("PrefabInstanceRootEntityPathNormalizesForOverrideIdentity",
                  Test_PrefabInstanceRootEntityPathNormalizesForOverrideIdentity);
    suite.AddTest("PrefabInstanceRevertPropertyRestoresChildEntityProperty",
                  Test_PrefabInstanceRevertPropertyRestoresChildEntityProperty);
    suite.AddTest("PrefabInstanceRevertPropertyUsesSiblingOrdinalEntityPath",
                  Test_PrefabInstanceRevertPropertyUsesSiblingOrdinalEntityPath);
    suite.AddTest("PrefabInstanceRevertPropertyKeepsAmbiguousDuplicateSiblingPath",
                  Test_PrefabInstanceRevertPropertyKeepsAmbiguousDuplicateSiblingPath);
    suite.AddTest("PrefabInstanceRevertPropertyRestoresActorComponentPayload",
                  Test_PrefabInstanceRevertPropertyRestoresActorComponentPayload);
    suite.AddTest("PrefabInstanceRevertPropertyRestoresNamedDuplicateActorComponentPayload",
                  Test_PrefabInstanceRevertPropertyRestoresNamedDuplicateActorComponentPayload);
    suite.AddTest("PrefabInstanceRemoveOverrideTargetsComponentName",
                  Test_PrefabInstanceRemoveOverrideTargetsComponentName);
    suite.AddTest("PrefabInstanceRevertPropertyUsesRuntimeNameBindingAfterUniquify",
                  Test_PrefabInstanceRevertPropertyUsesRuntimeNameBindingAfterUniquify);
    suite.AddTest("PrefabInstanceRevertPropertyRestoresChildNamedComponentPayload",
                  Test_PrefabInstanceRevertPropertyRestoresChildNamedComponentPayload);
    suite.AddTest("PrefabInstanceRevertPropertyScopesComponentBindingsByEntityPath",
                  Test_PrefabInstanceRevertPropertyScopesComponentBindingsByEntityPath);
    suite.AddTest("PrefabInstanceRevertPropertyRestoresLegacyComponentPayload",
                  Test_PrefabInstanceRevertPropertyRestoresLegacyComponentPayload);
    suite.AddTest("PrefabInstanceRevertPropertyKeepsPayloadOverrideWhenComponentMissing",
                  Test_PrefabInstanceRevertPropertyKeepsPayloadOverrideWhenComponentMissing);
    suite.AddTest("PrefabInstanceRevertPropertyAcceptsEmptyRootTarget",
                  Test_PrefabInstanceRevertPropertyAcceptsEmptyRootTarget);
    suite.AddTest("PrefabInstanceRevertPropertyClearsRootTargetAliases",
                  Test_PrefabInstanceRevertPropertyClearsRootTargetAliases);
    suite.AddTest("PrefabInstanceRevertPropertyKeepsUnsupportedOverride",
                  Test_PrefabInstanceRevertPropertyKeepsUnsupportedOverride);
    suite.AddTest("PrefabInstanceApplyToPrefabWritesRootEntityState",
                  Test_PrefabInstanceApplyToPrefabWritesRootEntityState);
    suite.AddTest("PrefabInstanceApplyToPrefabWritesRootComponentPayloads",
                  Test_PrefabInstanceApplyToPrefabWritesRootComponentPayloads);
    suite.AddTest("PrefabInstanceApplyToPrefabClearsOnlyMatchingNamedPayloadOverride",
                  Test_PrefabInstanceApplyToPrefabClearsOnlyMatchingNamedPayloadOverride);
    suite.AddTest("PrefabInstanceApplyToPrefabWritesChildEntityStateAndPayload",
                  Test_PrefabInstanceApplyToPrefabWritesChildEntityStateAndPayload);
    suite.AddTest("PrefabInstanceApplyToPrefabAddsLiveChildEntity",
                  Test_PrefabInstanceApplyToPrefabAddsLiveChildEntity);
    suite.AddTest("PrefabInstanceApplyToPrefabRemovesDeletedChildEntityAndOverrides",
                  Test_PrefabInstanceApplyToPrefabRemovesDeletedChildEntityAndOverrides);
    suite.AddTest("PrefabInstanceApplyToPrefabPreservesNestedParentIndices",
                  Test_PrefabInstanceApplyToPrefabPreservesNestedParentIndices);
    suite.AddTest("PrefabInstanceApplyToPrefabPreservesUnsupportedOverrideOnExistingChild",
                  Test_PrefabInstanceApplyToPrefabPreservesUnsupportedOverrideOnExistingChild);
    suite.AddTest("PrefabInstanceApplyToPrefabClearsRootAliasOverrides",
                  Test_PrefabInstanceApplyToPrefabClearsRootAliasOverrides);
    suite.AddTest("PrefabInstanceApplyToPrefabPreservesUnsupportedOverrides",
                  Test_PrefabInstanceApplyToPrefabPreservesUnsupportedOverrides);
    suite.AddTest("PrefabInstanceApplyToPrefabWithoutPrefabKeepsOverrides",
                  Test_PrefabInstanceApplyToPrefabWithoutPrefabKeepsOverrides);
    suite.AddTest("WorldLoadModelResourceReplacesSceneContent",
                  Test_WorldLoadModelResourceReplacesSceneContent);
    suite.AddTest("WorldLoadInvalidRequestKeepsExistingContent",
                  Test_WorldLoadInvalidRequestKeepsExistingContent);
    suite.AddTest("WorldLoadEmptyModelKeepsExistingContent",
                  Test_WorldLoadEmptyModelKeepsExistingContent);

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
