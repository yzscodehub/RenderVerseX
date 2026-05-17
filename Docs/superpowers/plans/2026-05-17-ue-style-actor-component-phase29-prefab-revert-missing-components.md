# Prefab Revert Missing Components Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make `PrefabInstance::RevertAll()` restore prefab-owned components that were deleted from an existing live prefab entity.

**Architecture:** Reuse the existing prefab component factory path, but split component creation into helpers that can create one missing component without duplicating components that already exist. `RestoreHierarchyStateTo()` will first ensure the serialized component set exists on each restored entity, then apply serialized payloads and clear overrides through the existing `RevertAll()` flow.

**Tech Stack:** C++20, RenderVerseX `Scene` module, `ComponentFactory`, standalone validation executable `ResourceInstantiationValidation`.

---

### Task 1: Add Failing Validation Coverage

**Files:**
- Modify: `Tests/ResourceInstantiationValidation/main.cpp`

- [ ] **Step 1: Add a root actor component recreation test**

Add this test near the other `PrefabInstanceRevertAll...` tests:

```cpp
bool Test_PrefabInstanceRevertAllRecreatesMissingActorComponent()
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
    TEST_ASSERT_NOT_NULL(instance);
    auto* prefabInstance = instance->GetComponent<PrefabInstance>();
    TEST_ASSERT_NOT_NULL(prefabInstance);

    auto* actor = static_cast<Actor*>(instance);
    auto* component = FindPrefabPayloadComponentByName(*actor, "PrimaryPayload");
    TEST_ASSERT_NOT_NULL(component);
    TEST_ASSERT_TRUE(actor->RemoveComponent<PrefabPayloadComponent>());
    TEST_ASSERT_TRUE(FindPrefabPayloadComponentByName(*actor, "PrimaryPayload") == nullptr);

    prefabInstance->AddOverride(
        {"PrefabPayloadComponent",
         "serializedData",
         std::string("deleted"),
         "PrimaryPayload"});

    prefabInstance->RevertAll();

    auto* restored = FindPrefabPayloadComponentByName(*actor, "PrimaryPayload");
    TEST_ASSERT_NOT_NULL(restored);
    TEST_ASSERT_EQ(std::string("ammo=12;mode=burst"), restored->payload);
    TEST_ASSERT_TRUE(prefabInstance->GetOverrides().empty());

    sceneManager.Shutdown();
    return true;
}
```

- [ ] **Step 2: Add a legacy component recreation test**

Add this test beside the actor component recreation test:

```cpp
bool Test_PrefabInstanceRevertAllRecreatesMissingLegacyComponent()
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
    TEST_ASSERT_NOT_NULL(instance);
    auto* prefabInstance = instance->GetComponent<PrefabInstance>();
    TEST_ASSERT_NOT_NULL(prefabInstance);

    TEST_ASSERT_NOT_NULL(instance->GetComponent<PrefabLegacyPayloadComponent>());
    TEST_ASSERT_TRUE(instance->RemoveComponent<PrefabLegacyPayloadComponent>());
    TEST_ASSERT_TRUE(instance->GetComponent<PrefabLegacyPayloadComponent>() == nullptr);

    prefabInstance->AddOverride(
        {"PrefabLegacyPayloadComponent", "serializedData", std::string("deleted")});

    prefabInstance->RevertAll();

    auto* restored = instance->GetComponent<PrefabLegacyPayloadComponent>();
    TEST_ASSERT_NOT_NULL(restored);
    TEST_ASSERT_EQ(std::string("legacy=enabled"), restored->payload);
    TEST_ASSERT_TRUE(prefabInstance->GetOverrides().empty());

    sceneManager.Shutdown();
    return true;
}
```

- [ ] **Step 3: Add a child actor component recreation test**

Add this test to prove entity-path-scoped component restoration works on existing child entities:

```cpp
bool Test_PrefabInstanceRevertAllRecreatesMissingChildActorComponent()
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
    TEST_ASSERT_NOT_NULL(root);
    auto* prefabInstance = root->GetComponent<PrefabInstance>();
    TEST_ASSERT_NOT_NULL(prefabInstance);
    TEST_ASSERT_EQ(static_cast<size_t>(1), root->GetChildren().size());

    SceneEntity* child = root->GetChildren()[0];
    auto* childActor = static_cast<Actor*>(child);
    TEST_ASSERT_NOT_NULL(FindPrefabPayloadComponentByName(*childActor, "ChildPayload"));
    TEST_ASSERT_TRUE(childActor->RemoveComponent<PrefabPayloadComponent>());
    TEST_ASSERT_TRUE(FindPrefabPayloadComponentByName(*childActor, "ChildPayload") == nullptr);

    prefabInstance->AddOverride(
        {"PrefabPayloadComponent",
         "serializedData",
         std::string("deleted"),
         "ChildPayload",
         "Child"});

    prefabInstance->RevertAll();

    auto* restored = FindPrefabPayloadComponentByName(*childActor, "ChildPayload");
    TEST_ASSERT_NOT_NULL(restored);
    TEST_ASSERT_EQ(std::string("slot=child"), restored->payload);
    TEST_ASSERT_TRUE(prefabInstance->GetOverrides().empty());

    sceneManager.Shutdown();
    return true;
}
```

- [ ] **Step 4: Add a failed recreation test that keeps overrides**

Add this test after the missing actor component recreation test:

```cpp
bool Test_PrefabInstanceRevertAllKeepsOverrideWhenMissingActorComponentClassUnavailable()
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
    TEST_ASSERT_NOT_NULL(instance);
    auto* prefabInstance = instance->GetComponent<PrefabInstance>();
    TEST_ASSERT_NOT_NULL(prefabInstance);

    auto* actor = static_cast<Actor*>(instance);
    TEST_ASSERT_NOT_NULL(FindPrefabPayloadComponentByName(*actor, "PrimaryPayload"));
    TEST_ASSERT_TRUE(actor->RemoveComponent<PrefabPayloadComponent>());
    TEST_ASSERT_TRUE(FindPrefabPayloadComponentByName(*actor, "PrimaryPayload") == nullptr);

    ComponentFactory::ClearComponentClasses();
    prefabInstance->AddOverride(
        {"PrefabPayloadComponent",
         "serializedData",
         std::string("deleted"),
         "PrimaryPayload"});

    prefabInstance->RevertAll();

    TEST_ASSERT_TRUE(FindPrefabPayloadComponentByName(*actor, "PrimaryPayload") == nullptr);
    TEST_ASSERT_TRUE(prefabInstance->IsOverridden(
        "PrefabPayloadComponent", "serializedData", "PrimaryPayload"));
    TEST_ASSERT_EQ(static_cast<size_t>(1), prefabInstance->GetOverrides().size());

    sceneManager.Shutdown();
    return true;
}
```

- [ ] **Step 5: Register the tests**

Add these registrations near the other `RevertAll` registrations in `main()`:

```cpp
suite.AddTest("PrefabInstanceRevertAllRecreatesMissingActorComponent",
              Test_PrefabInstanceRevertAllRecreatesMissingActorComponent);
suite.AddTest("PrefabInstanceRevertAllKeepsOverrideWhenMissingActorComponentClassUnavailable",
              Test_PrefabInstanceRevertAllKeepsOverrideWhenMissingActorComponentClassUnavailable);
suite.AddTest("PrefabInstanceRevertAllRecreatesMissingLegacyComponent",
              Test_PrefabInstanceRevertAllRecreatesMissingLegacyComponent);
suite.AddTest("PrefabInstanceRevertAllRecreatesMissingChildActorComponent",
              Test_PrefabInstanceRevertAllRecreatesMissingChildActorComponent);
```

- [ ] **Step 6: Run the validation and confirm it fails**

Run:

```powershell
cmake --build build --config Debug --target ResourceInstantiationValidation
.\build\Tests\Debug\ResourceInstantiationValidation.exe
```

Expected: build succeeds; the new tests fail because `RevertAll()` currently applies payloads only to components that still exist.

---

### Task 2: Create Missing Prefab Components During RevertAll

**Files:**
- Modify: `Scene/Private/Prefab.cpp`

- [ ] **Step 1: Add name-binding cleanup for one component identity**

Add this helper beside `RemoveActorComponentNameBindingsForEntityPath()`:

```cpp
void RemoveActorComponentNameBindingForIdentity(
    std::vector<PrefabActorComponentNameBinding>& bindings,
    const std::string& className,
    const std::string& prefabName,
    size_t ordinal,
    const std::string& entityPath)
{
    const std::string normalizedPath = NormalizeEntityPath(entityPath);
    bindings.erase(
        std::remove_if(bindings.begin(), bindings.end(),
            [&](const PrefabActorComponentNameBinding& binding) {
                return binding.className == className &&
                       binding.prefabName == prefabName &&
                       binding.ordinal == ordinal &&
                       NormalizeEntityPath(binding.entityPath) == normalizedPath;
            }),
        bindings.end());
}
```

- [ ] **Step 2: Add reusable legacy component creation helper**

Add this helper near the component payload helpers:

```cpp
bool CreateLegacyPrefabComponent(SceneEntity& owner,
                                 const std::string& className,
                                 const std::string& serializedData)
{
    auto component = ComponentFactory::CreateComponentByClassName(className);
    if (!component)
    {
        return false;
    }

    auto* legacyComponent = dynamic_cast<Component*>(component.get());
    if (!legacyComponent)
    {
        return false;
    }

    legacyComponent->DeserializePrefabData(serializedData);
    component.release();

    std::unique_ptr<Component> ownedComponent(legacyComponent);
    return owner.AddOwnedComponent(std::move(ownedComponent)) != nullptr;
}
```

- [ ] **Step 3: Add reusable actor component creation helper**

Add this helper near `CreateLegacyPrefabComponent()`:

```cpp
ActorComponent* CreateActorPrefabComponent(
    SceneEntity& owner,
    const PrefabActorComponentData& componentData,
    std::vector<PrefabActorComponentNameBinding>* nameBindings,
    size_t ordinal,
    const std::string& entityPath)
{
    auto component = ComponentFactory::CreateComponentByClassName(componentData.className);
    if (!component)
    {
        return nullptr;
    }

    if (!componentData.name.empty())
    {
        component->SetName(componentData.name);
    }
    component->DeserializePrefabData(componentData.serializedData);

    auto* raw = component.get();
    ActorComponent* inserted = static_cast<Actor&>(owner).AddOwnedComponent(std::move(component));
    if (!inserted)
    {
        return nullptr;
    }

    if (nameBindings && !componentData.name.empty())
    {
        nameBindings->push_back(
            {componentData.className,
             componentData.name,
             ordinal,
             inserted->GetName(),
             NormalizeEntityPath(entityPath)});
    }

    if (auto* sceneComponent = dynamic_cast<SceneComponent*>(raw))
    {
        sceneComponent->AttachToComponent(static_cast<Actor&>(owner).GetRootComponent());
    }

    return inserted;
}
```

- [ ] **Step 4: Refactor `Prefab::CreateComponents()` to use the new helpers**

Replace direct component creation in `Prefab::CreateComponents()` with:

```cpp
for (const auto& [className, serializedData] : data.componentData)
{
    if (!CreateLegacyPrefabComponent(*entity, className, serializedData))
    {
        return false;
    }
}

std::unordered_map<std::string, size_t> actorComponentOrdinals;
for (const auto& componentData : data.actorComponentData)
{
    const size_t ordinal = actorComponentOrdinals[componentData.className]++;
    if (!CreateActorPrefabComponent(*entity, componentData, nameBindings, ordinal, entityPath))
    {
        return false;
    }
}
```

- [ ] **Step 5: Add missing component preflight helpers**

Add these helpers after the creation helpers. They perform all predictable failure checks before mutating the live entity, because actor component removal by pointer is intentionally private and rollback after partial insertion is not available through the public API.

```cpp
enum class PrefabComponentKind : uint8
{
    Legacy,
    Actor
};

bool CanCreatePrefabComponentClass(const std::string& className, PrefabComponentKind kind)
{
    auto component = ComponentFactory::CreateComponentByClassName(className);
    if (!component)
    {
        return false;
    }

    const bool isLegacy = dynamic_cast<Component*>(component.get()) != nullptr;
    return kind == PrefabComponentKind::Legacy ? isLegacy : !isLegacy;
}

bool CanCreateMissingPrefabComponents(SceneEntity& owner,
                                      const PrefabEntityData& data,
                                      const std::vector<PrefabActorComponentNameBinding>& nameBindings,
                                      const std::string& entityPath)
{
    for (const auto& [className, serializedData] : data.componentData)
    {
        (void)serializedData;
        if (!FindLegacyComponentByTypeName(owner, className) &&
            !CanCreatePrefabComponentClass(className, PrefabComponentKind::Legacy))
        {
            return false;
        }
    }

    std::unordered_map<std::string, size_t> actorComponentOrdinals;
    for (const auto& componentData : data.actorComponentData)
    {
        size_t& consumed = actorComponentOrdinals[componentData.className];
        const size_t ordinal = consumed++;

        if (!componentData.name.empty())
        {
            const PrefabActorComponentNameBinding* binding = FindActorComponentNameBinding(
                nameBindings, componentData.className, componentData.name, ordinal, entityPath);
            const std::string* targetName = binding ? &binding->instanceName : &componentData.name;
            if (targetName && !targetName->empty() &&
                FindActorComponentByClassNameAndName(owner, componentData.className, *targetName))
            {
                continue;
            }

            if (!CanCreatePrefabComponentClass(componentData.className, PrefabComponentKind::Actor))
            {
                return false;
            }
            continue;
        }

        auto matches = FindActorComponentsByClassName(owner, componentData.className);
        if (ordinal < matches.size())
        {
            continue;
        }

        if (!CanCreatePrefabComponentClass(componentData.className, PrefabComponentKind::Actor))
        {
            return false;
        }
    }

    return true;
}
```

- [ ] **Step 6: Add a restore-wide preflight**

Add this helper after `CanCreateMissingPrefabComponents()`. It checks all currently resolvable live prefab entities before `RestoreHierarchyStateTo()` mutates properties, creates components, or updates name bindings. This prevents a later entity failure from discarding updated bindings for components already recreated earlier in the same revert.

```cpp
bool CanRestoreExistingEntityMissingComponents(
    SceneEntity& rootEntity,
    const std::vector<PrefabEntityData>& entities,
    const std::vector<std::string>& entityPaths,
    const std::vector<PrefabActorComponentNameBinding>& nameBindings)
{
    for (size_t i = 0; i < entities.size(); ++i)
    {
        SceneEntity* entity = i == 0
            ? &rootEntity
            : FindLiveEntityByPath(rootEntity, i < entityPaths.size() ? entityPaths[i] : std::string());
        if (!entity)
        {
            continue;
        }

        const std::string entityPath = i < entityPaths.size() ? entityPaths[i] : std::string();
        if (!CanCreateMissingPrefabComponents(*entity, entities[i], nameBindings, entityPath))
        {
            return false;
        }
    }

    return true;
}
```

At the start of `Prefab::RestoreHierarchyStateTo()`, immediately after `entityPaths` is built, add:

```cpp
if (!CanRestoreExistingEntityMissingComponents(rootEntity, m_entities, entityPaths, nameBindings))
{
    return false;
}
```

- [ ] **Step 7: Add `EnsurePrefabEntityComponents()`**

Add this helper after the creation helpers:

```cpp
bool EnsurePrefabEntityComponents(SceneEntity& owner,
                                  const PrefabEntityData& data,
                                  std::vector<PrefabActorComponentNameBinding>& nameBindings,
                                  const std::string& entityPath)
{
    if (!CanCreateMissingPrefabComponents(owner, data, nameBindings, entityPath))
    {
        return false;
    }

    for (const auto& [className, serializedData] : data.componentData)
    {
        if (!FindLegacyComponentByTypeName(owner, className) &&
            !CreateLegacyPrefabComponent(owner, className, serializedData))
        {
            return false;
        }
    }

    std::unordered_map<std::string, size_t> actorComponentOrdinals;
    for (const auto& componentData : data.actorComponentData)
    {
        size_t& consumed = actorComponentOrdinals[componentData.className];
        const size_t ordinal = consumed++;

        if (!componentData.name.empty())
        {
            const PrefabActorComponentNameBinding* binding = FindActorComponentNameBinding(
                nameBindings, componentData.className, componentData.name, ordinal, entityPath);
            const std::string* targetName = binding ? &binding->instanceName : &componentData.name;
            if (targetName && !targetName->empty() &&
                FindActorComponentByClassNameAndName(owner, componentData.className, *targetName))
            {
                continue;
            }

            std::vector<PrefabActorComponentNameBinding> bindingsBeforeCreate = nameBindings;
            RemoveActorComponentNameBindingForIdentity(
                nameBindings, componentData.className, componentData.name, ordinal, entityPath);
            if (!CreateActorPrefabComponent(owner, componentData, &nameBindings, ordinal, entityPath))
            {
                nameBindings = std::move(bindingsBeforeCreate);
                return false;
            }
            continue;
        }

        auto matches = FindActorComponentsByClassName(owner, componentData.className);
        if (ordinal < matches.size())
        {
            continue;
        }

        if (!CreateActorPrefabComponent(owner, componentData, nullptr, ordinal, entityPath))
        {
            return false;
        }
    }

    return true;
}
```

- [ ] **Step 8: Call the ensure helper from `RestoreHierarchyStateTo()`**

In `Prefab::RestoreHierarchyStateTo()`, track whether the entity was created during this call. Define this flag inside the entity loop, immediately after `SceneEntity* entity = nullptr;`, so it resets for every prefab entity:

```cpp
bool createdEntityThisCall = false;
```

Set it immediately after successful `SpawnPrefabEntity(...)`:

```cpp
createdEntityThisCall = true;
```

Then, after `restoredEntities[i] = entity;` and before `ApplyAllPrefabEntityProperties(...)`, add:

```cpp
if (!EnsurePrefabEntityComponents(*entity, entityData, nameBindings, entityPath))
{
    if (createdEntityThisCall)
    {
        sceneManager->DestroyEntity(entity->GetHandle());
    }
    return false;
}
```

Expected behavior: existing live entities keep existing prefab components; deleted prefab components are recreated and payloads are applied; unsupported missing component classes abort `RevertAll()` before clearing overrides. A newly spawned missing entity is destroyed if a later component-ensure invariant unexpectedly fails.

- [ ] **Step 9: Run the validation and confirm it passes**

Run:

```powershell
cmake --build build --config Debug --target ResourceInstantiationValidation
.\build\Tests\Debug\ResourceInstantiationValidation.exe
```

Expected: all `ResourceInstantiationValidation` tests pass.

---

### Task 3: Review, Broader Verification, and Commit

**Files:**
- Review: `Scene/Private/Prefab.cpp`
- Review: `Tests/ResourceInstantiationValidation/main.cpp`
- Modify: this plan checklist

- [ ] **Step 1: Request one code review**

Use one review subagent with:

```text
Description: Phase 29 makes PrefabInstance::RevertAll recreate missing prefab-owned components on existing entities.
Requirements: RevertAll should recreate missing legacy components, named actor components on the root, and named actor components on child entities without duplicating existing components or breaking runtime name bindings.
Base SHA: commit before Task 1 implementation
Head SHA: current working tree or implementation commit
```

Expected: reviewer reports no Critical or Important issues before proceeding.

- [ ] **Step 2: Fix Critical or Important review findings**

Apply only review findings that are technically valid. If a reviewer finding is wrong, document the reason in the implementation notes and keep the code unchanged.

- [ ] **Step 3: Run required verification**

Run:

```powershell
git diff --check
cmake --build build --config Debug --target ActorComponentValidation
cmake --build build --config Debug --target ResourceInstantiationValidation
cmake --build build --config Debug --target RenderSceneValidation
cmake --build build --config Debug --target ModelViewer
.\build\Tests\Debug\ActorComponentValidation.exe
.\build\Tests\Debug\ResourceInstantiationValidation.exe
.\build\Tests\Debug\RenderSceneValidation.exe
```

Expected: `git diff --check` exits 0; all three validation executables report 0 failed tests; `ModelViewer` target builds.

- [ ] **Step 4: Commit the phase**

Run:

```powershell
git add Docs/superpowers/plans/2026-05-17-ue-style-actor-component-phase29-prefab-revert-missing-components.md Scene/Private/Prefab.cpp Tests/ResourceInstantiationValidation/main.cpp
git commit -m "Restore missing prefab components on revert"
```

Expected: commit succeeds and only tracked Phase 29 files are included.

---

## Plan Self-Review

**Spec coverage:** This plan covers the next UE-style inherited component gap left after structural child restoration: missing prefab-owned components on existing root and child entities. It intentionally keeps `RevertProperty()` missing-component behavior unchanged because existing tests define it as preserving the override when the component is absent. The plan also handles failed missing component recreation by preflighting live entities before mutation and aborting `RevertAll()` before override clearing.

**Placeholder scan:** No placeholder steps remain. Every code-changing step names the target function and includes concrete code.

**Type consistency:** Helper signatures use existing `SceneEntity`, `Actor`, `ActorComponent`, `Component`, `PrefabEntityData`, `PrefabActorComponentData`, and `PrefabActorComponentNameBinding` types. The tests reuse existing `PrefabPayloadComponent`, `PrefabLegacyPayloadComponent`, and `FindPrefabPayloadComponentByName` helpers.
