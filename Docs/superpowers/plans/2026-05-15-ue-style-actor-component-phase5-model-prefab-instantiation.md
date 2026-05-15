# UE-Style Actor Component Phase 5 Model Prefab Instantiation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Move model instantiation and prefab class reconstruction onto the UE-style actor/component path while preserving existing `SceneEntity*` entry points.

**Architecture:** `SceneEntity` remains the compatibility object owned by `SceneManager`, but `ModelResource` gets a new `InstantiateActor()` entry point that returns the actor view and internally builds root-attached `StaticMeshComponent` primitives. `ComponentFactory` is widened from legacy `Component*` to `ActorComponent*` for model-node creators, but Phase 5 keeps ownership paths intentionally separated: pure UE-style `ActorComponent` instances are owned by `Actor`, while legacy `Component` subclasses remain owned by the `SceneEntity` legacy component map. `Actor::AddOwnedComponent()` must reject legacy `Component` subclasses to avoid container/owner split. Prefab support remains intentionally class-name based in this phase: it serializes pure actor component class names and instantiates registered actor component classes, but does not attempt full property serialization yet.

**Tech Stack:** C++20, RenderVerseX Resource/Scene modules, custom validation executables, CMake.

## Compatibility Decisions

- `ComponentFactory::Creator` is the model-node instantiation registry. It will return `ActorComponent*` because Phase 5 model nodes create `StaticMeshComponent` primitives. It is not a generic legacy-component reconstruction API.
- `ComponentFactory::ClassCreator` / `CreateComponentByClassName(...)` remains the prefab actor-component construction registry and already returns `std::unique_ptr<ActorComponent>`. It stays separate from model-node creators.
- `MeshRendererComponent` remains a supported legacy component for manually authored scenes and existing render fallback. Phase 5 only stops `ModelResource` and model-node factory defaults from creating new `MeshRendererComponent` instances. `RenderSceneCollector` legacy fallback and existing `RenderSceneValidation` coverage remain in place.
- `PrefabEntityData::componentData` stays as the current legacy metadata path. Phase 5 does not add legacy `Component` reconstruction semantics or change its unordered-map ordering behavior. Ordered duplicate support is introduced only for new `actorComponentData`.
- `Actor::AddOwnedComponent(...)` is deliberately narrower than `SceneEntity::AddComponent<T>()`: it accepts only pure actor components. If a legacy `Component` subclass must be added, callers must continue using `SceneEntity::AddComponent<T>()`.

---

## File Structure

- Modify `Resource/Include/Resource/Types/ModelResource.h`
  - Add `Actor* InstantiateActor(SceneManager* scene) const`.
  - Rename documentation from `MeshRendererComponent` to `StaticMeshComponent`.
  - Add private `InstantiateActorNode(...)` helper returning `SceneEntity*`.

- Modify `Resource/Private/Types/ModelResource.cpp`
  - Include `Scene/Actor.h` and `Scene/Components/StaticMeshComponent.h`.
  - Remove direct dependency on `MeshRendererComponent`.
  - Implement `InstantiateActor()` and make old `Instantiate()` delegate to it.
  - Create root-attached `StaticMeshComponent` for both index-mode and legacy mesh-component nodes.

- Modify `Scene/Include/Scene/Actor.h`
  - Add `ActorComponent* AddOwnedComponent(std::unique_ptr<ActorComponent> component)` as the only ownership-transfer API for factory-created pure actor components.
  - Document that legacy `Component` subclasses must use `SceneEntity::AddComponent<T>()` until a later explicit legacy-container bridge exists.

- Modify `Scene/Private/Actor.cpp`
  - Implement `AddOwnedComponent()` using the same lifecycle and auto-registration semantics as `AddComponent<T>()`.
  - Reject `Component` subclasses and return `nullptr` so legacy components cannot bypass `SceneEntity`'s component map or `Component::SetOwner(SceneEntity*)`.

- Modify `Scene/Include/Scene/ComponentFactory.h`
  - Change `Creator` return type from `Component*` to `ActorComponent*`.
  - Change `CreateComponent(...)` return type to `ActorComponent*`.
  - Update comments to say model-node actor components rather than only legacy components.

- Modify `Scene/Private/ComponentFactory.cpp`
  - Include `Scene/Actor.h` and `Scene/Components/StaticMeshComponent.h`.
  - Register default model creator as a `StaticMeshComponent` creator.
  - Create the component through `static_cast<Actor*>(entity)->AddComponent<StaticMeshComponent>()`, attach it to the entity root, assign mesh/materials, and return it as `ActorComponent*`.

- Modify `Scene/Include/Scene/Prefab.h`
  - Add ordered `actorComponentData` to `PrefabEntityData` for UE-style actor component class names, preserving duplicate components of the same class.

- Modify `Scene/Private/Prefab.cpp`
  - Serialize `Actor::GetActorComponents()` class names into ordered `actorComponentData`, excluding `PrefabInstance`, legacy `Component` subclasses, and the compatibility root component.
  - Instantiate registered actor component classes from `actorComponentData` through `ComponentFactory::CreateComponentByClassName`.
  - Insert reconstructed components only through `Actor::AddOwnedComponent(...)`.
  - Preserve legacy `componentData` behavior exactly as it is today; do not add unordered-map replay or property reconstruction in this phase.
  - Include actor component records in `GetMemoryUsage()`.

- Create `Tests/ResourceInstantiationValidation/main.cpp`
  - Tests `ModelResource::InstantiateActor`, old `Instantiate`, `ComponentFactory::RegisterDefaults`, and prefab actor-component class-name reconstruction.

- Modify `Tests/CMakeLists.txt`
  - Add `ResourceInstantiationValidation` target linked with `RVX_TestFramework`, `RVX::Resource`, `RVX::Scene`, and `RVX::World`.

- Modify `Samples/ModelViewer/main.cpp`
  - Update comments that describe model instantiation as `StaticMeshComponent`/primitive based.
  - Remove demo fallback that adds an empty `MeshRendererComponent` after model instantiation if the model is already valid.

---

## Task 1: Add Failing Resource Instantiation Tests

**Files:**
- Create: `Tests/ResourceInstantiationValidation/main.cpp`
- Modify: `Tests/CMakeLists.txt`

- [ ] **Step 1: Write validation tests**

Create `Tests/ResourceInstantiationValidation/main.cpp`:

```cpp
#include "Core/Core.h"
#include "Resource/Types/MaterialResource.h"
#include "Resource/Types/MeshResource.h"
#include "Resource/Types/ModelResource.h"
#include "Scene/Actor.h"
#include "Scene/Component.h"
#include "Scene/ComponentFactory.h"
#include "Scene/Components/StaticMeshComponent.h"
#include "Scene/Mesh.h"
#include "Scene/Node.h"
#include "Scene/Prefab.h"
#include "Scene/SceneManager.h"
#include "TestFramework/TestRunner.h"

#include <algorithm>

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

    ModelResource MakeIndexedModel()
    {
        ModelResource model;
        model.AddMesh(MakeMeshResource(1001));
        model.AddMaterial(MakeMaterialResource(2001));

        auto root = std::make_shared<Node>("RootNode");
        root->SetMeshIndex(0);
        root->SetMaterialIndices({0});
        root->GetLocalTransform().SetPosition(Vec3(3.0f, 0.0f, 0.0f));
        model.SetRootNode(root);
        return model;
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

        ModelResource model = MakeIndexedModel();
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

        ModelResource model = MakeIndexedModel();
        SceneEntity* entity = model.Instantiate(&sceneManager);
        TEST_ASSERT_NOT_NULL(entity);
        TEST_ASSERT_NOT_NULL(static_cast<Actor*>(entity)->GetComponent<StaticMeshComponent>());
        TEST_ASSERT_TRUE(entity->HasComponent<StaticMeshComponent>());

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
        ModelResource model = MakeIndexedModel();

        ActorComponent* component = ComponentFactory::CreateComponent("StaticMesh", entity,
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

    bool Test_PrefabSerializesActorComponentClassNames()
    {
        SceneManager sceneManager;
        sceneManager.Initialize();

        const auto handle = sceneManager.CreateEntity("PrefabSource");
        auto* entity = sceneManager.GetEntity(handle);
        auto* primitive = static_cast<Actor*>(entity)->AddComponent<StaticMeshComponent>();
        TEST_ASSERT_NOT_NULL(primitive);

        auto prefab = Prefab::CreateFromEntity(entity);
        TEST_ASSERT_NOT_NULL(prefab.get());
        const auto* data = prefab->GetRootData();
        TEST_ASSERT_NOT_NULL(data);
        bool foundStaticMesh = false;
        for (const auto& componentData : data->actorComponentData)
        {
            foundStaticMesh = foundStaticMesh || componentData.className == "StaticMeshComponent";
        }
        TEST_ASSERT_TRUE(foundStaticMesh);

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
    suite.AddTest("ComponentFactoryDefaultCreatesStaticMeshComponent",
                  Test_ComponentFactoryDefaultCreatesStaticMeshComponent);
    suite.AddTest("PrefabSerializesActorComponentClassNames",
                  Test_PrefabSerializesActorComponentClassNames);
    suite.AddTest("PrefabInstantiatesRegisteredActorComponentClasses",
                  Test_PrefabInstantiatesRegisteredActorComponentClasses);

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
```

- [ ] **Step 2: Add validation target**

Add after `SpatialComponentValidation` in `Tests/CMakeLists.txt`:

```cmake
# Resource instantiation validation tests
add_executable(ResourceInstantiationValidation
    ResourceInstantiationValidation/main.cpp
)
target_link_libraries(ResourceInstantiationValidation PRIVATE
    RVX_TestFramework
    RVX::Resource
    RVX::Scene
    RVX::World
)
target_compile_features(ResourceInstantiationValidation PRIVATE cxx_std_20)
```

- [ ] **Step 3: Run RED**

Run:

```powershell
cmake --build build\Debug --config Debug --target ResourceInstantiationValidation
```

Expected before implementation:

- Build fails because `Actor::AddOwnedComponent`, `ModelResource::InstantiateActor`, `PrefabEntityData::actorComponentData`, and `ComponentFactory::CreateComponent(...) -> ActorComponent*` are not implemented.

---

## Task 2: Add Actor Owned-Component Insertion API

**Files:**
- Modify: `Scene/Include/Scene/Actor.h`
- Modify: `Scene/Private/Actor.cpp`

- [ ] **Step 1: Declare ownership-transfer API**

In `Scene/Include/Scene/Actor.h`, add this public method near `AddComponent<T>()`:

```cpp
ActorComponent* AddOwnedComponent(std::unique_ptr<ActorComponent> component);
```

Document that this method accepts only pure `ActorComponent` instances. Legacy `Component` subclasses remain owned through `SceneEntity::AddComponent<T>()` so `Component::GetOwner()`, `SceneEntity::GetComponents()`, and `SceneEntity::HasComponent<T>()` for legacy components stay coherent.

- [ ] **Step 2: Implement lifecycle-consistent insertion**

In `Scene/Private/Actor.cpp`, include:

```cpp
#include "Scene/Component.h"
```

Then implement:

```cpp
ActorComponent* Actor::AddOwnedComponent(std::unique_ptr<ActorComponent> component)
{
    if (!component)
        return nullptr;

    if (dynamic_cast<Component*>(component.get()))
    {
        return nullptr;
    }

    ActorComponent* ptr = component.get();
    ptr->SetOwnerActor(this);
    m_components.push_back(std::move(component));
    ptr->OnComponentCreated();

    if (ShouldAutoRegisterComponent(ptr))
    {
        ptr->SetRegistered(true);
        ptr->OnRegister();
        if (!ptr->IsInitialized())
        {
            ptr->InitializeComponent();
            ptr->SetInitialized(true);
        }
    }

    return ptr;
}
```

This is the only API factories and prefab reconstruction may use when they already own a `unique_ptr<ActorComponent>`.
The legacy-component rejection is intentional: inserting a `Component` into `Actor::m_components` would bypass `SceneEntity::m_components` and leave `Component::GetOwner()` unset.

- [ ] **Step 3: Run owned-component RED/GREEN**

Run:

```powershell
cmake --build build\Debug --config Debug --target ResourceInstantiationValidation ActorComponentValidation
.\build\Debug\Tests\Debug\ResourceInstantiationValidation.exe
.\build\Debug\Tests\Debug\ActorComponentValidation.exe
```

Expected after Task 2:

- `ActorAddOwnedComponentUsesNormalLifecycle` passes.
- `AddOwnedComponentRejectsLegacyComponentToAvoidContainerSplit` passes.
- Tests that depend on model/prefab migration may still fail until later tasks.

---

## Task 3: Widen Model-Node ComponentFactory to ActorComponent and StaticMesh Defaults

**Files:**
- Modify: `Scene/Include/Scene/ComponentFactory.h`
- Modify: `Scene/Private/ComponentFactory.cpp`

- [ ] **Step 1: Change creator return types**

In `ComponentFactory.h`:

```cpp
using Creator = std::function<ActorComponent*(SceneEntity*, const Node*, const Resource::ModelResource*)>;
```

Change `CreateComponent(...)` declaration to:

```cpp
static ActorComponent* CreateComponent(const std::string& typeName,
                                       SceneEntity* entity,
                                       const Node* node,
                                       const Resource::ModelResource* model);
```

Update comments to make this boundary explicit: this registry creates components from model `Node` metadata. It is not the prefab class-name registry and not a legacy `Component` replay API.

- [ ] **Step 2: Register StaticMesh default creator**

In `ComponentFactory.cpp`, include:

```cpp
#include "Scene/Actor.h"
#include "Scene/Components/StaticMeshComponent.h"
```

Update `RegisterDefaults()` to clear and register a single default creator named `"StaticMesh"`:

```cpp
ClearAll();

Register("StaticMesh", [](SceneEntity* entity, const Node* node,
                           const Resource::ModelResource* model) -> ActorComponent* {
    if (!entity || !node || !model || node->GetMeshIndex() < 0)
        return nullptr;

    const int meshIndex = node->GetMeshIndex();
    if (static_cast<size_t>(meshIndex) >= model->GetMeshCount())
        return nullptr;

    auto meshHandle = model->GetMesh(static_cast<size_t>(meshIndex));
    if (!meshHandle.IsValid())
        return nullptr;

    auto* primitive = static_cast<Actor*>(entity)->AddComponent<StaticMeshComponent>();
    primitive->AttachToComponent(entity->GetRootComponent());
    primitive->SetMesh(meshHandle);

    const auto& materialIndices = node->GetMaterialIndices();
    for (size_t i = 0; i < materialIndices.size(); ++i)
    {
        const int matIndex = materialIndices[i];
        if (matIndex >= 0 && static_cast<size_t>(matIndex) < model->GetMaterialCount())
        {
            auto material = model->GetMaterial(static_cast<size_t>(matIndex));
            if (material.IsValid())
            {
                primitive->SetMaterial(i, material);
            }
        }
    }

    return primitive;
});
```

- [ ] **Step 3: Keep factory boundary deterministic**

Do not register a second `"MeshRenderer"` default alias in this phase. `CreateComponents()` calls every registered model creator, so two mesh creators would duplicate render primitives. Compatibility is preserved by keeping `ModelResource::Instantiate(...)` as the old public entry point while changing its internals to the same `StaticMeshComponent` path.
This does not remove `MeshRendererComponent` from the engine: callers may still add it through `SceneEntity::AddComponent<MeshRendererComponent>()`, and `RenderSceneCollector` still keeps its legacy fallback path.

- [ ] **Step 4: Keep class creator APIs unchanged**

Do not change `RegisterComponentClass<T>()` or `CreateComponentByClassName(...)`; Prefab will reuse those APIs.
These APIs are the actor-component class registry and remain separate from the model-node creator map.

- [ ] **Step 5: Run GREEN for factory test**

Run:

```powershell
cmake --build build\Debug --config Debug --target ResourceInstantiationValidation
.\build\Debug\Tests\Debug\ResourceInstantiationValidation.exe
```

Expected after Task 3 only:

- Factory-specific test passes.
- ModelResource and Prefab tests may still fail until Tasks 4 and 5.
- Existing direct `MeshRendererComponent` usage is unaffected because only `ComponentFactory` defaults change.

---

## Task 4: Migrate ModelResource to StaticMeshComponent Actor Path

**Files:**
- Modify: `Resource/Include/Resource/Types/ModelResource.h`
- Modify: `Resource/Private/Types/ModelResource.cpp`
- Modify: `Samples/ModelViewer/main.cpp`

- [ ] **Step 1: Add actor instantiation API**

In `ModelResource.h`, forward declare `class Actor;` and add:

```cpp
/// Instantiate the model into the scene and return the actor view.
/// Compatibility implementation currently creates SceneEntity instances.
Actor* InstantiateActor(SceneManager* scene) const;
```

Update `Instantiate()` docs:

```cpp
/// Creates a SceneEntity tree with StaticMeshComponents attached.
```

- [ ] **Step 2: Implement actor path**

In `ModelResource.cpp`, replace `MeshRendererComponent` include with:

```cpp
#include "Scene/Actor.h"
#include "Scene/Components/StaticMeshComponent.h"
```

Implement:

```cpp
Actor* ModelResource::InstantiateActor(SceneManager* scene) const
{
    if (!m_rootNode || !scene)
        return nullptr;

    return InstantiateNode(m_rootNode.get(), scene, nullptr);
}

SceneEntity* ModelResource::Instantiate(SceneManager* scene) const
{
    return dynamic_cast<SceneEntity*>(InstantiateActor(scene));
}
```

- [ ] **Step 3: Use StaticMeshComponent in index mode**

Keep `ComponentFactory::CreateComponents(entity, node, this);` for index mode after Task 3 has switched defaults to `StaticMeshComponent`.

- [ ] **Step 4: Use StaticMeshComponent in legacy pointer mode**

Replace legacy `MeshRendererComponent` creation with:

```cpp
auto* primitive = static_cast<Actor*>(entity)->AddComponent<StaticMeshComponent>();
primitive->AttachToComponent(entity->GetRootComponent());
primitive->SetMesh(meshHandle);
```

Assign materials and render flags:

```cpp
primitive->SetMaterial(i, m_materials[matIndex]);
primitive->SetVisible(meshComp->IsVisible());
primitive->SetCastsShadow(meshComp->CastsShadows());
```

- [ ] **Step 5: Update ModelViewer comments and fallback**

Change comments from `MeshRendererComponent` to `StaticMeshComponent`. Remove the demo code that adds an empty `MeshRendererComponent` if `modelEntity` exists and already came from `Instantiate()`.
Do not remove `MeshRendererComponent` itself, and do not modify `RenderSceneCollector`'s legacy fallback in this phase. Existing `RenderSceneValidation` tests continue to protect the manually-authored legacy path.

- [ ] **Step 6: Run model instantiation validation**

Run:

```powershell
cmake --build build\Debug --config Debug --target ResourceInstantiationValidation RenderSceneValidation ModelViewer
.\build\Debug\Tests\Debug\ResourceInstantiationValidation.exe
.\build\Debug\Tests\Debug\RenderSceneValidation.exe
```

Expected:

- ModelResource tests pass.
- RenderSceneValidation remains green because render extraction already uses primitive registry.
- Legacy `MeshRendererComponent` render fallback remains green through the existing RenderSceneValidation tests.

---

## Task 5: Add Prefab Actor Component Class Reconstruction

**Files:**
- Modify: `Scene/Include/Scene/Prefab.h`
- Modify: `Scene/Private/Prefab.cpp`

- [ ] **Step 1: Extend PrefabEntityData**

Add this structure before `PrefabEntityData`:

```cpp
struct PrefabActorComponentData
{
    std::string className;
    std::string serializedData;
};
```

Then add this field to `PrefabEntityData`:

```cpp
std::vector<PrefabActorComponentData> actorComponentData;
```

Keep `componentData` for legacy `Component` serialization. Use a `std::vector` here rather than a map so prefabs can represent multiple actor components with the same class and preserve component order.
Do not change `componentData` from `std::unordered_map` in this phase. Current legacy component reconstruction is a placeholder, so changing the legacy storage contract would expand scope without restoring behavior. A later reflection/property phase can replace `componentData` with an ordered legacy record list if legacy replay becomes a requirement.

- [ ] **Step 2: Serialize actor component class names**

In `Prefab.cpp`, include:

```cpp
#include "Scene/Actor.h"
#include "Scene/Component.h"
#include "Scene/ComponentFactory.h"
#include "Scene/Prefab.h"
#include "Scene/SceneComponent.h"
```

In `SerializeEntity(...)`, after legacy `GetComponents()` serialization, iterate:

```cpp
for (const auto& actorComponent : entity->GetActorComponents())
{
    if (!actorComponent)
        continue;

    const std::string className = actorComponent->GetClassName();
    if (className == "SceneComponent" || className == "PrefabInstance")
        continue;
    if (dynamic_cast<Component*>(actorComponent.get()))
        continue;

    data.actorComponentData.push_back({className, ""});
}
```

Legacy `Component` subclasses continue to be tracked by `componentData`; this phase does not reinterpret them as actor-owned components.

- [ ] **Step 3: Instantiate actor components by class name**

In `CreateComponents(...)`, replace the empty stub with:

```cpp
for (const auto& componentData : data.actorComponentData)
{
    (void)componentData.serializedData;
    auto component = ComponentFactory::CreateComponentByClassName(componentData.className);
    if (!component)
        continue;

    auto* raw = component.get();
    static_cast<Actor*>(entity)->AddOwnedComponent(std::move(component));

    if (auto* sceneComponent = dynamic_cast<SceneComponent*>(raw))
    {
        sceneComponent->AttachToComponent(entity->GetRootComponent());
    }
}
```

All reconstructed actor components must be inserted through `Actor::AddOwnedComponent(...)`; do not construct raw actor components or bypass ownership.
If `AddOwnedComponent(...)` returns `nullptr`, leave the component discarded and continue; this is expected for any accidentally registered legacy `Component` subclass.
Do not add legacy `componentData` replay here; preserving the existing placeholder behavior avoids inventing partial legacy reconstruction semantics during the actor-component migration.

- [ ] **Step 4: Account for actor component metadata memory**

In `Prefab::GetMemoryUsage()`, add `actorComponentData` string capacity accounting:

```cpp
for (const auto& component : entity.actorComponentData)
{
    size += component.className.capacity() + component.serializedData.capacity();
}
```

- [ ] **Step 5: Run prefab validation**

Run:

```powershell
cmake --build build\Debug --config Debug --target ResourceInstantiationValidation ActorComponentValidation
.\build\Debug\Tests\Debug\ResourceInstantiationValidation.exe
.\build\Debug\Tests\Debug\ActorComponentValidation.exe
```

Expected:

- Prefab actor component tests pass.
- ActorComponent lifecycle tests remain green.

---

## Task 6: Full Validation, ModelViewer Smoke, Review, and Commit

**Files:**
- No new source changes unless validation uncovers issues.

- [ ] **Step 1: Build validation set**

Run:

```powershell
cmake --build build\Debug --config Debug --target ActorComponentValidation SpatialComponentValidation ResourceInstantiationValidation RenderSceneValidation SystemIntegrationTest MaterialSystemValidation RenderPassBindingValidation ModelViewer
```

- [ ] **Step 2: Run validation executables**

Run:

```powershell
.\build\Debug\Tests\Debug\ActorComponentValidation.exe
.\build\Debug\Tests\Debug\SpatialComponentValidation.exe
.\build\Debug\Tests\Debug\ResourceInstantiationValidation.exe
.\build\Debug\Tests\Debug\RenderSceneValidation.exe
.\build\Debug\Tests\Debug\SystemIntegrationTest.exe
.\build\Debug\Tests\Debug\MaterialSystemValidation.exe
.\build\Debug\Tests\Debug\RenderPassBindingValidation.exe
```

Expected: all return `0`.

- [ ] **Step 3: Run ModelViewer smoke**

Run hidden for eight seconds and capture logs:

```powershell
Start-Process -FilePath ".\build\Debug\Samples\ModelViewer\Debug\ModelViewer.exe" -RedirectStandardOutput "build\Debug\modelviewer_phase5_smoke.log" -RedirectStandardError "build\Debug\modelviewer_phase5_smoke.err.log" -WindowStyle Hidden -PassThru
```

Expected log signals:

- `Model loaded successfully`
- `Model instantiated as SceneEntity`
- `Uploaded mesh to GPU`
- `RenderGraph stats`
- no `critical`, `Assertion Failed`, or `[error]`

- [ ] **Step 4: Request one code review**

Use one reviewer:

```text
DESCRIPTION: Implemented UE-style actor component Phase 5 model/prefab instantiation migration. ModelResource now exposes InstantiateActor and creates root-attached StaticMeshComponents; ComponentFactory returns ActorComponent and registers StaticMesh defaults; Prefab serializes and reconstructs actor component class names while keeping legacy entry points.
PLAN_OR_REQUIREMENTS: Docs/superpowers/plans/2026-05-15-ue-style-actor-component-phase5-model-prefab-instantiation.md
BASE_SHA: commit before Task 1 implementation
HEAD_SHA: current HEAD
```

Fix Critical and Important issues before committing.

- [ ] **Step 5: Commit implementation**

Run:

```powershell
git add Resource/Include/Resource/Types/ModelResource.h Resource/Private/Types/ModelResource.cpp Scene/Include/Scene/Actor.h Scene/Private/Actor.cpp Scene/Include/Scene/ComponentFactory.h Scene/Private/ComponentFactory.cpp Scene/Include/Scene/Prefab.h Scene/Private/Prefab.cpp Samples/ModelViewer/main.cpp Tests/ResourceInstantiationValidation/main.cpp Tests/CMakeLists.txt
git commit -m "Migrate model prefab instantiation to actor components"
```

---

## Self-Review

- Spec coverage: Phase 5 asks for actor-based model instantiate path, prefab serialization migration to actor/component graph, and preserved old entry points. Tasks 2-5 add `AddOwnedComponent`, `InstantiateActor`, switch model primitives to `StaticMeshComponent`, and add ordered class-name prefab actor component reconstruction while keeping `Instantiate()` and legacy `componentData`.
- Placeholder scan: No unresolved placeholders remain in executable steps. Existing project deferred-work comments in source are not introduced by the plan except when explicitly left untouched in existing Prefab override methods.
- Type consistency: `ComponentFactory::Creator` and `CreateComponent` return `ActorComponent*`; the default model path returns pure `StaticMeshComponent` primitives. Legacy `Component` subclasses remain on `SceneEntity::AddComponent<T>()`/`componentData` and are rejected by `Actor::AddOwnedComponent()` to avoid split ownership. `StaticMeshComponent` is created through `Actor` ownership and attached to the entity root. Factory-created `unique_ptr<ActorComponent>` instances enter actors only through `Actor::AddOwnedComponent`. Prefab actor component entries use ordered `PrefabActorComponentData` records, so duplicate same-class components are representable.
- Compatibility check: `MeshRendererComponent` is not removed. Phase 5 only stops model instantiation and default model-node factories from creating new MeshRenderer components. Manually-authored legacy components continue through `SceneEntity::AddComponent<MeshRendererComponent>()` and the existing `RenderSceneCollector` fallback.
- Risk check: The plan avoids full property reflection and legacy prefab replay changes in Phase 5, keeps old `SceneEntity*` APIs, and keeps render/RHI actor-agnostic. ModelViewer should continue to render because RenderScene collection already consumes `StaticMeshComponent` primitives.
