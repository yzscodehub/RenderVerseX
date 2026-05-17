# UE-Style Actor Component Phase 26 Hierarchy Prefab Overrides Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [x]`) syntax for tracking.

**Goal:** Make prefab revert/apply operations work on child entities and child actor components, not only the prefab root.

**Architecture:** Append an optional `entityPath` field to `PropertyOverride` after `componentName`, preserving all existing aggregate initializers. The path is relative to the instantiated prefab root: empty string or `"."` normalizes to root, `"Child"` means a unique direct child named `Child`, `"Arm/Hand"` means nested descendants, and duplicate sibling names are disambiguated with zero-based same-name ordinals such as `"Child[0]"` and `"Child[1]"`. `PrefabInstance` APIs gain an optional `entityPath` argument and always compare normalized paths.

**Tech Stack:** C++20, RenderVerseX Scene module, standalone validation executable `ResourceInstantiationValidation`.

---

## Scope

- Append `PropertyOverride::entityPath` after `componentName`.
- Append `PrefabActorComponentNameBinding::entityPath` after `instanceName` so named component bindings can be scoped per prefab entity.
- Add optional `entityPath` parameters to `RemoveOverride(...)`, `IsOverridden(...)`, and `RevertProperty(...)`.
- Preserve root behavior when `entityPath` is empty.
- Make `RevertAll()` restore existing live entities for every serialized prefab entity path.
- Make `RevertProperty(...)` restore a single entity property or component payload at the requested entity path.
- Make `ApplyToPrefab()` capture root and existing child entity state/component payloads, then clear only matching overrides for paths/components that were actually written back.
- Do not create missing child entities or delete extra live children in this phase; that is a later structural sync phase.
- Do not introduce editor reflection/property metadata in this phase.
- Treat ambiguous unqualified paths and names containing path grammar characters (`/`, `[`, `]`) as not addressable in this phase. They must fail to match rather than silently targeting the first entity.

## Files

- Modify: `E:\WorkSpace\RenderVerseX\Scene\Include\Scene\Prefab.h`
- Modify: `E:\WorkSpace\RenderVerseX\Scene\Private\Prefab.cpp`
- Modify: `E:\WorkSpace\RenderVerseX\Tests\ResourceInstantiationValidation\main.cpp`
- Modify: `E:\WorkSpace\RenderVerseX\Docs\superpowers\plans\2026-05-17-ue-style-actor-component-phase26-hierarchy-prefab-overrides.md`

---

## Task 1: Add Minimal Failing Hierarchy Override Coverage

- [x] **Step 1: Add child `RevertAll` state/payload test**

In `Tests\ResourceInstantiationValidation\main.cpp`, add a focused test near the existing `RevertAll` tests:

```cpp
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
```

- [x] **Step 2: Add child entity property `RevertProperty` test**

Add this test near existing `RevertProperty` tests:

```cpp
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
```

- [x] **Step 3: Add child named component payload `RevertProperty` test**

Add this test near named component payload tests:

```cpp
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
```

- [x] **Step 4: Add child `ApplyToPrefab` test**

Add this test near existing `ApplyToPrefab` tests:

```cpp
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
```

- [x] **Step 5: Add root path normalization test**

Add one tiny API-level test:

```cpp
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
```

- [x] **Step 6: Add duplicate sibling path disambiguation test**

Add one focused test showing that `"Child[1]"` targets the second same-name child:

```cpp
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
```

- [x] **Step 7: Add missing live child apply preservation test**

Add a focused apply cleanup guard:

```cpp
bool Test_PrefabInstanceApplyToPrefabPreservesChildOverrideWhenLiveChildMissing()
{
    PrefabEntityData rootData;
    rootData.name = "MissingApplyRoot";

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

    TEST_ASSERT_TRUE(prefabInstance->IsOverridden("SceneEntity", "position", "", "Child"));

    sceneManager.Shutdown();
    return true;
}
```

- [x] **Step 8: Add ambiguous duplicate sibling path no-match test**

Add a focused guard that proves unqualified duplicate sibling names do not select the first match:

```cpp
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
```

- [x] **Step 9: Add path-scoped runtime binding test**

Add this test to ensure child path scoping prevents root binding collision. The root requests `PrimaryPayload_1` and gets runtime `PrimaryPayload_1`; the child is a `PrefabNameCollisionActor`, requests `PrimaryPayload`, and also gets runtime `PrimaryPayload_1`. Without entity-path-scoped binding lookup, the child lookup can pick the root binding and fail to map back to the child prefab data.

```cpp
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
```

- [x] **Step 10: Register the tests**

Register the nine tests near their related existing groups in `main()`.

- [x] **Step 11: Confirm RED**

Run:

```powershell
cmake --build build\Debug --config Debug --target ResourceInstantiationValidation
```

Expected before implementation: compile fails because `PropertyOverride` and `PrefabInstance` APIs do not yet accept `entityPath`.

---

## Task 2: Implement Hierarchy-Aware Override Identity

- [x] **Step 1: Extend public data types**

In `Scene\Include\Scene\Prefab.h`:

```cpp
struct PropertyOverride
{
    std::string componentType;
    std::string propertyPath;
    PropertyValue value;
    std::string componentName;
    std::string entityPath;
};

struct PrefabActorComponentNameBinding
{
    std::string className;
    std::string prefabName;
    size_t ordinal = 0;
    std::string instanceName;
    std::string entityPath;
};
```

- [x] **Step 2: Extend public prefab instance APIs**

Update declarations and definitions:

```cpp
void RemoveOverride(const std::string& componentType,
                    const std::string& propertyPath,
                    const std::string& componentName = "",
                    const std::string& entityPath = "");

bool IsOverridden(const std::string& componentType,
                  const std::string& propertyPath,
                  const std::string& componentName = "",
                  const std::string& entityPath = "") const;

void RevertProperty(const std::string& componentType,
                    const std::string& propertyPath,
                    const std::string& componentName = "",
                    const std::string& entityPath = "");
```

- [x] **Step 3: Include entity path in override identity**

`AddOverride(...)`, `RemoveOverride(...)`, and `IsOverridden(...)` must compare `componentType`, `propertyPath`, `componentName`, and normalized `entityPath`.

---

## Task 3: Implement Prefab/Live Entity Path Helpers

- [x] **Step 1: Add path normalization and splitting helpers**

In `Scene\Private\Prefab.cpp`, add anonymous namespace helpers:

```cpp
bool IsRootEntityPath(const std::string& entityPath);
std::string NormalizeEntityPath(const std::string& entityPath);
std::vector<std::string> SplitEntityPath(const std::string& entityPath);
```

`NormalizeEntityPath("")` and `NormalizeEntityPath(".")` return `""`. Repeated slashes, empty segments, unqualified duplicate sibling paths, and names containing `/`, `[`, or `]` must fail lookup instead of matching the first child.

- [x] **Step 2: Add prefab entity path helpers**

Add helpers that derive paths from `PrefabEntityData::parentIndex`, sibling names, and same-name sibling ordinals:

```cpp
std::vector<std::string> BuildPrefabEntityPaths(const Prefab& prefab);
const PrefabEntityData* FindPrefabEntityDataByPath(const Prefab& prefab,
                                                   const std::string& entityPath);
```

Root path must be `""`. Unique child sibling names use `Name`; duplicate sibling names use `Name[0]`, `Name[1]`, etc. This makes path generation deterministic for the current `parentIndex/name` hierarchy.

- [x] **Step 3: Add live entity lookup helpers**

Add:

```cpp
SceneEntity* FindLiveEntityByPath(SceneEntity& root, const std::string& entityPath);
const SceneEntity* FindLiveEntityByPath(const SceneEntity& root, const std::string& entityPath);
```

These traverse `SceneEntity::GetChildren()` by name segment.
If a segment lacks an ordinal and multiple siblings share the requested name, lookup returns `nullptr` rather than choosing the first sibling.

---

## Task 4: Make Revert/Apply Hierarchy-Aware

- [x] **Step 1: Scope name bindings by entity path**

Update `Prefab::CreateComponents(...)` to accept the current `entityPath`, and store it in each `PrefabActorComponentNameBinding`.

In `Prefab::InstantiateInternal(...)`, build prefab entity paths once and pass each path into `CreateComponents(...)`. Store all entity bindings in the root `PrefabInstance`, not only root bindings.

- [x] **Step 2: Scope actor component payload lookup by entity path**

Update `FindActorComponentNameBinding(...)`, `FindActorComponentNameBindingByInstanceName(...)`, `FindActorComponentDataByRuntimeName(...)`, `ApplyPrefabEntityComponentPayloads(...)`, and `ApplyPrefabEntityComponentPayload(...)` so bindings only match the requested `entityPath`.

- [x] **Step 3: Extend `RevertAll()`**

For each prefab entity path:

1. Find the live entity from the prefab root.
2. If present, apply all serialized entity properties.
3. If present, apply all serialized component payloads using the same path-scoped name bindings.
4. After the pass, clear overrides.

- [x] **Step 4: Extend `RevertProperty(...)`**

Resolve both target prefab data and live entity by `entityPath`. Apply entity properties or `"serializedData"` component payloads only to that target. Remove only matching overrides for the same `entityPath`.

- [x] **Step 5: Extend prefab apply**

Add `Prefab::UpdateHierarchyStateFrom(const SceneEntity& rootEntity)` to capture properties/components from each matching live entity path into its existing prefab entity data. Return a capture report containing:

```cpp
struct PrefabComponentPayloadKey
{
    std::string entityPath;
    std::string componentType;
    std::string componentName;
};

struct PrefabApplyCaptureReport
{
    std::vector<std::string> capturedEntityPaths;
    std::vector<PrefabComponentPayloadKey> capturedComponentPayloads;
};
```

`entityPath` must be normalized before insertion. `componentName` is empty for legacy components and class-only actor component matches; named actor components also insert their captured runtime component name. Clearing rules:

- Entity property overrides clear only when normalized `entityPath` is in `capturedEntityPaths`.
- Component payload overrides with empty `componentName` clear when any captured payload key has matching `entityPath` and `componentType`.
- Component payload overrides with non-empty `componentName` clear only when a captured payload key matches `entityPath`, `componentType`, and `componentName`.

Do not add or remove prefab entities in this phase.

Update `PrefabInstance::ApplyToPrefab()` to call `UpdateHierarchyStateFrom(...)`, clear name bindings, remove supported property overrides only for captured entity paths, and remove supported component payload overrides only for captured component payload keys.

---

## Task 5: Validate, Review, And Commit

- [x] **Step 1: Focused validation**

Run:

```powershell
cmake --build build\Debug --config Debug --target ResourceInstantiationValidation
.\build\Debug\Tests\Debug\ResourceInstantiationValidation.exe
```

- [x] **Step 2: Required build/test validation**

Run:

```powershell
git diff --check
foreach ($target in @('ActorComponentValidation','ResourceInstantiationValidation','RenderSceneValidation','ModelViewer')) {
    cmake --build build\Debug --config Debug --target $target
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}
.\build\Debug\Tests\Debug\ActorComponentValidation.exe
.\build\Debug\Tests\Debug\ResourceInstantiationValidation.exe
.\build\Debug\Tests\Debug\RenderSceneValidation.exe
```

- [x] **Step 3: ModelViewer smoke**

Run `ModelViewer.exe C:\Users\yinzs\Desktop\DamagedHelmet.glb` for several seconds. Confirm DamagedHelmet loads, mesh/texture upload starts, RenderGraph stats appear, and stderr is empty.

- [x] **Step 4: Request exactly one code review agent**

Ask for Critical and Important issues only.

- [x] **Step 5: Fix any Critical or Important findings**

If there are none, continue.

- [x] **Step 6: Update this plan checklist**

Mark completed items.

- [x] **Step 7: Commit**

```powershell
git add Docs\superpowers\plans\2026-05-17-ue-style-actor-component-phase26-hierarchy-prefab-overrides.md Scene\Include\Scene\Prefab.h Scene\Private\Prefab.cpp Tests\ResourceInstantiationValidation\main.cpp
git commit -m "Support hierarchy prefab overrides"
```

---

## Self-Review

- Spec coverage: Covers child entity property revert, child component payload revert, `RevertAll`, `ApplyToPrefab`, path-scoped identity, and path-scoped name bindings.
- Compatibility: `PropertyOverride::entityPath` and `PrefabActorComponentNameBinding::entityPath` are appended, so existing aggregate initializers remain valid.
- Scope: Missing child creation/deletion and reflection-style property metadata are explicitly deferred. Duplicate sibling path disambiguation is included through `Name[index]`.
- Placeholder scan: No TODO/TBD placeholders.
