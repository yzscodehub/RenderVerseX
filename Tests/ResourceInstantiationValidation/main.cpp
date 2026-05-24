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
#include "World/World.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

using namespace RVX;
using namespace RVX::Resource;

namespace
{
    class LogEnvironment : public ::testing::Environment
    {
    public:
        void SetUp() override
        {
            Log::Initialize();
            RVX_CORE_INFO("Resource Instantiation Validation Tests");
        }

        void TearDown() override
        {
            Log::Shutdown();
        }
    };

    [[maybe_unused]] const auto* g_logEnvironment =
        ::testing::AddGlobalTestEnvironment(new LogEnvironment);

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

    TEST(ResourceInstantiationValidation, ActorAddOwnedComponentUsesNormalLifecycle)
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

        EXPECT_EQ(raw, inserted);
        EXPECT_EQ(static_cast<Actor*>(entity), raw->GetOwner());
        EXPECT_TRUE(raw->IsRegistered());
        EXPECT_TRUE(raw->IsInitialized());
        EXPECT_EQ(1, raw->created);
        EXPECT_EQ(1, raw->registered);
        EXPECT_EQ(1, raw->initialized);

        sceneManager.Shutdown();
    }

    TEST(ResourceInstantiationValidation, AddOwnedComponentRejectsLegacyComponentToAvoidContainerSplit)
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

        EXPECT_EQ(nullptr, inserted);
        EXPECT_EQ(initialActorCount, actor->GetActorComponentCount());
        EXPECT_EQ(initialLegacyCount, entity->GetComponentCount());
        EXPECT_FALSE(entity->HasComponent<OwnedLegacyComponent>());

        sceneManager.Shutdown();
    }

    TEST(ResourceInstantiationValidation, ModelResourceInstantiateActorUsesStaticMeshComponent)
    {
        ComponentFactory::RegisterDefaults();

        SceneManager sceneManager;
        sceneManager.Initialize();

        ModelResource model;
        FillIndexedModel(model);
        Actor* actor = model.InstantiateActor(&sceneManager);
        ASSERT_NE(nullptr, actor);

        auto* entity = dynamic_cast<SceneEntity*>(actor);
        ASSERT_NE(nullptr, entity);

        auto* primitive = actor->GetComponent<StaticMeshComponent>();
        ASSERT_NE(nullptr, primitive);
        EXPECT_TRUE(primitive->IsRegistered());
        EXPECT_EQ(actor, primitive->GetOwner());
        EXPECT_EQ(primitive, entity->GetComponent<StaticMeshComponent>());
        EXPECT_TRUE(entity->HasComponent<StaticMeshComponent>());
        EXPECT_FALSE(entity->HasComponent<MeshRendererComponent>());
        EXPECT_EQ(entity->GetRootComponent(), primitive->GetAttachParent());
        EXPECT_TRUE(primitive->HasValidMesh());
        EXPECT_EQ(static_cast<ResourceId>(1001), primitive->GetMesh().GetId());
        EXPECT_EQ(static_cast<ResourceId>(2001), primitive->GetMaterial(0).GetId());
        EXPECT_EQ(Vec3(3.0f, 0.0f, 0.0f), entity->GetPosition());
        EXPECT_TRUE(std::find(sceneManager.GetPrimitives().begin(),
                                   sceneManager.GetPrimitives().end(),
                                   primitive) != sceneManager.GetPrimitives().end());

        sceneManager.Shutdown();
    }

    TEST(ResourceInstantiationValidation, LegacyInstantiateDelegatesToActorPath)
    {
        ComponentFactory::RegisterDefaults();

        SceneManager sceneManager;
        sceneManager.Initialize();

        ModelResource model;
        FillIndexedModel(model);
        SceneEntity* entity = model.Instantiate(&sceneManager);
        ASSERT_NE(nullptr, entity);
        ASSERT_NE(nullptr, static_cast<Actor*>(entity)->GetComponent<StaticMeshComponent>());
        EXPECT_TRUE(entity->HasComponent<StaticMeshComponent>());
        EXPECT_FALSE(entity->HasComponent<MeshRendererComponent>());

        sceneManager.Shutdown();
    }

    TEST(ResourceInstantiationValidation, ModelResourceInstantiateActorBuildsSpawnedHierarchy)
    {
        ComponentFactory::RegisterDefaults();

        SceneManager sceneManager;
        sceneManager.Initialize();

        ModelResource model;
        FillHierarchicalIndexedModel(model);

        Actor* actor = model.InstantiateActor(&sceneManager);
        ASSERT_NE(nullptr, actor);

        auto* root = dynamic_cast<SceneEntity*>(actor);
        ASSERT_NE(nullptr, root);
        EXPECT_EQ(std::string("RootNode"), root->GetName());
        EXPECT_EQ(Vec3(1.0f, 2.0f, 3.0f), root->GetPosition());
        EXPECT_EQ(static_cast<size_t>(1), root->GetChildren().size());
        EXPECT_EQ(static_cast<size_t>(2), sceneManager.GetEntityCount());

        auto* child = root->GetChildren()[0];
        ASSERT_NE(nullptr, child);
        EXPECT_EQ(std::string("ChildNode"), child->GetName());
        EXPECT_EQ(root, child->GetParent());
        EXPECT_EQ(Vec3(4.0f, 5.0f, 6.0f), child->GetPosition());
        ASSERT_NE(nullptr, static_cast<Actor*>(child)->GetComponent<StaticMeshComponent>());

        sceneManager.Shutdown();
    }

    TEST(ResourceInstantiationValidation, ComponentFactoryDefaultCreatesStaticMeshComponent)
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
        ASSERT_NE(nullptr, component);
        auto* primitive = dynamic_cast<StaticMeshComponent*>(component);
        ASSERT_NE(nullptr, primitive);
        EXPECT_EQ(entity->GetRootComponent(), primitive->GetAttachParent());
        EXPECT_EQ(static_cast<ResourceId>(1001), primitive->GetMesh().GetId());

        sceneManager.Shutdown();
    }

    TEST(ResourceInstantiationValidation, ComponentFactoryRegisterDefaultsPreservesCustomCreators)
    {
        ComponentFactory::ClearAll();
        ComponentFactory::Register("CustomNoop",
                                   [](SceneEntity*, const Node*, const ModelResource*) -> ActorComponent* {
                                       return nullptr;
                                   });

        ComponentFactory::RegisterDefaults();

        EXPECT_TRUE(ComponentFactory::IsRegistered("CustomNoop"));
        EXPECT_TRUE(ComponentFactory::IsRegistered("StaticMesh"));

        ComponentFactory::ClearAll();
    }

    TEST(ResourceInstantiationValidation, PrefabSerializesActorComponentClassNames)
    {
        SceneManager sceneManager;
        sceneManager.Initialize();

        const auto handle = sceneManager.CreateEntity("PrefabSource");
        auto* entity = sceneManager.GetEntity(handle);
        auto* primitive = static_cast<Actor*>(entity)->AddComponent<StaticMeshComponent>();
        ASSERT_NE(nullptr, primitive);
        auto* legacyRenderer = entity->AddComponent<MeshRendererComponent>();
        ASSERT_NE(nullptr, legacyRenderer);

        auto prefab = Prefab::CreateFromEntity(entity);
        ASSERT_NE(nullptr, prefab.get());
        const auto* data = prefab->GetRootData();
        ASSERT_NE(nullptr, data);
        bool foundStaticMesh = false;
        bool foundLegacyRenderer = false;
        for (const auto& componentData : data->actorComponentData)
        {
            foundStaticMesh = foundStaticMesh || componentData.className == "StaticMeshComponent";
            foundLegacyRenderer = foundLegacyRenderer || componentData.className == "MeshRenderer";
        }
        EXPECT_TRUE(foundStaticMesh);
        EXPECT_FALSE(foundLegacyRenderer);
        EXPECT_TRUE(data->componentData.find("MeshRenderer") != data->componentData.end());

        sceneManager.Shutdown();
    }

    TEST(ResourceInstantiationValidation, PrefabSerializesActorComponentPayloads)
    {
        SceneManager sceneManager;
        sceneManager.Initialize();

        ActorSpawnParams params;
        params.name = "PayloadSource";
        SceneEntity* entity = sceneManager.SpawnActor(params);
        ASSERT_NE(nullptr, entity);

        auto* component = static_cast<Actor*>(entity)->AddComponent<PrefabPayloadComponent>();
        ASSERT_NE(nullptr, component);
        component->payload = "health=75;tag=elite";

        auto prefab = Prefab::CreateFromEntity(entity);
        ASSERT_NE(nullptr, prefab);
        const PrefabEntityData* data = prefab->GetRootData();
        ASSERT_NE(nullptr, data);
        EXPECT_EQ(static_cast<size_t>(1), data->actorComponentData.size());
        EXPECT_EQ(std::string("PrefabPayloadComponent"),
                       data->actorComponentData[0].className);
        EXPECT_EQ(std::string("health=75;tag=elite"),
                       data->actorComponentData[0].serializedData);

        sceneManager.Shutdown();
    }

    TEST(ResourceInstantiationValidation, PrefabSerializesActorComponentNames)
    {
        SceneManager sceneManager;
        sceneManager.Initialize();

        ActorSpawnParams params;
        params.name = "NamedPayloadSource";
        SceneEntity* entity = sceneManager.SpawnActor(params);
        ASSERT_NE(nullptr, entity);

        auto* component = static_cast<Actor*>(entity)->AddComponent<PrefabPayloadComponent>();
        ASSERT_NE(nullptr, component);
        component->SetName("PrimaryPayload");
        component->payload = "health=75;tag=elite";

        auto prefab = Prefab::CreateFromEntity(entity);
        ASSERT_NE(nullptr, prefab);
        const PrefabEntityData* data = prefab->GetRootData();
        ASSERT_NE(nullptr, data);
        EXPECT_EQ(static_cast<size_t>(1), data->actorComponentData.size());
        EXPECT_EQ(std::string("PrimaryPayload"), data->actorComponentData[0].name);

        sceneManager.Shutdown();
    }

    TEST(ResourceInstantiationValidation, PrefabSerializesLegacyComponentPayloads)
    {
        SceneManager sceneManager;
        sceneManager.Initialize();

        const auto handle = sceneManager.CreateEntity("LegacyPayloadSource");
        SceneEntity* entity = sceneManager.GetEntity(handle);
        ASSERT_NE(nullptr, entity);

        auto* component = entity->AddComponent<PrefabLegacyPayloadComponent>();
        ASSERT_NE(nullptr, component);
        component->payload = "legacy=enabled";

        auto prefab = Prefab::CreateFromEntity(entity);
        ASSERT_NE(nullptr, prefab);

        const PrefabEntityData* rootData = prefab->GetRootData();
        ASSERT_NE(nullptr, rootData);
        auto it = rootData->componentData.find("PrefabLegacyPayloadComponent");
        EXPECT_TRUE(it != rootData->componentData.end());
        EXPECT_EQ(std::string("legacy=enabled"), it->second);

        sceneManager.Shutdown();
    }

    TEST(ResourceInstantiationValidation, PrefabSerializesActorClassNames)
    {
        SceneManager sceneManager;
        sceneManager.Initialize();

        ActorSpawnParams rootParams;
        rootParams.name = "CustomPrefabRoot";
        auto* root = sceneManager.SpawnActor<PrefabCustomSceneActor>(rootParams);
        ASSERT_NE(nullptr, root);

        ActorSpawnParams childParams;
        childParams.name = "CustomPrefabChild";
        childParams.parent = root;
        auto* child = sceneManager.SpawnActor<PrefabCustomSceneActor>(childParams);
        ASSERT_NE(nullptr, child);

        auto prefab = Prefab::CreateFromEntity(root);
        ASSERT_NE(nullptr, prefab.get());
        EXPECT_EQ(static_cast<size_t>(2), prefab->GetEntityCount());

        const auto* rootData = prefab->GetEntityData(0);
        const auto* childData = prefab->GetEntityData(1);
        ASSERT_NE(nullptr, rootData);
        ASSERT_NE(nullptr, childData);
        EXPECT_EQ(std::string("PrefabCustomSceneActor"), rootData->actorClassName);
        EXPECT_EQ(std::string("PrefabCustomSceneActor"), childData->actorClassName);

        sceneManager.Shutdown();
    }

    TEST(ResourceInstantiationValidation, PrefabInstantiatesRegisteredActorClasses)
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
        ASSERT_NE(nullptr, root);
        ASSERT_NE(nullptr, dynamic_cast<PrefabCustomSceneActor*>(root));
        EXPECT_EQ(std::string("RegisteredPrefabRoot"), root->GetName());
        EXPECT_EQ(Vec3(1.0f, 2.0f, 3.0f), root->GetPosition());
        EXPECT_EQ(static_cast<size_t>(1), root->GetChildren().size());
        EXPECT_EQ(static_cast<size_t>(2), sceneManager.GetEntityCount());

        SceneEntity* child = root->GetChildren()[0];
        ASSERT_NE(nullptr, child);
        ASSERT_NE(nullptr, dynamic_cast<PrefabCustomSceneActor*>(child));
        EXPECT_EQ(root, child->GetParent());
        EXPECT_EQ(std::string("RegisteredPrefabChild"), child->GetName());
        EXPECT_EQ(Vec3(4.0f, 5.0f, 6.0f), child->GetPosition());

        sceneManager.Shutdown();
        ActorFactory::ClearAll();
    }

    TEST(ResourceInstantiationValidation, PrefabInstantiateFailsForMissingActorClassAndCleansUp)
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
        EXPECT_EQ(nullptr, root);
        EXPECT_EQ(static_cast<size_t>(0), sceneManager.GetEntityCount());

        sceneManager.Shutdown();
    }

    TEST(ResourceInstantiationValidation, PrefabInstantiateFailsForMissingActorComponentClassAndCleansUp)
    {
        ComponentFactoryClassGuard componentFactoryGuard;

        PrefabEntityData data;
        data.name = "MissingComponentActor";
        data.actorComponentData.push_back({"MissingActorComponentClass", ""});

        auto prefab = Prefab::CreateFromData({data});

        SceneManager sceneManager;
        sceneManager.Initialize();

        SceneEntity* instance = prefab->Instantiate(sceneManager);
        EXPECT_EQ(nullptr, instance);
        EXPECT_EQ(static_cast<size_t>(0), sceneManager.GetEntityCount());

        sceneManager.Shutdown();
    }

    TEST(ResourceInstantiationValidation, PrefabInstantiateFailsForMissingLegacyComponentClassAndCleansUp)
    {
        ComponentFactoryClassGuard componentFactoryGuard;

        PrefabEntityData data;
        data.name = "MissingLegacyComponentActor";
        data.componentData["MissingLegacyComponentClass"] = "";

        auto prefab = Prefab::CreateFromData({data});

        SceneManager sceneManager;
        sceneManager.Initialize();

        SceneEntity* instance = prefab->Instantiate(sceneManager);
        EXPECT_EQ(nullptr, instance);
        EXPECT_EQ(static_cast<size_t>(0), sceneManager.GetEntityCount());

        sceneManager.Shutdown();
    }

    TEST(ResourceInstantiationValidation, PrefabInstantiateFailsForActorOnlyComponentDataAndCleansUp)
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
        EXPECT_EQ(nullptr, instance);
        EXPECT_EQ(static_cast<size_t>(0), sceneManager.GetEntityCount());

        sceneManager.Shutdown();
    }

    TEST(ResourceInstantiationValidation, PrefabInstantiateFailsForRejectedActorComponentAndCleansUp)
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
        EXPECT_EQ(nullptr, instance);
        EXPECT_EQ(static_cast<size_t>(0), sceneManager.GetEntityCount());

        sceneManager.Shutdown();
    }

    TEST(ResourceInstantiationValidation, PrefabInstantiatesRegisteredActorComponentClasses)
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
        ASSERT_NE(nullptr, instance);
        auto* firstPrimitive = static_cast<Actor*>(instance)->GetComponent<StaticMeshComponent>();
        ASSERT_NE(nullptr, firstPrimitive);
        EXPECT_TRUE(firstPrimitive->IsRegistered());

        size_t staticMeshCount = 0;
        for (const auto& component : static_cast<Actor*>(instance)->GetActorComponents())
        {
            if (component && std::string(component->GetClassName()) == "StaticMeshComponent")
            {
                ++staticMeshCount;
            }
        }
        EXPECT_EQ(static_cast<size_t>(2), staticMeshCount);
        EXPECT_EQ(static_cast<size_t>(2), sceneManager.GetPrimitives().size());

        sceneManager.Shutdown();
    }

    TEST(ResourceInstantiationValidation, PrefabInstantiatesActorComponentPayloads)
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
        ASSERT_NE(nullptr, instance);

        auto* component = static_cast<Actor*>(instance)->GetComponent<PrefabPayloadComponent>();
        ASSERT_NE(nullptr, component);
        EXPECT_EQ(std::string("ammo=12;mode=burst"), component->payload);
        EXPECT_EQ(std::string("ammo=12;mode=burst"),
                       component->payloadObservedOnRegister);
        EXPECT_EQ(std::string("ammo=12;mode=burst"),
                       component->payloadObservedOnInitialize);
        EXPECT_TRUE(component->IsRegistered());
        EXPECT_TRUE(component->IsInitialized());

        sceneManager.Shutdown();
        ComponentFactory::ClearComponentClasses();
    }

    TEST(ResourceInstantiationValidation, PrefabInstantiatesActorComponentNames)
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
        ASSERT_NE(nullptr, instance);

        auto* component = static_cast<Actor*>(instance)->GetComponent<PrefabPayloadComponent>();
        ASSERT_NE(nullptr, component);
        EXPECT_EQ(std::string("PrimaryPayload"), component->GetName());
        EXPECT_EQ(std::string("ammo=12"), component->payload);

        sceneManager.Shutdown();
    }

    TEST(ResourceInstantiationValidation, PrefabInstantiatesRegisteredLegacyComponentPayloads)
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
        ASSERT_NE(nullptr, instance);

        auto* component = instance->GetComponent<PrefabLegacyPayloadComponent>();
        ASSERT_NE(nullptr, component);
        EXPECT_EQ(std::string("legacy=enabled"), component->payload);
        EXPECT_EQ(std::string("legacy=enabled"), component->payloadObservedOnAttach);

        sceneManager.Shutdown();
    }

    TEST(ResourceInstantiationValidation, PrefabInstantiateAsChildBuildsSpawnedHierarchy)
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
        ASSERT_NE(nullptr, parent);

        SceneEntity* root = prefab->InstantiateAsChild(parent);
        ASSERT_NE(nullptr, root);
        EXPECT_EQ(parent, root->GetParent());
        EXPECT_EQ(static_cast<size_t>(1), root->GetChildren().size());
        EXPECT_EQ(static_cast<size_t>(3), sceneManager.GetEntityCount());
        ASSERT_NE(nullptr, static_cast<Actor*>(root)->GetComponent<StaticMeshComponent>());

        auto* child = root->GetChildren()[0];
        ASSERT_NE(nullptr, child);
        EXPECT_EQ(std::string("PrefabChild"), child->GetName());
        EXPECT_EQ(root, child->GetParent());
        EXPECT_EQ(Vec3(0.0f, 2.0f, 0.0f), child->GetPosition());

        sceneManager.Shutdown();
    }

    TEST(ResourceInstantiationValidation, PrefabInstanceRevertAllRestoresRootEntityState)
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
        ASSERT_NE(nullptr, instance);
        auto* prefabInstance = instance->GetComponent<PrefabInstance>();
        ASSERT_NE(nullptr, prefabInstance);

        instance->SetPosition(Vec3(9.0f, 9.0f, 9.0f));
        instance->SetRotation(Quat(1.0f, 0.0f, 0.0f, 0.0f));
        instance->SetScale(Vec3(8.0f, 8.0f, 8.0f));
        instance->SetLayerMask(0xFu);
        instance->SetActive(true);
        prefabInstance->AddOverride({"SceneEntity", "position", Vec3(9.0f, 9.0f, 9.0f)});
        prefabInstance->AddOverride({"SceneEntity", "scale", Vec3(8.0f, 8.0f, 8.0f)});

        prefabInstance->RevertAll();

        EXPECT_EQ(Vec3(1.0f, 2.0f, 3.0f), instance->GetPosition());
        EXPECT_EQ(Quat(0.7071068f, 0.0f, 0.7071068f, 0.0f), instance->GetRotation());
        EXPECT_EQ(Vec3(2.0f, 3.0f, 4.0f), instance->GetScale());
        EXPECT_EQ(static_cast<uint32_t>(0x5u), instance->GetLayerMask());
        EXPECT_FALSE(instance->IsActive());
        EXPECT_TRUE(prefabInstance->GetOverrides().empty());

        sceneManager.Shutdown();
    }

    TEST(ResourceInstantiationValidation, PrefabInstanceRevertAllRestoresActorComponentPayload)
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
        ASSERT_NE(nullptr, instance);
        auto* prefabInstance = instance->GetComponent<PrefabInstance>();
        ASSERT_NE(nullptr, prefabInstance);

        auto* component = static_cast<Actor*>(instance)->GetComponent<PrefabPayloadComponent>();
        ASSERT_NE(nullptr, component);
        component->payload = "ammo=1;mode=single";
        prefabInstance->AddOverride({"PrefabPayloadComponent", "serializedData", component->payload});

        prefabInstance->RevertAll();

        EXPECT_EQ(std::string("ammo=12;mode=burst"), component->payload);
        EXPECT_TRUE(prefabInstance->GetOverrides().empty());

        sceneManager.Shutdown();
    }

    TEST(ResourceInstantiationValidation, PrefabInstanceRevertAllRecreatesMissingActorComponent)
    {
        ComponentFactoryClassGuard componentFactoryGuard;
        ComponentFactory::RegisterComponentClass<PrefabPayloadComponent>(
            "PrefabPayloadComponent");

        PrefabEntityData data;
        data.name = "RecreateMissingActorComponentRoot";
        data.actorComponentData.push_back(
            {"PrefabPayloadComponent", "ammo=12;mode=burst", "PrimaryPayload"});

        auto prefab = Prefab::CreateFromData({data});

        SceneManager sceneManager;
        sceneManager.Initialize();

        SceneEntity* instance = prefab->Instantiate(sceneManager);
        ASSERT_NE(nullptr, instance);
        auto* prefabInstance = instance->GetComponent<PrefabInstance>();
        ASSERT_NE(nullptr, prefabInstance);

        auto* actor = static_cast<Actor*>(instance);
        auto* component = FindPrefabPayloadComponentByName(*actor, "PrimaryPayload");
        ASSERT_NE(nullptr, component);
        EXPECT_TRUE(actor->RemoveComponent<PrefabPayloadComponent>());
        EXPECT_TRUE(FindPrefabPayloadComponentByName(*actor, "PrimaryPayload") == nullptr);

        prefabInstance->AddOverride(
            {"PrefabPayloadComponent",
             "serializedData",
             std::string("deleted"),
             "PrimaryPayload"});

        prefabInstance->RevertAll();

        auto* restored = FindPrefabPayloadComponentByName(*actor, "PrimaryPayload");
        ASSERT_NE(nullptr, restored);
        EXPECT_EQ(std::string("ammo=12;mode=burst"), restored->payload);
        EXPECT_TRUE(prefabInstance->GetOverrides().empty());

        sceneManager.Shutdown();
    }

    TEST(ResourceInstantiationValidation, PrefabInstanceRevertAllKeepsOverrideWhenMissingActorComponentClassUnavailable)
    {
        ComponentFactoryClassGuard componentFactoryGuard;
        ComponentFactory::RegisterComponentClass<PrefabPayloadComponent>(
            "PrefabPayloadComponent");

        PrefabEntityData data;
        data.name = "UnavailableMissingActorComponentRoot";
        data.actorComponentData.push_back(
            {"PrefabPayloadComponent", "ammo=12", "PrimaryPayload"});

        auto prefab = Prefab::CreateFromData({data});

        SceneManager sceneManager;
        sceneManager.Initialize();

        SceneEntity* instance = prefab->Instantiate(sceneManager);
        ASSERT_NE(nullptr, instance);
        auto* prefabInstance = instance->GetComponent<PrefabInstance>();
        ASSERT_NE(nullptr, prefabInstance);

        auto* actor = static_cast<Actor*>(instance);
        ASSERT_NE(nullptr, FindPrefabPayloadComponentByName(*actor, "PrimaryPayload"));
        EXPECT_TRUE(actor->RemoveComponent<PrefabPayloadComponent>());
        EXPECT_TRUE(FindPrefabPayloadComponentByName(*actor, "PrimaryPayload") == nullptr);

        ComponentFactory::ClearComponentClasses();
        prefabInstance->AddOverride(
            {"PrefabPayloadComponent",
             "serializedData",
             std::string("deleted"),
             "PrimaryPayload"});

        prefabInstance->RevertAll();

        EXPECT_TRUE(FindPrefabPayloadComponentByName(*actor, "PrimaryPayload") == nullptr);
        EXPECT_TRUE(prefabInstance->IsOverridden(
            "PrefabPayloadComponent", "serializedData", "PrimaryPayload"));
        EXPECT_EQ(static_cast<size_t>(1), prefabInstance->GetOverrides().size());

        sceneManager.Shutdown();
    }

    TEST(ResourceInstantiationValidation, PrefabInstanceRevertAllRestoresNamedDuplicateActorComponentPayloads)
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
        ASSERT_NE(nullptr, instance);
        auto* prefabInstance = instance->GetComponent<PrefabInstance>();
        ASSERT_NE(nullptr, prefabInstance);

        auto* primary = FindPrefabPayloadComponentByName(*static_cast<Actor*>(instance),
                                                         "PrimaryPayload");
        auto* secondary = FindPrefabPayloadComponentByName(*static_cast<Actor*>(instance),
                                                           "SecondaryPayload");
        ASSERT_NE(nullptr, primary);
        ASSERT_NE(nullptr, secondary);

        primary->SetName("TemporaryPayloadName");
        secondary->SetName("PrimaryPayload");
        primary->SetName("SecondaryPayload");
        primary->payload = "dirty-secondary";
        secondary->payload = "dirty-primary";
        // RevertAll must restore each named duplicate through runtime name binding
        // even when multiple components share the same class.
        prefabInstance->AddOverride({"PrefabPayloadComponent", "serializedData", std::string("dirty")});

        prefabInstance->RevertAll();

        EXPECT_EQ(std::string("slot=secondary"), primary->payload);
        EXPECT_EQ(std::string("slot=primary"), secondary->payload);
        EXPECT_TRUE(prefabInstance->GetOverrides().empty());

        sceneManager.Shutdown();
    }

    TEST(ResourceInstantiationValidation, PrefabInstanceRevertAllUsesRuntimeNameBindingAfterUniquify)
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
        ASSERT_NE(nullptr, instance);
        auto* prefabInstance = instance->GetComponent<PrefabInstance>();
        ASSERT_NE(nullptr, prefabInstance);

        auto* actor = static_cast<Actor*>(instance);
        auto* constructorComponent = FindPrefabPayloadComponentByName(*actor, "PrimaryPayload");
        auto* prefabComponent = FindPrefabPayloadComponentByName(*actor, "PrimaryPayload_1");
        ASSERT_NE(nullptr, constructorComponent);
        ASSERT_NE(nullptr, prefabComponent);
        EXPECT_EQ(std::string("constructor"), constructorComponent->payload);
        EXPECT_EQ(std::string("slot=prefab"), prefabComponent->payload);

        constructorComponent->payload = "constructor-dirty";
        prefabComponent->payload = "prefab-dirty";
        prefabInstance->AddOverride({"PrefabPayloadComponent", "serializedData", std::string("prefab-dirty")});

        prefabInstance->RevertAll();

        EXPECT_EQ(std::string("constructor-dirty"), constructorComponent->payload);
        EXPECT_EQ(std::string("slot=prefab"), prefabComponent->payload);
        EXPECT_TRUE(prefabInstance->GetOverrides().empty());

        sceneManager.Shutdown();
        ActorFactory::ClearAll();
    }

    TEST(ResourceInstantiationValidation, PrefabInstanceRevertAllRestoresChildEntityStateAndPayload)
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
        ASSERT_NE(nullptr, root);
        auto* prefabInstance = root->GetComponent<PrefabInstance>();
        ASSERT_NE(nullptr, prefabInstance);
        EXPECT_EQ(static_cast<size_t>(1), root->GetChildren().size());

        SceneEntity* child = root->GetChildren()[0];
        ASSERT_NE(nullptr, child);
        auto* childPayload = FindPrefabPayloadComponentByName(
            *static_cast<Actor*>(child), "ChildPayload");
        ASSERT_NE(nullptr, childPayload);

        child->SetPosition(Vec3(9.0f, 9.0f, 9.0f));
        childPayload->payload = "dirty-child";
        prefabInstance->AddOverride(
            {"SceneEntity", "position", child->GetPosition(), "", "Child"});
        prefabInstance->AddOverride(
            {"PrefabPayloadComponent", "serializedData", childPayload->payload, "ChildPayload", "Child"});

        prefabInstance->RevertAll();

        EXPECT_EQ(Vec3(1.0f, 2.0f, 3.0f), child->GetPosition());
        EXPECT_EQ(std::string("slot=child"), childPayload->payload);
        EXPECT_TRUE(prefabInstance->GetOverrides().empty());

        sceneManager.Shutdown();
    }

    TEST(ResourceInstantiationValidation, PrefabInstanceRevertAllRestoresRenamedChildEntityByRuntimeBinding)
    {
        PrefabEntityData rootData;
        rootData.name = "RuntimeBindingRoot";

        PrefabEntityData childData;
        childData.name = "Child";
        childData.parentIndex = 0;
        childData.position = Vec3(1.0f, 2.0f, 3.0f);

        auto prefab = Prefab::CreateFromData({rootData, childData});

        SceneManager sceneManager;
        sceneManager.Initialize();

        SceneEntity* root = prefab->Instantiate(sceneManager);
        ASSERT_NE(nullptr, root);

        auto* prefabInstance = root->GetComponent<PrefabInstance>();
        ASSERT_NE(nullptr, prefabInstance);
        EXPECT_EQ(static_cast<size_t>(1), root->GetChildren().size());

        SceneEntity* child = root->GetChildren()[0];
        child->SetName("RenamedChild");
        child->SetPosition(Vec3(9.0f, 9.0f, 9.0f));
        prefabInstance->AddOverride(
            {"SceneEntity", "position", child->GetPosition(), "", "Child"});

        prefabInstance->RevertAll();

        EXPECT_EQ(static_cast<size_t>(1), root->GetChildren().size());
        EXPECT_EQ(child, root->GetChildren()[0]);
        EXPECT_EQ(std::string("Child"), child->GetName());
        EXPECT_EQ(Vec3(1.0f, 2.0f, 3.0f), child->GetPosition());
        EXPECT_TRUE(prefabInstance->GetOverrides().empty());

        sceneManager.Shutdown();
    }

    TEST(ResourceInstantiationValidation, PrefabInstanceRevertAllRecreatesMissingChildActorComponent)
    {
        ComponentFactoryClassGuard componentFactoryGuard;
        ComponentFactory::RegisterComponentClass<PrefabPayloadComponent>(
            "PrefabPayloadComponent");

        PrefabEntityData rootData;
        rootData.name = "RecreateMissingChildComponentRoot";

        PrefabEntityData childData;
        childData.name = "Child";
        childData.parentIndex = 0;
        childData.actorComponentData.push_back(
            {"PrefabPayloadComponent", "slot=child", "ChildPayload"});

        auto prefab = Prefab::CreateFromData({rootData, childData});

        SceneManager sceneManager;
        sceneManager.Initialize();

        SceneEntity* root = prefab->Instantiate(sceneManager);
        ASSERT_NE(nullptr, root);
        auto* prefabInstance = root->GetComponent<PrefabInstance>();
        ASSERT_NE(nullptr, prefabInstance);
        EXPECT_EQ(static_cast<size_t>(1), root->GetChildren().size());

        SceneEntity* child = root->GetChildren()[0];
        auto* childActor = static_cast<Actor*>(child);
        ASSERT_NE(nullptr, FindPrefabPayloadComponentByName(*childActor, "ChildPayload"));
        EXPECT_TRUE(childActor->RemoveComponent<PrefabPayloadComponent>());
        EXPECT_TRUE(FindPrefabPayloadComponentByName(*childActor, "ChildPayload") == nullptr);

        prefabInstance->AddOverride(
            {"PrefabPayloadComponent",
             "serializedData",
             std::string("deleted"),
             "ChildPayload",
             "Child"});

        prefabInstance->RevertAll();

        auto* restored = FindPrefabPayloadComponentByName(*childActor, "ChildPayload");
        ASSERT_NE(nullptr, restored);
        EXPECT_EQ(std::string("slot=child"), restored->payload);
        EXPECT_TRUE(prefabInstance->GetOverrides().empty());

        sceneManager.Shutdown();
    }

    TEST(ResourceInstantiationValidation, PrefabInstanceRevertAllRecreatesMissingChildEntity)
    {
        ComponentFactoryClassGuard componentFactoryGuard;
        ComponentFactory::RegisterComponentClass<PrefabPayloadComponent>(
            "PrefabPayloadComponent");

        PrefabEntityData rootData;
        rootData.name = "RecreateChildRoot";

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
        ASSERT_NE(nullptr, root);
        auto* prefabInstance = root->GetComponent<PrefabInstance>();
        ASSERT_NE(nullptr, prefabInstance);
        EXPECT_EQ(static_cast<size_t>(1), root->GetChildren().size());

        SceneEntity* child = root->GetChildren()[0];
        sceneManager.DestroyEntity(child->GetHandle());
        EXPECT_TRUE(root->GetChildren().empty());

        prefabInstance->AddOverride(
            {"SceneEntity", "position", Vec3(9.0f, 9.0f, 9.0f), "", "Child"});

        prefabInstance->RevertAll();

        EXPECT_EQ(static_cast<size_t>(1), root->GetChildren().size());
        SceneEntity* recreated = root->GetChildren()[0];
        ASSERT_NE(nullptr, recreated);
        EXPECT_EQ(std::string("Child"), recreated->GetName());
        EXPECT_EQ(root, recreated->GetParent());
        EXPECT_EQ(Vec3(1.0f, 2.0f, 3.0f), recreated->GetPosition());

        auto* childPayload = FindPrefabPayloadComponentByName(
            *static_cast<Actor*>(recreated), "ChildPayload");
        ASSERT_NE(nullptr, childPayload);
        EXPECT_EQ(std::string("slot=child"), childPayload->payload);
        EXPECT_TRUE(prefabInstance->GetOverrides().empty());

        sceneManager.Shutdown();
    }

    TEST(ResourceInstantiationValidation, PrefabInstanceRevertAllRecreatesNestedMissingDescendant)
    {
        PrefabEntityData rootData;
        rootData.name = "RecreateNestedRoot";

        PrefabEntityData branchData;
        branchData.name = "Branch";
        branchData.parentIndex = 0;

        PrefabEntityData leafData;
        leafData.name = "Leaf";
        leafData.parentIndex = 1;
        leafData.scale = Vec3(2.0f, 3.0f, 4.0f);

        auto prefab = Prefab::CreateFromData({rootData, branchData, leafData});

        SceneManager sceneManager;
        sceneManager.Initialize();

        SceneEntity* root = prefab->Instantiate(sceneManager);
        ASSERT_NE(nullptr, root);
        auto* prefabInstance = root->GetComponent<PrefabInstance>();
        ASSERT_NE(nullptr, prefabInstance);
        EXPECT_EQ(static_cast<size_t>(1), root->GetChildren().size());
        SceneEntity* branch = root->GetChildren()[0];
        EXPECT_EQ(static_cast<size_t>(1), branch->GetChildren().size());

        SceneEntity* leaf = branch->GetChildren()[0];
        sceneManager.DestroyEntity(leaf->GetHandle());
        EXPECT_TRUE(branch->GetChildren().empty());

        prefabInstance->RevertAll();

        EXPECT_EQ(static_cast<size_t>(1), branch->GetChildren().size());
        SceneEntity* recreatedLeaf = branch->GetChildren()[0];
        ASSERT_NE(nullptr, recreatedLeaf);
        EXPECT_EQ(std::string("Leaf"), recreatedLeaf->GetName());
        EXPECT_EQ(branch, recreatedLeaf->GetParent());
        EXPECT_EQ(Vec3(2.0f, 3.0f, 4.0f), recreatedLeaf->GetScale());

        sceneManager.Shutdown();
    }

    TEST(ResourceInstantiationValidation, PrefabInstanceRevertAllPrunesExtraLiveChildEntity)
    {
        PrefabEntityData rootData;
        rootData.name = "PruneExtraRoot";

        auto prefab = Prefab::CreateFromData({rootData});

        SceneManager sceneManager;
        sceneManager.Initialize();

        SceneEntity* root = prefab->Instantiate(sceneManager);
        ASSERT_NE(nullptr, root);
        auto* prefabInstance = root->GetComponent<PrefabInstance>();
        ASSERT_NE(nullptr, prefabInstance);

        ActorSpawnParams extraParams;
        extraParams.name = "ExtraChild";
        extraParams.parent = root;
        SceneEntity* extra = sceneManager.SpawnActor(extraParams);
        ASSERT_NE(nullptr, extra);
        const SceneEntity::Handle extraHandle = extra->GetHandle();

        prefabInstance->RevertAll();

        EXPECT_TRUE(root->GetChildren().empty());
        EXPECT_TRUE(sceneManager.GetEntity(extraHandle) == nullptr);

        sceneManager.Shutdown();
    }

    TEST(ResourceInstantiationValidation, PrefabInstanceRevertAllPrunesNestedExtraLiveChildSubtree)
    {
        PrefabEntityData rootData;
        rootData.name = "PruneNestedExtraRoot";

        auto prefab = Prefab::CreateFromData({rootData});

        SceneManager sceneManager;
        sceneManager.Initialize();

        SceneEntity* root = prefab->Instantiate(sceneManager);
        ASSERT_NE(nullptr, root);
        auto* prefabInstance = root->GetComponent<PrefabInstance>();
        ASSERT_NE(nullptr, prefabInstance);

        ActorSpawnParams branchParams;
        branchParams.name = "ExtraBranch";
        branchParams.parent = root;
        SceneEntity* branch = sceneManager.SpawnActor(branchParams);
        ASSERT_NE(nullptr, branch);

        ActorSpawnParams leafParams;
        leafParams.name = "ExtraLeaf";
        leafParams.parent = branch;
        SceneEntity* leaf = sceneManager.SpawnActor(leafParams);
        ASSERT_NE(nullptr, leaf);

        const SceneEntity::Handle branchHandle = branch->GetHandle();
        const SceneEntity::Handle leafHandle = leaf->GetHandle();

        prefabInstance->RevertAll();

        EXPECT_TRUE(root->GetChildren().empty());
        EXPECT_TRUE(sceneManager.GetEntity(branchHandle) == nullptr);
        EXPECT_TRUE(sceneManager.GetEntity(leafHandle) == nullptr);

        sceneManager.Shutdown();
    }

    TEST(ResourceInstantiationValidation, PrefabInstanceRevertAllPrunesDuplicateExtraChildAfterBoundRestore)
    {
        PrefabEntityData rootData;
        rootData.name = "DuplicateExtraRestoreRoot";

        PrefabEntityData childData;
        childData.name = "Child";
        childData.parentIndex = 0;

        auto prefab = Prefab::CreateFromData({rootData, childData});

        SceneManager sceneManager;
        sceneManager.Initialize();

        SceneEntity* root = prefab->Instantiate(sceneManager);
        ASSERT_NE(nullptr, root);
        auto* prefabInstance = root->GetComponent<PrefabInstance>();
        ASSERT_NE(nullptr, prefabInstance);
        EXPECT_EQ(static_cast<size_t>(1), root->GetChildren().size());

        ActorSpawnParams duplicateParams;
        duplicateParams.name = "Child";
        duplicateParams.parent = root;
        SceneEntity* duplicate = sceneManager.SpawnActor(duplicateParams);
        ASSERT_NE(nullptr, duplicate);
        const SceneEntity::Handle duplicateHandle = duplicate->GetHandle();
        EXPECT_EQ(static_cast<size_t>(2), root->GetChildren().size());

        prefabInstance->RevertAll();

        EXPECT_EQ(static_cast<size_t>(1), root->GetChildren().size());
        EXPECT_TRUE(sceneManager.GetEntity(duplicateHandle) == nullptr);
        EXPECT_EQ(std::string("Child"), root->GetChildren()[0]->GetName());

        sceneManager.Shutdown();
    }

    TEST(ResourceInstantiationValidation, PrefabInstanceRevertAllRestoresReparentedBoundChildAndPrunesExtraParent)
    {
        PrefabEntityData rootData;
        rootData.name = "ReparentRoot";

        PrefabEntityData childData;
        childData.name = "Child";
        childData.parentIndex = 0;
        childData.position = Vec3(1.0f, 2.0f, 3.0f);

        auto prefab = Prefab::CreateFromData({rootData, childData});

        SceneManager sceneManager;
        sceneManager.Initialize();

        SceneEntity* root = prefab->Instantiate(sceneManager);
        ASSERT_NE(nullptr, root);
        auto* prefabInstance = root->GetComponent<PrefabInstance>();
        ASSERT_NE(nullptr, prefabInstance);
        EXPECT_EQ(static_cast<size_t>(1), root->GetChildren().size());

        SceneEntity* child = root->GetChildren()[0];
        const SceneEntity::Handle childHandle = child->GetHandle();

        ActorSpawnParams extraParams;
        extraParams.name = "ExtraParent";
        extraParams.parent = root;
        SceneEntity* extraParent = sceneManager.SpawnActor(extraParams);
        ASSERT_NE(nullptr, extraParent);
        const SceneEntity::Handle extraParentHandle = extraParent->GetHandle();

        child->SetParent(extraParent);
        child->SetPosition(Vec3(9.0f, 9.0f, 9.0f));
        prefabInstance->AddOverride(
            {"SceneEntity", "position", child->GetPosition(), "", "Child"});

        prefabInstance->RevertAll();

        SceneEntity* restoredChild = sceneManager.GetEntity(childHandle);
        ASSERT_NE(nullptr, restoredChild);
        EXPECT_EQ(root, restoredChild->GetParent());
        EXPECT_EQ(static_cast<size_t>(1), root->GetChildren().size());
        EXPECT_EQ(restoredChild, root->GetChildren()[0]);
        EXPECT_TRUE(sceneManager.GetEntity(extraParentHandle) == nullptr);
        EXPECT_EQ(Vec3(1.0f, 2.0f, 3.0f), restoredChild->GetPosition());
        EXPECT_TRUE(prefabInstance->GetOverrides().empty());

        sceneManager.Shutdown();
    }

    TEST(ResourceInstantiationValidation, PrefabInstanceRevertAllRestoresReparentedNestedBoundChild)
    {
        PrefabEntityData rootData;
        rootData.name = "NestedReparentRoot";

        PrefabEntityData branchData;
        branchData.name = "Branch";
        branchData.parentIndex = 0;

        PrefabEntityData leafData;
        leafData.name = "Leaf";
        leafData.parentIndex = 1;
        leafData.scale = Vec3(2.0f, 3.0f, 4.0f);

        auto prefab = Prefab::CreateFromData({rootData, branchData, leafData});

        SceneManager sceneManager;
        sceneManager.Initialize();

        SceneEntity* root = prefab->Instantiate(sceneManager);
        ASSERT_NE(nullptr, root);
        auto* prefabInstance = root->GetComponent<PrefabInstance>();
        ASSERT_NE(nullptr, prefabInstance);
        EXPECT_EQ(static_cast<size_t>(1), root->GetChildren().size());

        SceneEntity* branch = root->GetChildren()[0];
        EXPECT_EQ(static_cast<size_t>(1), branch->GetChildren().size());
        SceneEntity* leaf = branch->GetChildren()[0];
        const SceneEntity::Handle leafHandle = leaf->GetHandle();

        ActorSpawnParams extraParams;
        extraParams.name = "ExtraNestedParent";
        extraParams.parent = root;
        SceneEntity* extraParent = sceneManager.SpawnActor(extraParams);
        ASSERT_NE(nullptr, extraParent);
        const SceneEntity::Handle extraParentHandle = extraParent->GetHandle();

        leaf->SetParent(extraParent);
        leaf->SetScale(Vec3(9.0f, 9.0f, 9.0f));

        prefabInstance->RevertAll();

        SceneEntity* restoredLeaf = sceneManager.GetEntity(leafHandle);
        ASSERT_NE(nullptr, restoredLeaf);
        EXPECT_EQ(branch, restoredLeaf->GetParent());
        EXPECT_EQ(static_cast<size_t>(1), branch->GetChildren().size());
        EXPECT_EQ(restoredLeaf, branch->GetChildren()[0]);
        EXPECT_TRUE(sceneManager.GetEntity(extraParentHandle) == nullptr);
        EXPECT_EQ(Vec3(2.0f, 3.0f, 4.0f), restoredLeaf->GetScale());

        sceneManager.Shutdown();
    }

    TEST(ResourceInstantiationValidation, PrefabRestoreHierarchySkipsPruningWhenEntityBindingMissingAndPathAmbiguous)
    {
        PrefabEntityData rootData;
        rootData.name = "MissingBindingAmbiguousRoot";

        PrefabEntityData childData;
        childData.name = "Child";
        childData.parentIndex = 0;

        auto prefab = Prefab::CreateFromData({rootData, childData});

        SceneManager sceneManager;
        sceneManager.Initialize();

        SceneEntity* root = prefab->Instantiate(sceneManager);
        ASSERT_NE(nullptr, root);
        EXPECT_EQ(static_cast<size_t>(1), root->GetChildren().size());

        ActorSpawnParams duplicateParams;
        duplicateParams.name = "Child";
        duplicateParams.parent = root;
        SceneEntity* duplicate = sceneManager.SpawnActor(duplicateParams);
        ASSERT_NE(nullptr, duplicate);
        const SceneEntity::Handle duplicateHandle = duplicate->GetHandle();
        EXPECT_EQ(static_cast<size_t>(2), root->GetChildren().size());

        std::vector<PrefabActorComponentNameBinding> nameBindings;
        std::vector<PrefabEntityRuntimeBinding> entityBindings;
        EXPECT_TRUE(prefab->RestoreHierarchyStateTo(*root, nameBindings, entityBindings));

        EXPECT_EQ(static_cast<size_t>(2), root->GetChildren().size());
        ASSERT_NE(nullptr, sceneManager.GetEntity(duplicateHandle));

        sceneManager.Shutdown();
    }

    TEST(ResourceInstantiationValidation, PrefabInstanceRevertAllKeepsExistingNameBindingsWhenRecreatingChild)
    {
        ActorFactory::ClearAll();
        ActorFactory::Register<PrefabNameCollisionActor>("PrefabNameCollisionActor");

        ComponentFactoryClassGuard componentFactoryGuard;
        ComponentFactory::RegisterComponentClass<PrefabPayloadComponent>(
            "PrefabPayloadComponent");

        PrefabEntityData rootData;
        rootData.name = "BindingRestoreRoot";
        rootData.actorClassName = "PrefabNameCollisionActor";
        rootData.actorComponentData.push_back(
            {"PrefabPayloadComponent", "slot=root-prefab", "PrimaryPayload"});

        PrefabEntityData childData;
        childData.name = "Child";
        childData.parentIndex = 0;
        childData.actorComponentData.push_back(
            {"PrefabPayloadComponent", "slot=child", "ChildPayload"});

        auto prefab = Prefab::CreateFromData({rootData, childData});

        SceneManager sceneManager;
        sceneManager.Initialize();

        SceneEntity* root = prefab->Instantiate(sceneManager);
        ASSERT_NE(nullptr, root);
        auto* prefabInstance = root->GetComponent<PrefabInstance>();
        ASSERT_NE(nullptr, prefabInstance);

        auto* rootPrefabPayload = FindPrefabPayloadComponentByName(
            *static_cast<Actor*>(root), "PrimaryPayload_1");
        ASSERT_NE(nullptr, rootPrefabPayload);
        rootPrefabPayload->payload = "dirty-root";

        EXPECT_EQ(static_cast<size_t>(1), root->GetChildren().size());
        SceneEntity* child = root->GetChildren()[0];
        sceneManager.DestroyEntity(child->GetHandle());
        EXPECT_TRUE(root->GetChildren().empty());

        prefabInstance->RevertAll();

        EXPECT_EQ(std::string("slot=root-prefab"), rootPrefabPayload->payload);

        rootPrefabPayload->payload = "dirty-root-again";
        prefabInstance->RevertProperty(
            "PrefabPayloadComponent", "serializedData", "PrimaryPayload_1");

        EXPECT_EQ(std::string("slot=root-prefab"), rootPrefabPayload->payload);
        EXPECT_EQ(static_cast<size_t>(1), root->GetChildren().size());

        sceneManager.Shutdown();
        ActorFactory::ClearAll();
    }

    TEST(ResourceInstantiationValidation, PrefabInstanceRevertAllDropsStaleBindingsForRecreatedChild)
    {
        ActorFactory::ClearAll();
        ActorFactory::Register("PrefabRecreateBindingActor", []() {
            auto actor = std::make_unique<SceneEntity>("PrefabRecreateBindingActorDefault");
            auto* component = actor->AddComponent<PrefabPayloadComponent>();
            component->SetName("ChildPayload");
            component->payload = "constructor";
            return actor;
        });

        ComponentFactoryClassGuard componentFactoryGuard;
        ComponentFactory::RegisterComponentClass<PrefabPayloadComponent>(
            "PrefabPayloadComponent");

        PrefabEntityData rootData;
        rootData.name = "DropStaleBindingRoot";

        PrefabEntityData childData;
        childData.name = "Child";
        childData.parentIndex = 0;
        childData.actorClassName = "PrefabRecreateBindingActor";
        childData.actorComponentData.push_back(
            {"PrefabPayloadComponent", "slot=child", "ChildPayload"});

        auto prefab = Prefab::CreateFromData({rootData, childData});

        SceneManager sceneManager;
        sceneManager.Initialize();

        SceneEntity* root = prefab->Instantiate(sceneManager);
        ASSERT_NE(nullptr, root);
        auto* prefabInstance = root->GetComponent<PrefabInstance>();
        ASSERT_NE(nullptr, prefabInstance);
        EXPECT_EQ(static_cast<size_t>(1), root->GetChildren().size());

        SceneEntity* child = root->GetChildren()[0];
        auto* initialPrefabPayload = FindPrefabPayloadComponentByName(
            *static_cast<Actor*>(child), "ChildPayload_1");
        ASSERT_NE(nullptr, initialPrefabPayload);

        sceneManager.DestroyEntity(child->GetHandle());
        EXPECT_TRUE(root->GetChildren().empty());

        ActorFactory::Register("PrefabRecreateBindingActor", []() {
            return std::make_unique<SceneEntity>("PrefabRecreateBindingActorDefault");
        });

        prefabInstance->RevertAll();

        EXPECT_EQ(static_cast<size_t>(1), root->GetChildren().size());
        SceneEntity* recreated = root->GetChildren()[0];
        auto* recreatedPrefabPayload = FindPrefabPayloadComponentByName(
            *static_cast<Actor*>(recreated), "ChildPayload");
        ASSERT_NE(nullptr, recreatedPrefabPayload);

        auto* extraPayload = static_cast<Actor*>(recreated)->AddComponent<PrefabPayloadComponent>();
        ASSERT_NE(nullptr, extraPayload);
        extraPayload->SetName("ChildPayload_1");
        extraPayload->payload = "extra-dirty";

        prefabInstance->AddOverride(
            {"PrefabPayloadComponent",
             "serializedData",
             std::string("extra-dirty"),
             "ChildPayload_1",
             "Child"});

        prefabInstance->RevertProperty(
            "PrefabPayloadComponent", "serializedData", "ChildPayload_1", "Child");

        EXPECT_EQ(std::string("extra-dirty"), extraPayload->payload);
        EXPECT_TRUE(prefabInstance->IsOverridden(
            "PrefabPayloadComponent", "serializedData", "ChildPayload_1", "Child"));

        sceneManager.Shutdown();
        ActorFactory::ClearAll();
    }

    TEST(ResourceInstantiationValidation, PrefabInstanceRevertAllRestoresLegacyComponentPayload)
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
        ASSERT_NE(nullptr, instance);
        auto* prefabInstance = instance->GetComponent<PrefabInstance>();
        ASSERT_NE(nullptr, prefabInstance);

        auto* component = instance->GetComponent<PrefabLegacyPayloadComponent>();
        ASSERT_NE(nullptr, component);
        component->payload = "legacy=disabled";
        prefabInstance->AddOverride({"PrefabLegacyPayloadComponent", "serializedData", component->payload});

        prefabInstance->RevertAll();

        EXPECT_EQ(std::string("legacy=enabled"), component->payload);
        EXPECT_TRUE(prefabInstance->GetOverrides().empty());

        sceneManager.Shutdown();
    }

    TEST(ResourceInstantiationValidation, PrefabInstanceRevertAllRecreatesMissingLegacyComponent)
    {
        ComponentFactoryClassGuard componentFactoryGuard;
        ComponentFactory::RegisterComponentClass<PrefabLegacyPayloadComponent>(
            "PrefabLegacyPayloadComponent");

        PrefabEntityData data;
        data.name = "RecreateMissingLegacyComponentRoot";
        data.componentData["PrefabLegacyPayloadComponent"] = "legacy=enabled";

        auto prefab = Prefab::CreateFromData({data});

        SceneManager sceneManager;
        sceneManager.Initialize();

        SceneEntity* instance = prefab->Instantiate(sceneManager);
        ASSERT_NE(nullptr, instance);
        auto* prefabInstance = instance->GetComponent<PrefabInstance>();
        ASSERT_NE(nullptr, prefabInstance);

        ASSERT_NE(nullptr, instance->GetComponent<PrefabLegacyPayloadComponent>());
        EXPECT_TRUE(instance->RemoveComponent<PrefabLegacyPayloadComponent>());
        EXPECT_TRUE(instance->GetComponent<PrefabLegacyPayloadComponent>() == nullptr);

        prefabInstance->AddOverride(
            {"PrefabLegacyPayloadComponent", "serializedData", std::string("deleted")});

        prefabInstance->RevertAll();

        auto* restored = instance->GetComponent<PrefabLegacyPayloadComponent>();
        ASSERT_NE(nullptr, restored);
        EXPECT_EQ(std::string("legacy=enabled"), restored->payload);
        EXPECT_TRUE(prefabInstance->GetOverrides().empty());

        sceneManager.Shutdown();
    }

    TEST(ResourceInstantiationValidation, PrefabInstanceRevertPropertyRestoresOnlyRequestedEntityProperty)
    {
        PrefabEntityData data;
        data.name = "RevertPropertyRoot";
        data.position = Vec3(1.0f, 2.0f, 3.0f);
        data.scale = Vec3(2.0f, 2.0f, 2.0f);

        auto prefab = Prefab::CreateFromData({data});

        SceneManager sceneManager;
        sceneManager.Initialize();

        SceneEntity* instance = prefab->Instantiate(sceneManager);
        ASSERT_NE(nullptr, instance);
        auto* prefabInstance = instance->GetComponent<PrefabInstance>();
        ASSERT_NE(nullptr, prefabInstance);

        instance->SetPosition(Vec3(9.0f, 9.0f, 9.0f));
        instance->SetScale(Vec3(8.0f, 8.0f, 8.0f));
        prefabInstance->AddOverride({"SceneEntity", "position", Vec3(9.0f, 9.0f, 9.0f)});
        prefabInstance->AddOverride({"SceneEntity", "scale", Vec3(8.0f, 8.0f, 8.0f)});

        prefabInstance->RevertProperty("SceneEntity", "position");

        EXPECT_EQ(Vec3(1.0f, 2.0f, 3.0f), instance->GetPosition());
        EXPECT_EQ(Vec3(8.0f, 8.0f, 8.0f), instance->GetScale());
        EXPECT_FALSE(prefabInstance->IsOverridden("SceneEntity", "position"));
        EXPECT_TRUE(prefabInstance->IsOverridden("SceneEntity", "scale"));
        EXPECT_EQ(static_cast<size_t>(1), prefabInstance->GetOverrides().size());

        sceneManager.Shutdown();
    }

    TEST(ResourceInstantiationValidation, PrefabInstanceRevertPropertyRestoresActorComponentPayload)
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
        ASSERT_NE(nullptr, instance);
        auto* prefabInstance = instance->GetComponent<PrefabInstance>();
        ASSERT_NE(nullptr, prefabInstance);

        auto* actorComponent = static_cast<Actor*>(instance)->GetComponent<PrefabPayloadComponent>();
        ASSERT_NE(nullptr, actorComponent);
        auto* legacyComponent = instance->GetComponent<PrefabLegacyPayloadComponent>();
        ASSERT_NE(nullptr, legacyComponent);

        actorComponent->payload = "ammo=1;mode=single";
        legacyComponent->payload = "legacy=disabled";
        prefabInstance->AddOverride({"PrefabPayloadComponent", "serializedData", actorComponent->payload});
        prefabInstance->AddOverride({"PrefabLegacyPayloadComponent", "serializedData", legacyComponent->payload});

        prefabInstance->RevertProperty("PrefabPayloadComponent", "serializedData");

        EXPECT_EQ(std::string("ammo=12;mode=burst"), actorComponent->payload);
        EXPECT_EQ(std::string("legacy=disabled"), legacyComponent->payload);
        EXPECT_FALSE(prefabInstance->IsOverridden("PrefabPayloadComponent", "serializedData"));
        EXPECT_TRUE(prefabInstance->IsOverridden("PrefabLegacyPayloadComponent", "serializedData"));
        EXPECT_EQ(static_cast<size_t>(1), prefabInstance->GetOverrides().size());

        sceneManager.Shutdown();
    }

    TEST(ResourceInstantiationValidation, PrefabInstanceRootEntityPathNormalizesForOverrideIdentity)
    {
        PrefabInstance prefabInstance;
        prefabInstance.AddOverride({"SceneEntity", "position", Vec3(1.0f), "", "."});

        EXPECT_TRUE(prefabInstance.IsOverridden("SceneEntity", "position"));
        EXPECT_TRUE(prefabInstance.IsOverridden("SceneEntity", "position", "", "."));

        prefabInstance.RemoveOverride("SceneEntity", "position");

        EXPECT_FALSE(prefabInstance.IsOverridden("SceneEntity", "position", "", "."));
        EXPECT_TRUE(prefabInstance.GetOverrides().empty());
    }

    TEST(ResourceInstantiationValidation, PrefabInstanceRevertPropertyRestoresChildEntityProperty)
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
        ASSERT_NE(nullptr, root);
        auto* prefabInstance = root->GetComponent<PrefabInstance>();
        ASSERT_NE(nullptr, prefabInstance);
        EXPECT_EQ(static_cast<size_t>(1), root->GetChildren().size());
        SceneEntity* child = root->GetChildren()[0];

        root->SetPosition(Vec3(7.0f, 0.0f, 0.0f));
        child->SetPosition(Vec3(0.0f, 9.0f, 0.0f));
        prefabInstance->AddOverride({"SceneEntity", "position", root->GetPosition()});
        prefabInstance->AddOverride(
            {"SceneEntity", "position", child->GetPosition(), "", "Child"});

        prefabInstance->RevertProperty("SceneEntity", "position", "", "Child");

        EXPECT_EQ(Vec3(7.0f, 0.0f, 0.0f), root->GetPosition());
        EXPECT_EQ(Vec3(0.0f, 2.0f, 0.0f), child->GetPosition());
        EXPECT_TRUE(prefabInstance->IsOverridden("SceneEntity", "position"));
        EXPECT_FALSE(prefabInstance->IsOverridden("SceneEntity", "position", "", "Child"));

        sceneManager.Shutdown();
    }

    TEST(ResourceInstantiationValidation, PrefabInstanceRevertPropertyRestoresRenamedChildEntityByRuntimeBinding)
    {
        PrefabEntityData rootData;
        rootData.name = "RuntimeBindingPropertyRoot";

        PrefabEntityData childData;
        childData.name = "Child";
        childData.parentIndex = 0;
        childData.position = Vec3(2.0f, 3.0f, 4.0f);

        auto prefab = Prefab::CreateFromData({rootData, childData});

        SceneManager sceneManager;
        sceneManager.Initialize();

        SceneEntity* root = prefab->Instantiate(sceneManager);
        ASSERT_NE(nullptr, root);

        auto* prefabInstance = root->GetComponent<PrefabInstance>();
        ASSERT_NE(nullptr, prefabInstance);
        EXPECT_EQ(static_cast<size_t>(1), root->GetChildren().size());

        SceneEntity* child = root->GetChildren()[0];
        child->SetName("RenamedChild");
        child->SetPosition(Vec3(8.0f, 8.0f, 8.0f));
        prefabInstance->AddOverride(
            {"SceneEntity", "position", child->GetPosition(), "", "Child"});

        prefabInstance->RevertProperty("SceneEntity", "position", "", "Child");

        EXPECT_EQ(static_cast<size_t>(1), root->GetChildren().size());
        EXPECT_EQ(child, root->GetChildren()[0]);
        EXPECT_EQ(std::string("RenamedChild"), child->GetName());
        EXPECT_EQ(Vec3(2.0f, 3.0f, 4.0f), child->GetPosition());
        EXPECT_FALSE(prefabInstance->IsOverridden(
            "SceneEntity", "position", "", "Child"));

        sceneManager.Shutdown();
    }

    TEST(ResourceInstantiationValidation, PrefabInstanceRevertPropertyUsesSiblingOrdinalEntityPath)
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
        ASSERT_NE(nullptr, root);
        auto* prefabInstance = root->GetComponent<PrefabInstance>();
        ASSERT_NE(nullptr, prefabInstance);
        EXPECT_EQ(static_cast<size_t>(2), root->GetChildren().size());

        SceneEntity* firstChild = root->GetChildren()[0];
        SceneEntity* secondChild = root->GetChildren()[1];
        firstChild->SetPosition(Vec3(9.0f, 0.0f, 0.0f));
        secondChild->SetPosition(Vec3(8.0f, 0.0f, 0.0f));
        prefabInstance->AddOverride(
            {"SceneEntity", "position", secondChild->GetPosition(), "", "Child[1]"});

        prefabInstance->RevertProperty("SceneEntity", "position", "", "Child[1]");

        EXPECT_EQ(Vec3(9.0f, 0.0f, 0.0f), firstChild->GetPosition());
        EXPECT_EQ(Vec3(2.0f, 0.0f, 0.0f), secondChild->GetPosition());
        EXPECT_FALSE(prefabInstance->IsOverridden("SceneEntity", "position", "", "Child[1]"));

        sceneManager.Shutdown();
    }

    TEST(ResourceInstantiationValidation, PrefabInstanceRevertPropertyKeepsAmbiguousDuplicateSiblingPath)
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
        ASSERT_NE(nullptr, root);
        auto* prefabInstance = root->GetComponent<PrefabInstance>();
        ASSERT_NE(nullptr, prefabInstance);
        EXPECT_EQ(static_cast<size_t>(2), root->GetChildren().size());

        SceneEntity* firstChild = root->GetChildren()[0];
        SceneEntity* secondChild = root->GetChildren()[1];
        firstChild->SetPosition(Vec3(9.0f, 0.0f, 0.0f));
        secondChild->SetPosition(Vec3(8.0f, 0.0f, 0.0f));
        prefabInstance->AddOverride(
            {"SceneEntity", "position", secondChild->GetPosition(), "", "Child"});

        prefabInstance->RevertProperty("SceneEntity", "position", "", "Child");

        EXPECT_EQ(Vec3(9.0f, 0.0f, 0.0f), firstChild->GetPosition());
        EXPECT_EQ(Vec3(8.0f, 0.0f, 0.0f), secondChild->GetPosition());
        EXPECT_TRUE(prefabInstance->IsOverridden("SceneEntity", "position", "", "Child"));

        sceneManager.Shutdown();
    }

    TEST(ResourceInstantiationValidation, PrefabInstanceRevertPropertyRestoresNamedDuplicateActorComponentPayload)
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
        ASSERT_NE(nullptr, instance);
        auto* prefabInstance = instance->GetComponent<PrefabInstance>();
        ASSERT_NE(nullptr, prefabInstance);

        auto* primary = FindPrefabPayloadComponentByName(*static_cast<Actor*>(instance),
                                                         "PrimaryPayload");
        auto* secondary = FindPrefabPayloadComponentByName(*static_cast<Actor*>(instance),
                                                           "SecondaryPayload");
        ASSERT_NE(nullptr, primary);
        ASSERT_NE(nullptr, secondary);

        primary->payload = "dirty-primary";
        secondary->payload = "dirty-secondary";
        prefabInstance->AddOverride(
            {"PrefabPayloadComponent", "serializedData", primary->payload, "PrimaryPayload"});
        prefabInstance->AddOverride(
            {"PrefabPayloadComponent", "serializedData", secondary->payload, "SecondaryPayload"});

        prefabInstance->RevertProperty("PrefabPayloadComponent", "serializedData", "SecondaryPayload");

        EXPECT_EQ(std::string("dirty-primary"), primary->payload);
        EXPECT_EQ(std::string("slot=secondary"), secondary->payload);
        EXPECT_TRUE(
            prefabInstance->IsOverridden("PrefabPayloadComponent", "serializedData", "PrimaryPayload"));
        EXPECT_FALSE(
            prefabInstance->IsOverridden("PrefabPayloadComponent", "serializedData", "SecondaryPayload"));
        EXPECT_EQ(static_cast<size_t>(1), prefabInstance->GetOverrides().size());

        sceneManager.Shutdown();
    }

    TEST(ResourceInstantiationValidation, PrefabInstanceRemoveOverrideTargetsComponentName)
    {
        PrefabInstance prefabInstance;
        prefabInstance.AddOverride(
            {"PrefabPayloadComponent", "serializedData", std::string("primary"), "PrimaryPayload"});
        prefabInstance.AddOverride(
            {"PrefabPayloadComponent", "serializedData", std::string("secondary"), "SecondaryPayload"});

        prefabInstance.RemoveOverride("PrefabPayloadComponent", "serializedData", "SecondaryPayload");

        EXPECT_TRUE(
            prefabInstance.IsOverridden("PrefabPayloadComponent", "serializedData", "PrimaryPayload"));
        EXPECT_FALSE(
            prefabInstance.IsOverridden("PrefabPayloadComponent", "serializedData", "SecondaryPayload"));
        EXPECT_EQ(static_cast<size_t>(1), prefabInstance.GetOverrides().size());
    }

    TEST(ResourceInstantiationValidation, PrefabInstanceRevertPropertyUsesRuntimeNameBindingAfterUniquify)
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
        ASSERT_NE(nullptr, instance);
        auto* prefabInstance = instance->GetComponent<PrefabInstance>();
        ASSERT_NE(nullptr, prefabInstance);

        auto* actor = static_cast<Actor*>(instance);
        auto* constructorComponent = FindPrefabPayloadComponentByName(*actor, "PrimaryPayload");
        auto* prefabComponent = FindPrefabPayloadComponentByName(*actor, "PrimaryPayload_1");
        ASSERT_NE(nullptr, constructorComponent);
        ASSERT_NE(nullptr, prefabComponent);

        constructorComponent->payload = "constructor-dirty";
        prefabComponent->payload = "prefab-dirty";
        prefabInstance->AddOverride(
            {"PrefabPayloadComponent", "serializedData", prefabComponent->payload, "PrimaryPayload_1"});

        prefabInstance->RevertProperty("PrefabPayloadComponent", "serializedData", "PrimaryPayload_1");

        EXPECT_EQ(std::string("constructor-dirty"), constructorComponent->payload);
        EXPECT_EQ(std::string("slot=prefab"), prefabComponent->payload);
        EXPECT_FALSE(
            prefabInstance->IsOverridden("PrefabPayloadComponent", "serializedData", "PrimaryPayload_1"));

        sceneManager.Shutdown();
        ActorFactory::ClearAll();
    }

    TEST(ResourceInstantiationValidation, PrefabInstanceRevertPropertyRestoresChildNamedComponentPayload)
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
        ASSERT_NE(nullptr, root);
        auto* prefabInstance = root->GetComponent<PrefabInstance>();
        ASSERT_NE(nullptr, prefabInstance);
        EXPECT_EQ(static_cast<size_t>(1), root->GetChildren().size());
        SceneEntity* child = root->GetChildren()[0];

        auto* primary = FindPrefabPayloadComponentByName(
            *static_cast<Actor*>(child), "PrimaryPayload");
        auto* secondary = FindPrefabPayloadComponentByName(
            *static_cast<Actor*>(child), "SecondaryPayload");
        ASSERT_NE(nullptr, primary);
        ASSERT_NE(nullptr, secondary);

        primary->payload = "dirty-primary";
        secondary->payload = "dirty-secondary";
        prefabInstance->AddOverride(
            {"PrefabPayloadComponent", "serializedData", primary->payload, "PrimaryPayload", "Child"});
        prefabInstance->AddOverride(
            {"PrefabPayloadComponent", "serializedData", secondary->payload, "SecondaryPayload", "Child"});

        prefabInstance->RevertProperty(
            "PrefabPayloadComponent", "serializedData", "SecondaryPayload", "Child");

        EXPECT_EQ(std::string("dirty-primary"), primary->payload);
        EXPECT_EQ(std::string("slot=secondary"), secondary->payload);
        EXPECT_TRUE(prefabInstance->IsOverridden(
            "PrefabPayloadComponent", "serializedData", "PrimaryPayload", "Child"));
        EXPECT_FALSE(prefabInstance->IsOverridden(
            "PrefabPayloadComponent", "serializedData", "SecondaryPayload", "Child"));

        sceneManager.Shutdown();
    }

    TEST(ResourceInstantiationValidation, PrefabInstanceRevertPropertyScopesComponentBindingsByEntityPath)
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
        ASSERT_NE(nullptr, root);
        auto* prefabInstance = root->GetComponent<PrefabInstance>();
        ASSERT_NE(nullptr, prefabInstance);
        EXPECT_EQ(static_cast<size_t>(1), root->GetChildren().size());
        SceneEntity* child = root->GetChildren()[0];

        auto* rootPayload = FindPrefabPayloadComponentByName(
            *static_cast<Actor*>(root), "PrimaryPayload_1");
        auto* childPayload = FindPrefabPayloadComponentByName(
            *static_cast<Actor*>(child), "PrimaryPayload_1");
        ASSERT_NE(nullptr, rootPayload);
        ASSERT_NE(nullptr, childPayload);

        rootPayload->payload = "dirty-root";
        childPayload->payload = "dirty-child";
        prefabInstance->AddOverride(
            {"PrefabPayloadComponent", "serializedData", childPayload->payload, "PrimaryPayload_1", "Child"});

        prefabInstance->RevertProperty(
            "PrefabPayloadComponent", "serializedData", "PrimaryPayload_1", "Child");

        EXPECT_EQ(std::string("dirty-root"), rootPayload->payload);
        EXPECT_EQ(std::string("slot=child"), childPayload->payload);

        sceneManager.Shutdown();
        ActorFactory::ClearAll();
    }

    TEST(ResourceInstantiationValidation, PrefabInstanceRevertPropertyRestoresLegacyComponentPayload)
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
        ASSERT_NE(nullptr, instance);
        auto* prefabInstance = instance->GetComponent<PrefabInstance>();
        ASSERT_NE(nullptr, prefabInstance);

        auto* actorComponent = static_cast<Actor*>(instance)->GetComponent<PrefabPayloadComponent>();
        ASSERT_NE(nullptr, actorComponent);
        auto* legacyComponent = instance->GetComponent<PrefabLegacyPayloadComponent>();
        ASSERT_NE(nullptr, legacyComponent);

        actorComponent->payload = "ammo=99";
        legacyComponent->payload = "legacy=disabled";
        prefabInstance->AddOverride({"PrefabPayloadComponent", "serializedData", actorComponent->payload});
        prefabInstance->AddOverride({"PrefabLegacyPayloadComponent", "serializedData", legacyComponent->payload});

        prefabInstance->RevertProperty("PrefabLegacyPayloadComponent", "serializedData");

        EXPECT_EQ(std::string("ammo=99"), actorComponent->payload);
        EXPECT_EQ(std::string("legacy=enabled"), legacyComponent->payload);
        EXPECT_TRUE(prefabInstance->IsOverridden("PrefabPayloadComponent", "serializedData"));
        EXPECT_FALSE(prefabInstance->IsOverridden("PrefabLegacyPayloadComponent", "serializedData"));
        EXPECT_EQ(static_cast<size_t>(1), prefabInstance->GetOverrides().size());

        sceneManager.Shutdown();
    }

    TEST(ResourceInstantiationValidation, PrefabInstanceRevertPropertyKeepsPayloadOverrideWhenComponentMissing)
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
        ASSERT_NE(nullptr, instance);
        auto* prefabInstance = instance->GetComponent<PrefabInstance>();
        ASSERT_NE(nullptr, prefabInstance);

        EXPECT_TRUE(static_cast<Actor*>(instance)->RemoveComponent<PrefabPayloadComponent>());
        EXPECT_TRUE(static_cast<Actor*>(instance)->GetComponent<PrefabPayloadComponent>() == nullptr);
        prefabInstance->AddOverride({"PrefabPayloadComponent", "serializedData", std::string("ammo=99")});

        prefabInstance->RevertProperty("PrefabPayloadComponent", "serializedData");

        EXPECT_TRUE(static_cast<Actor*>(instance)->GetComponent<PrefabPayloadComponent>() == nullptr);
        EXPECT_TRUE(prefabInstance->IsOverridden("PrefabPayloadComponent", "serializedData"));
        EXPECT_EQ(static_cast<size_t>(1), prefabInstance->GetOverrides().size());

        sceneManager.Shutdown();
    }

    TEST(ResourceInstantiationValidation, PrefabInstanceRevertPropertyAcceptsEmptyRootTarget)
    {
        PrefabEntityData data;
        data.name = "EmptyRootTarget";
        data.position = Vec3(1.0f, 2.0f, 3.0f);

        auto prefab = Prefab::CreateFromData({data});

        SceneManager sceneManager;
        sceneManager.Initialize();

        SceneEntity* instance = prefab->Instantiate(sceneManager);
        ASSERT_NE(nullptr, instance);
        auto* prefabInstance = instance->GetComponent<PrefabInstance>();
        ASSERT_NE(nullptr, prefabInstance);

        instance->SetPosition(Vec3(9.0f, 9.0f, 9.0f));
        prefabInstance->AddOverride({"", "position", Vec3(9.0f, 9.0f, 9.0f)});

        prefabInstance->RevertProperty("", "position");

        EXPECT_EQ(Vec3(1.0f, 2.0f, 3.0f), instance->GetPosition());
        EXPECT_FALSE(prefabInstance->IsOverridden("", "position"));
        EXPECT_TRUE(prefabInstance->GetOverrides().empty());

        sceneManager.Shutdown();
    }

    TEST(ResourceInstantiationValidation, PrefabInstanceRevertPropertyClearsRootTargetAliases)
    {
        PrefabEntityData data;
        data.name = "AliasRootTarget";
        data.position = Vec3(1.0f, 2.0f, 3.0f);

        auto prefab = Prefab::CreateFromData({data});

        SceneManager sceneManager;
        sceneManager.Initialize();

        SceneEntity* instance = prefab->Instantiate(sceneManager);
        ASSERT_NE(nullptr, instance);
        auto* prefabInstance = instance->GetComponent<PrefabInstance>();
        ASSERT_NE(nullptr, prefabInstance);

        instance->SetPosition(Vec3(9.0f, 9.0f, 9.0f));
        prefabInstance->AddOverride({"SceneEntity", "position", Vec3(9.0f, 9.0f, 9.0f)});

        prefabInstance->RevertProperty("Actor", "position");

        EXPECT_EQ(Vec3(1.0f, 2.0f, 3.0f), instance->GetPosition());
        EXPECT_FALSE(prefabInstance->IsOverridden("SceneEntity", "position"));
        EXPECT_TRUE(prefabInstance->GetOverrides().empty());

        sceneManager.Shutdown();
    }

    TEST(ResourceInstantiationValidation, PrefabInstanceRevertPropertyKeepsUnsupportedOverride)
    {
        PrefabEntityData data;
        data.name = "UnsupportedOverrideRoot";

        auto prefab = Prefab::CreateFromData({data});

        SceneManager sceneManager;
        sceneManager.Initialize();

        SceneEntity* instance = prefab->Instantiate(sceneManager);
        ASSERT_NE(nullptr, instance);
        auto* prefabInstance = instance->GetComponent<PrefabInstance>();
        ASSERT_NE(nullptr, prefabInstance);

        prefabInstance->AddOverride({"UnknownComponent", "payload", std::string("custom")});

        prefabInstance->RevertProperty("UnknownComponent", "payload");

        EXPECT_TRUE(prefabInstance->IsOverridden("UnknownComponent", "payload"));
        EXPECT_EQ(static_cast<size_t>(1), prefabInstance->GetOverrides().size());

        sceneManager.Shutdown();
    }

    TEST(ResourceInstantiationValidation, PrefabInstanceApplyToPrefabWritesRootEntityState)
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
        ASSERT_NE(nullptr, instance);
        auto* prefabInstance = instance->GetComponent<PrefabInstance>();
        ASSERT_NE(nullptr, prefabInstance);

        instance->SetPosition(Vec3(9.0f, 8.0f, 7.0f));
        instance->SetRotation(Quat(0.7071068f, 0.0f, 0.7071068f, 0.0f));
        instance->SetScale(Vec3(3.0f, 4.0f, 5.0f));
        instance->SetLayerMask(0xAu);
        instance->SetActive(false);
        prefabInstance->AddOverride({"SceneEntity", "position", Vec3(9.0f, 8.0f, 7.0f)});
        prefabInstance->AddOverride({"SceneEntity", "scale", Vec3(3.0f, 4.0f, 5.0f)});

        prefabInstance->ApplyToPrefab();

        const PrefabEntityData* rootData = prefab->GetRootData();
        ASSERT_NE(nullptr, rootData);
        EXPECT_EQ(Vec3(9.0f, 8.0f, 7.0f), rootData->position);
        EXPECT_EQ(Quat(0.7071068f, 0.0f, 0.7071068f, 0.0f), rootData->rotation);
        EXPECT_EQ(Vec3(3.0f, 4.0f, 5.0f), rootData->scale);
        EXPECT_EQ(static_cast<uint32_t>(0xAu), rootData->layerMask);
        EXPECT_FALSE(rootData->isActive);
        EXPECT_TRUE(prefabInstance->GetOverrides().empty());

        sceneManager.Shutdown();
    }

    TEST(ResourceInstantiationValidation, PrefabInstanceApplyToPrefabWritesRootComponentPayloads)
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
        ASSERT_NE(nullptr, instance);
        auto* prefabInstance = instance->GetComponent<PrefabInstance>();
        ASSERT_NE(nullptr, prefabInstance);

        auto* actorComponent = static_cast<Actor*>(instance)->GetComponent<PrefabPayloadComponent>();
        ASSERT_NE(nullptr, actorComponent);
        actorComponent->payload = "ammo=99;mode=auto";

        auto* legacyComponent = instance->GetComponent<PrefabLegacyPayloadComponent>();
        ASSERT_NE(nullptr, legacyComponent);
        legacyComponent->payload = "legacy=new";

        prefabInstance->AddOverride({"PrefabPayloadComponent", "serializedData", actorComponent->payload});
        prefabInstance->AddOverride({"PrefabLegacyPayloadComponent", "serializedData", legacyComponent->payload});
        prefabInstance->AddOverride({"UnknownComponent", "serializedData", std::string("keep")});

        prefabInstance->ApplyToPrefab();

        const PrefabEntityData* rootData = prefab->GetRootData();
        ASSERT_NE(nullptr, rootData);
        EXPECT_EQ(static_cast<size_t>(1), rootData->actorComponentData.size());
        EXPECT_EQ(std::string("PrefabPayloadComponent"),
                       rootData->actorComponentData[0].className);
        EXPECT_EQ(std::string("ammo=99;mode=auto"),
                       rootData->actorComponentData[0].serializedData);

        auto it = rootData->componentData.find("PrefabLegacyPayloadComponent");
        EXPECT_TRUE(it != rootData->componentData.end());
        EXPECT_EQ(std::string("legacy=new"), it->second);

        EXPECT_FALSE(prefabInstance->IsOverridden("PrefabPayloadComponent", "serializedData"));
        EXPECT_FALSE(prefabInstance->IsOverridden("PrefabLegacyPayloadComponent", "serializedData"));
        EXPECT_TRUE(prefabInstance->IsOverridden("UnknownComponent", "serializedData"));

        sceneManager.Shutdown();
    }

    TEST(ResourceInstantiationValidation, PrefabInstanceApplyToPrefabClearsOnlyMatchingNamedPayloadOverride)
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
        ASSERT_NE(nullptr, instance);
        auto* prefabInstance = instance->GetComponent<PrefabInstance>();
        ASSERT_NE(nullptr, prefabInstance);

        auto* primary = FindPrefabPayloadComponentByName(*static_cast<Actor*>(instance),
                                                         "PrimaryPayload");
        auto* secondary = FindPrefabPayloadComponentByName(*static_cast<Actor*>(instance),
                                                           "SecondaryPayload");
        ASSERT_NE(nullptr, primary);
        ASSERT_NE(nullptr, secondary);

        primary->payload = "slot=primary-applied";
        prefabInstance->AddOverride(
            {"PrefabPayloadComponent", "serializedData", primary->payload, "PrimaryPayload"});
        prefabInstance->AddOverride(
            {"PrefabPayloadComponent", "serializedData", std::string("stale"), "DetachedPayload"});

        prefabInstance->ApplyToPrefab();

        const PrefabEntityData* rootData = prefab->GetRootData();
        ASSERT_NE(nullptr, rootData);
        EXPECT_EQ(static_cast<size_t>(2), rootData->actorComponentData.size());
        EXPECT_FALSE(
            prefabInstance->IsOverridden("PrefabPayloadComponent", "serializedData", "PrimaryPayload"));
        EXPECT_TRUE(
            prefabInstance->IsOverridden("PrefabPayloadComponent", "serializedData", "DetachedPayload"));
        EXPECT_EQ(static_cast<size_t>(1), prefabInstance->GetOverrides().size());

        sceneManager.Shutdown();
    }

    TEST(ResourceInstantiationValidation, PrefabInstanceApplyToPrefabWritesChildEntityStateAndPayload)
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
        ASSERT_NE(nullptr, root);
        auto* prefabInstance = root->GetComponent<PrefabInstance>();
        ASSERT_NE(nullptr, prefabInstance);
        EXPECT_EQ(static_cast<size_t>(1), root->GetChildren().size());
        SceneEntity* child = root->GetChildren()[0];
        auto* childPayload = FindPrefabPayloadComponentByName(
            *static_cast<Actor*>(child), "ChildPayload");
        ASSERT_NE(nullptr, childPayload);

        child->SetPosition(Vec3(0.0f, 7.0f, 0.0f));
        childPayload->payload = "slot=new";
        prefabInstance->AddOverride(
            {"SceneEntity", "position", child->GetPosition(), "", "Child"});
        prefabInstance->AddOverride(
            {"PrefabPayloadComponent", "serializedData", childPayload->payload, "ChildPayload", "Child"});

        prefabInstance->ApplyToPrefab();

        const PrefabEntityData* updatedChildData = prefab->GetEntityData(1);
        ASSERT_NE(nullptr, updatedChildData);
        EXPECT_EQ(Vec3(0.0f, 7.0f, 0.0f), updatedChildData->position);
        EXPECT_EQ(static_cast<size_t>(1), updatedChildData->actorComponentData.size());
        EXPECT_EQ(std::string("slot=new"),
                       updatedChildData->actorComponentData[0].serializedData);
        EXPECT_FALSE(prefabInstance->IsOverridden("SceneEntity", "position", "", "Child"));
        EXPECT_FALSE(prefabInstance->IsOverridden(
            "PrefabPayloadComponent", "serializedData", "ChildPayload", "Child"));

        sceneManager.Shutdown();
    }

    TEST(ResourceInstantiationValidation, PrefabInstanceApplyToPrefabRebuildsRuntimeEntityBindings)
    {
        PrefabEntityData rootData;
        rootData.name = "ApplyRebindRoot";

        PrefabEntityData childData;
        childData.name = "Child";
        childData.parentIndex = 0;
        childData.position = Vec3(1.0f, 2.0f, 3.0f);

        auto prefab = Prefab::CreateFromData({rootData, childData});

        SceneManager sceneManager;
        sceneManager.Initialize();

        SceneEntity* root = prefab->Instantiate(sceneManager);
        ASSERT_NE(nullptr, root);
        auto* prefabInstance = root->GetComponent<PrefabInstance>();
        ASSERT_NE(nullptr, prefabInstance);
        EXPECT_EQ(static_cast<size_t>(1), root->GetChildren().size());

        SceneEntity* child = root->GetChildren()[0];
        prefabInstance->ApplyToPrefab();

        child->SetName("RenamedAfterApply");
        child->SetPosition(Vec3(9.0f, 9.0f, 9.0f));
        prefabInstance->AddOverride(
            {"SceneEntity", "position", child->GetPosition(), "", "Child"});

        prefabInstance->RevertAll();

        EXPECT_EQ(static_cast<size_t>(1), root->GetChildren().size());
        EXPECT_EQ(child, root->GetChildren()[0]);
        EXPECT_EQ(std::string("Child"), child->GetName());
        EXPECT_EQ(Vec3(1.0f, 2.0f, 3.0f), child->GetPosition());
        EXPECT_TRUE(prefabInstance->GetOverrides().empty());

        sceneManager.Shutdown();
    }

    TEST(ResourceInstantiationValidation, PrefabInstanceApplyToPrefabAddsLiveChildEntity)
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
        ASSERT_NE(nullptr, root);
        auto* prefabInstance = root->GetComponent<PrefabInstance>();
        ASSERT_NE(nullptr, prefabInstance);

        ActorSpawnParams childParams;
        childParams.name = "AddedChild";
        childParams.parent = root;
        SceneEntity* child = sceneManager.SpawnActor(childParams);
        ASSERT_NE(nullptr, child);
        child->SetPosition(Vec3(4.0f, 5.0f, 6.0f));
        auto* payload = child->AddComponent<PrefabPayloadComponent>();
        ASSERT_NE(nullptr, payload);
        payload->SetName("AddedPayload");
        payload->payload = "slot=added";

        prefabInstance->ApplyToPrefab();

        EXPECT_EQ(static_cast<size_t>(2), prefab->GetEntityCount());
        const PrefabEntityData* addedData = prefab->GetEntityData(1);
        ASSERT_NE(nullptr, addedData);
        EXPECT_EQ(std::string("AddedChild"), addedData->name);
        EXPECT_EQ(static_cast<int32_t>(0), addedData->parentIndex);
        EXPECT_EQ(Vec3(4.0f, 5.0f, 6.0f), addedData->position);
        EXPECT_EQ(static_cast<size_t>(1), addedData->actorComponentData.size());
        EXPECT_EQ(std::string("PrefabPayloadComponent"),
                       addedData->actorComponentData[0].className);
        EXPECT_EQ(std::string("AddedPayload"),
                       addedData->actorComponentData[0].name);
        EXPECT_EQ(std::string("slot=added"),
                       addedData->actorComponentData[0].serializedData);

        sceneManager.Shutdown();
    }

    TEST(ResourceInstantiationValidation, PrefabInstanceApplyToPrefabRemovesDeletedChildEntityAndOverrides)
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
        ASSERT_NE(nullptr, root);
        auto* prefabInstance = root->GetComponent<PrefabInstance>();
        ASSERT_NE(nullptr, prefabInstance);
        EXPECT_EQ(static_cast<size_t>(1), root->GetChildren().size());

        SceneEntity* child = root->GetChildren()[0];
        prefabInstance->AddOverride(
            {"SceneEntity", "position", Vec3(9.0f, 9.0f, 9.0f), "", "Child"});
        sceneManager.DestroyEntity(child->GetHandle());

        prefabInstance->ApplyToPrefab();

        EXPECT_EQ(static_cast<size_t>(1), prefab->GetEntityCount());
        EXPECT_FALSE(prefabInstance->IsOverridden("SceneEntity", "position", "", "Child"));

        sceneManager.Shutdown();
    }

    TEST(ResourceInstantiationValidation, PrefabInstanceApplyToPrefabPreservesNestedParentIndices)
    {
        PrefabEntityData rootData;
        rootData.name = "ApplyNestedRoot";

        auto prefab = Prefab::CreateFromData({rootData});

        SceneManager sceneManager;
        sceneManager.Initialize();

        SceneEntity* root = prefab->Instantiate(sceneManager);
        ASSERT_NE(nullptr, root);
        auto* prefabInstance = root->GetComponent<PrefabInstance>();
        ASSERT_NE(nullptr, prefabInstance);

        ActorSpawnParams branchParams;
        branchParams.name = "Branch";
        branchParams.parent = root;
        SceneEntity* branch = sceneManager.SpawnActor(branchParams);
        ASSERT_NE(nullptr, branch);

        ActorSpawnParams leafParams;
        leafParams.name = "Leaf";
        leafParams.parent = branch;
        SceneEntity* leaf = sceneManager.SpawnActor(leafParams);
        ASSERT_NE(nullptr, leaf);
        leaf->SetScale(Vec3(2.0f, 3.0f, 4.0f));

        prefabInstance->ApplyToPrefab();

        EXPECT_EQ(static_cast<size_t>(3), prefab->GetEntityCount());
        const PrefabEntityData* branchData = prefab->GetEntityData(1);
        const PrefabEntityData* leafData = prefab->GetEntityData(2);
        ASSERT_NE(nullptr, branchData);
        ASSERT_NE(nullptr, leafData);
        EXPECT_EQ(std::string("Branch"), branchData->name);
        EXPECT_EQ(static_cast<int32_t>(0), branchData->parentIndex);
        EXPECT_EQ(std::string("Leaf"), leafData->name);
        EXPECT_EQ(static_cast<int32_t>(1), leafData->parentIndex);
        EXPECT_EQ(Vec3(2.0f, 3.0f, 4.0f), leafData->scale);

        sceneManager.Shutdown();
    }

    TEST(ResourceInstantiationValidation, PrefabInstanceApplyToPrefabPreservesUnsupportedOverrideOnExistingChild)
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
        ASSERT_NE(nullptr, root);
        auto* prefabInstance = root->GetComponent<PrefabInstance>();
        ASSERT_NE(nullptr, prefabInstance);

        prefabInstance->AddOverride(
            {"CustomComponent", "customField", std::string("value"), "", "Child"});

        prefabInstance->ApplyToPrefab();

        EXPECT_TRUE(prefabInstance->IsOverridden(
            "CustomComponent", "customField", "", "Child"));

        sceneManager.Shutdown();
    }

    TEST(ResourceInstantiationValidation, PrefabInstanceApplyToPrefabClearsRootAliasOverrides)
    {
        PrefabEntityData data;
        data.name = "ApplyAliases";
        data.position = Vec3(1.0f, 2.0f, 3.0f);
        data.scale = Vec3(1.0f, 1.0f, 1.0f);

        auto prefab = Prefab::CreateFromData({data});

        SceneManager sceneManager;
        sceneManager.Initialize();

        SceneEntity* instance = prefab->Instantiate(sceneManager);
        ASSERT_NE(nullptr, instance);
        auto* prefabInstance = instance->GetComponent<PrefabInstance>();
        ASSERT_NE(nullptr, prefabInstance);

        instance->SetPosition(Vec3(5.0f, 6.0f, 7.0f));
        instance->SetScale(Vec3(2.0f, 2.0f, 2.0f));
        prefabInstance->AddOverride({"", "position", Vec3(5.0f, 6.0f, 7.0f)});
        prefabInstance->AddOverride({"Actor", "scale", Vec3(2.0f, 2.0f, 2.0f)});

        prefabInstance->ApplyToPrefab();

        EXPECT_FALSE(prefabInstance->IsOverridden("", "position"));
        EXPECT_FALSE(prefabInstance->IsOverridden("Actor", "scale"));
        EXPECT_TRUE(prefabInstance->GetOverrides().empty());

        const PrefabEntityData* rootData = prefab->GetRootData();
        ASSERT_NE(nullptr, rootData);
        EXPECT_EQ(Vec3(5.0f, 6.0f, 7.0f), rootData->position);
        EXPECT_EQ(Vec3(2.0f, 2.0f, 2.0f), rootData->scale);

        sceneManager.Shutdown();
    }

    TEST(ResourceInstantiationValidation, PrefabInstanceApplyToPrefabPreservesUnsupportedOverrides)
    {
        PrefabEntityData data;
        data.name = "ApplyUnsupported";
        data.position = Vec3(1.0f, 2.0f, 3.0f);

        auto prefab = Prefab::CreateFromData({data});

        SceneManager sceneManager;
        sceneManager.Initialize();

        SceneEntity* instance = prefab->Instantiate(sceneManager);
        ASSERT_NE(nullptr, instance);
        auto* prefabInstance = instance->GetComponent<PrefabInstance>();
        ASSERT_NE(nullptr, prefabInstance);

        instance->SetPosition(Vec3(4.0f, 5.0f, 6.0f));
        prefabInstance->AddOverride({"SceneEntity", "position", Vec3(4.0f, 5.0f, 6.0f)});
        prefabInstance->AddOverride({"SceneEntity", "custom", std::string("root-custom")});
        prefabInstance->AddOverride({"UnknownComponent", "payload", std::string("component-custom")});

        prefabInstance->ApplyToPrefab();

        EXPECT_FALSE(prefabInstance->IsOverridden("SceneEntity", "position"));
        EXPECT_TRUE(prefabInstance->IsOverridden("SceneEntity", "custom"));
        EXPECT_TRUE(prefabInstance->IsOverridden("UnknownComponent", "payload"));
        EXPECT_EQ(static_cast<size_t>(2), prefabInstance->GetOverrides().size());

        const PrefabEntityData* rootData = prefab->GetRootData();
        ASSERT_NE(nullptr, rootData);
        EXPECT_EQ(Vec3(4.0f, 5.0f, 6.0f), rootData->position);

        sceneManager.Shutdown();
    }

    TEST(ResourceInstantiationValidation, PrefabInstanceApplyToPrefabWithoutPrefabKeepsOverrides)
    {
        SceneManager sceneManager;
        sceneManager.Initialize();

        ActorSpawnParams params;
        params.name = "NoPrefabActor";
        SceneEntity* entity = sceneManager.SpawnActor(params);
        ASSERT_NE(nullptr, entity);

        auto* prefabInstance = entity->AddComponent<PrefabInstance>();
        ASSERT_NE(nullptr, prefabInstance);
        prefabInstance->AddOverride({"SceneEntity", "position", Vec3(1.0f, 2.0f, 3.0f)});

        prefabInstance->ApplyToPrefab();

        EXPECT_TRUE(prefabInstance->IsOverridden("SceneEntity", "position"));
        EXPECT_EQ(static_cast<size_t>(1), prefabInstance->GetOverrides().size());

        sceneManager.Shutdown();
    }

    TEST(ResourceInstantiationValidation, WorldLoadModelResourceReplacesSceneContent)
    {
        ResourceManagerTestGuard resourceGuard(true);
        auto& resourceManager = ResourceManager::Get();
        resourceManager.RegisterLoader(ResourceType::Model, std::make_unique<WorldLoadModelLoader>());

        World world;
        world.Initialize();

        ActorSpawnParams existingParams;
        existingParams.name = "ExistingActor";
        ASSERT_NE(nullptr, world.SpawnActor(existingParams));
        EXPECT_EQ(static_cast<size_t>(1), world.GetSceneManager()->GetEntityCount());

        world.Load("fixture.model");

        SceneManager* sceneManager = world.GetSceneManager();
        ASSERT_NE(nullptr, sceneManager);
        EXPECT_EQ(static_cast<size_t>(2), sceneManager->GetEntityCount());

        auto* root = FindEntityByName(*sceneManager, "LoadedWorldRoot");
        auto* child = FindEntityByName(*sceneManager, "LoadedWorldChild");
        ASSERT_NE(nullptr, root);
        ASSERT_NE(nullptr, child);
        EXPECT_EQ(nullptr, FindEntityByName(*sceneManager, "ExistingActor"));
        EXPECT_EQ(Vec3(1.0f, 2.0f, 3.0f), root->GetPosition());
        EXPECT_EQ(root, child->GetParent());
        EXPECT_EQ(Vec3(4.0f, 5.0f, 6.0f), child->GetPosition());
        EXPECT_EQ(static_cast<Actor*>(root), world.GetActor(root->GetHandle()));

        world.Shutdown();
    }

    TEST(ResourceInstantiationValidation, WorldLoadInvalidRequestKeepsExistingContent)
    {
        ResourceManagerTestGuard resourceGuard(false);

        World world;
        world.Initialize();

        ActorSpawnParams params;
        params.name = "PersistentActor";
        SceneEntity* existing = world.SpawnActor(params);
        ASSERT_NE(nullptr, existing);

        world.Load("missing.model");

        EXPECT_EQ(static_cast<size_t>(1), world.GetSceneManager()->GetEntityCount());
        EXPECT_EQ(existing, FindEntityByName(*world.GetSceneManager(), "PersistentActor"));

        world.Shutdown();
    }

    TEST(ResourceInstantiationValidation, WorldLoadEmptyModelKeepsExistingContent)
    {
        ResourceManagerTestGuard resourceGuard(true);
        auto& resourceManager = ResourceManager::Get();
        resourceManager.RegisterLoader(ResourceType::Model, std::make_unique<WorldLoadEmptyModelLoader>());

        World world;
        world.Initialize();

        ActorSpawnParams params;
        params.name = "PersistentActor";
        SceneEntity* existing = world.SpawnActor(params);
        ASSERT_NE(nullptr, existing);

        world.Load("empty.model");

        EXPECT_EQ(static_cast<size_t>(1), world.GetSceneManager()->GetEntityCount());
        EXPECT_EQ(existing, FindEntityByName(*world.GetSceneManager(), "PersistentActor"));

        world.Shutdown();
    }
} // namespace
