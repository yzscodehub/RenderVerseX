# UE-Style Actor Component Phase 28 Prefab Revert Structural Restore Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make `PrefabInstance::RevertAll()` restore missing prefab child entities instead of only applying state to entities that already exist.

**Architecture:** Add a prefab-owned restore helper that walks serialized prefab entity data in parent-before-child order. It reuses existing live entities by path, creates missing serialized descendants under their restored parents, applies prefab entity state/component payloads, and leaves extra live children untouched until a later stable-identity phase can delete them safely.

**Tech Stack:** C++20, RenderVerseX Scene module, standalone validation executable `ResourceInstantiationValidation`.

---

## Scope

- Extend `RevertAll()` only; do not change `RevertProperty(...)` in this phase.
- Recreate serialized prefab child entities that are missing from the live instance.
- Recreate nested missing descendants when their serialized parent exists or was recreated earlier in the same pass.
- Create recreated entities through `SceneManager::SpawnActor(...)` / `SpawnActorByClassName(...)` so lifecycle and scene ownership remain normal.
- Apply prefab transform/layer/active state and serialized component payloads to both existing and recreated entities.
- Create components for newly recreated entities using the existing prefab component creation path.
- Preserve existing extra live children that are not represented by the prefab. Deleting extras is deferred until stable prefab entity identity is introduced.
- Avoid creating another child when an unqualified unique prefab path is ambiguous in the live hierarchy because multiple same-name live children already exist.
- Keep component creation for already-existing entities out of scope; missing components on existing entities are still not recreated in this phase.
- Preserve existing `PrefabInstance` runtime component name bindings when new missing children are recreated. New bindings from recreated entities are merged into the existing binding list instead of replacing it.

## Files

- Modify: `E:\WorkSpace\RenderVerseX\Scene\Include\Scene\Prefab.h`
- Modify: `E:\WorkSpace\RenderVerseX\Scene\Private\Prefab.cpp`
- Modify: `E:\WorkSpace\RenderVerseX\Tests\ResourceInstantiationValidation\main.cpp`
- Modify: `E:\WorkSpace\RenderVerseX\Docs\superpowers\plans\2026-05-17-ue-style-actor-component-phase28-prefab-revert-structural-restore.md`

---

## Task 1: Add Minimal Failing Structural Revert Coverage

- [x] **Step 1: Add missing child recreation test**

In `Tests\ResourceInstantiationValidation\main.cpp`, add this test near the existing `RevertAll` tests:

```cpp
bool Test_PrefabInstanceRevertAllRecreatesMissingChildEntity()
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
    TEST_ASSERT_NOT_NULL(root);
    auto* prefabInstance = root->GetComponent<PrefabInstance>();
    TEST_ASSERT_NOT_NULL(prefabInstance);
    TEST_ASSERT_EQ(static_cast<size_t>(1), root->GetChildren().size());

    SceneEntity* child = root->GetChildren()[0];
    sceneManager.DestroyEntity(child->GetHandle());
    TEST_ASSERT_TRUE(root->GetChildren().empty());

    prefabInstance->AddOverride(
        {"SceneEntity", "position", Vec3(9.0f, 9.0f, 9.0f), "", "Child"});

    prefabInstance->RevertAll();

    TEST_ASSERT_EQ(static_cast<size_t>(1), root->GetChildren().size());
    SceneEntity* recreated = root->GetChildren()[0];
    TEST_ASSERT_NOT_NULL(recreated);
    TEST_ASSERT_EQ(std::string("Child"), recreated->GetName());
    TEST_ASSERT_EQ(root, recreated->GetParent());
    TEST_ASSERT_EQ(Vec3(1.0f, 2.0f, 3.0f), recreated->GetPosition());

    auto* childPayload = FindPrefabPayloadComponentByName(
        *static_cast<Actor*>(recreated), "ChildPayload");
    TEST_ASSERT_NOT_NULL(childPayload);
    TEST_ASSERT_EQ(std::string("slot=child"), childPayload->payload);
    TEST_ASSERT_TRUE(prefabInstance->GetOverrides().empty());

    sceneManager.Shutdown();
    return true;
}
```

- [x] **Step 2: Add nested missing descendant recreation test**

Add this test near the previous one:

```cpp
bool Test_PrefabInstanceRevertAllRecreatesNestedMissingDescendant()
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
    TEST_ASSERT_NOT_NULL(root);
    auto* prefabInstance = root->GetComponent<PrefabInstance>();
    TEST_ASSERT_NOT_NULL(prefabInstance);
    TEST_ASSERT_EQ(static_cast<size_t>(1), root->GetChildren().size());
    SceneEntity* branch = root->GetChildren()[0];
    TEST_ASSERT_EQ(static_cast<size_t>(1), branch->GetChildren().size());

    SceneEntity* leaf = branch->GetChildren()[0];
    sceneManager.DestroyEntity(leaf->GetHandle());
    TEST_ASSERT_TRUE(branch->GetChildren().empty());

    prefabInstance->RevertAll();

    TEST_ASSERT_EQ(static_cast<size_t>(1), branch->GetChildren().size());
    SceneEntity* recreatedLeaf = branch->GetChildren()[0];
    TEST_ASSERT_NOT_NULL(recreatedLeaf);
    TEST_ASSERT_EQ(std::string("Leaf"), recreatedLeaf->GetName());
    TEST_ASSERT_EQ(branch, recreatedLeaf->GetParent());
    TEST_ASSERT_EQ(Vec3(2.0f, 3.0f, 4.0f), recreatedLeaf->GetScale());

    sceneManager.Shutdown();
    return true;
}
```

- [x] **Step 3: Add extra live child preservation test**

Add a guard that `RevertAll()` does not delete live children that are not part of the prefab:

```cpp
bool Test_PrefabInstanceRevertAllPreservesExtraLiveChildEntity()
{
    PrefabEntityData rootData;
    rootData.name = "PreserveExtraRoot";

    auto prefab = Prefab::CreateFromData({rootData});

    SceneManager sceneManager;
    sceneManager.Initialize();

    SceneEntity* root = prefab->Instantiate(sceneManager);
    TEST_ASSERT_NOT_NULL(root);
    auto* prefabInstance = root->GetComponent<PrefabInstance>();
    TEST_ASSERT_NOT_NULL(prefabInstance);

    ActorSpawnParams extraParams;
    extraParams.name = "ExtraChild";
    extraParams.parent = root;
    SceneEntity* extra = sceneManager.SpawnActor(extraParams);
    TEST_ASSERT_NOT_NULL(extra);

    prefabInstance->RevertAll();

    TEST_ASSERT_EQ(static_cast<size_t>(1), root->GetChildren().size());
    TEST_ASSERT_EQ(extra, root->GetChildren()[0]);
    TEST_ASSERT_EQ(root, extra->GetParent());

    sceneManager.Shutdown();
    return true;
}
```

- [x] **Step 4: Add ambiguous same-name live child no-create guard**

Add this test to avoid duplicating a unique prefab child path when the live hierarchy already has ambiguous same-name children:

```cpp
bool Test_PrefabInstanceRevertAllDoesNotCreateWhenUniquePathIsAmbiguous()
{
    PrefabEntityData rootData;
    rootData.name = "AmbiguousRestoreRoot";

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
    TEST_ASSERT_EQ(static_cast<size_t>(1), root->GetChildren().size());

    ActorSpawnParams duplicateParams;
    duplicateParams.name = "Child";
    duplicateParams.parent = root;
    SceneEntity* duplicate = sceneManager.SpawnActor(duplicateParams);
    TEST_ASSERT_NOT_NULL(duplicate);
    TEST_ASSERT_EQ(static_cast<size_t>(2), root->GetChildren().size());

    prefabInstance->RevertAll();

    TEST_ASSERT_EQ(static_cast<size_t>(2), root->GetChildren().size());

    sceneManager.Shutdown();
    return true;
}
```

- [x] **Step 5: Add binding preservation regression test**

Add this test near the previous `RevertAll` tests. It proves that recreating a
missing child does not replace existing runtime name bindings needed by another
existing entity:

```cpp
bool Test_PrefabInstanceRevertAllKeepsExistingNameBindingsWhenRecreatingChild()
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
    TEST_ASSERT_NOT_NULL(root);
    auto* prefabInstance = root->GetComponent<PrefabInstance>();
    TEST_ASSERT_NOT_NULL(prefabInstance);

    auto* rootPrefabPayload = FindPrefabPayloadComponentByName(
        *static_cast<Actor*>(root), "PrimaryPayload_1");
    TEST_ASSERT_NOT_NULL(rootPrefabPayload);
    rootPrefabPayload->payload = "dirty-root";

    TEST_ASSERT_EQ(static_cast<size_t>(1), root->GetChildren().size());
    SceneEntity* child = root->GetChildren()[0];
    sceneManager.DestroyEntity(child->GetHandle());
    TEST_ASSERT_TRUE(root->GetChildren().empty());

    prefabInstance->RevertAll();

    rootPrefabPayload->payload = "dirty-root-again";
    prefabInstance->RevertProperty(
        "PrefabPayloadComponent", "serializedData", "PrimaryPayload_1");

    TEST_ASSERT_EQ(std::string("slot=root-prefab"), rootPrefabPayload->payload);
    TEST_ASSERT_EQ(static_cast<size_t>(1), root->GetChildren().size());

    sceneManager.Shutdown();
    ActorFactory::ClearAll();
    return true;
}
```

- [x] **Step 6: Add stale recreated-child binding regression test**

Add a test that first instantiates a child actor whose constructor forces the
prefab component runtime name to be uniquified, deletes that child, re-registers
the same actor class without the collision, then calls `RevertAll()`. After the
child is recreated, add an extra component using the stale runtime name and call
`RevertProperty(...)` for that stale name. The extra component must not receive
the prefab payload, and the override must remain because the stale binding should
have been dropped when the child was recreated.

- [x] **Step 7: Register the tests**

Register the six tests near the existing `RevertAll` group:

```cpp
suite.AddTest("PrefabInstanceRevertAllRecreatesMissingChildEntity",
              Test_PrefabInstanceRevertAllRecreatesMissingChildEntity);
suite.AddTest("PrefabInstanceRevertAllRecreatesNestedMissingDescendant",
              Test_PrefabInstanceRevertAllRecreatesNestedMissingDescendant);
suite.AddTest("PrefabInstanceRevertAllPreservesExtraLiveChildEntity",
              Test_PrefabInstanceRevertAllPreservesExtraLiveChildEntity);
suite.AddTest("PrefabInstanceRevertAllDoesNotCreateWhenUniquePathIsAmbiguous",
              Test_PrefabInstanceRevertAllDoesNotCreateWhenUniquePathIsAmbiguous);
suite.AddTest("PrefabInstanceRevertAllKeepsExistingNameBindingsWhenRecreatingChild",
              Test_PrefabInstanceRevertAllKeepsExistingNameBindingsWhenRecreatingChild);
suite.AddTest("PrefabInstanceRevertAllDropsStaleBindingsForRecreatedChild",
              Test_PrefabInstanceRevertAllDropsStaleBindingsForRecreatedChild);
```

- [x] **Step 8: Confirm RED**

Run:

```powershell
cmake --build build\Debug --config Debug --target ResourceInstantiationValidation
.\build\Debug\Tests\Debug\ResourceInstantiationValidation.exe
```

Expected before implementation: the build succeeds, and the missing child / nested missing descendant tests fail because `RevertAll()` skips missing live entities.

---

## Task 2: Add Prefab Restore Helper API

- [x] **Step 1: Declare restore operation**

In `Scene\Include\Scene\Prefab.h`, add a public method declaration immediately
after `RebuildHierarchyStateFrom(...)`:

```cpp
/// Restore serialized prefab hierarchy state into an existing root entity.
bool RestoreHierarchyStateTo(SceneEntity& rootEntity,
                             std::vector<PrefabActorComponentNameBinding>& nameBindings) const;
```

This method is public because `PrefabInstance` owns the operation entry point but
does not have access to `Prefab` private members. Keep `CreateComponents(...)`
private; the restore operation internally uses it.

- [x] **Step 2: Add child-name counting helper**

In `Scene\Private\Prefab.cpp`, add this helper in the anonymous namespace near `FindLiveEntityByPath(...)`:

```cpp
size_t CountChildrenNamed(const SceneEntity& parent, const std::string& name)
{
    size_t count = 0;
    for (const SceneEntity* child : parent.GetChildren())
    {
        if (child && child->GetName() == name)
        {
            ++count;
        }
    }
    return count;
}
```

- [x] **Step 3: Add ambiguity-safe creation helper**

Add this helper near `CountChildrenNamed(...)`:

```cpp
bool CanCreateMissingEntityForPath(const SceneEntity& parent,
                                   const std::string& entityPath,
                                   const PrefabEntityData& entityData)
{
    const std::vector<std::string> segments = SplitEntityPath(entityPath);
    if (segments.empty())
    {
        return false;
    }

    const EntityPathSegment segment = ParseEntityPathSegment(segments.back());
    if (!segment.valid)
    {
        return false;
    }

    if (segment.hasOrdinal)
    {
        return true;
    }

    return CountChildrenNamed(parent, entityData.name) == 0;
}
```

This preserves extra same-name live children instead of creating a third child when a unique prefab path became ambiguous.

- [x] **Step 4: Add actor creation helper**

Add this helper in the anonymous namespace after `CanCreateMissingEntityForPath(...)`:

```cpp
SceneEntity* SpawnPrefabEntity(SceneManager& sceneManager,
                               const PrefabEntityData& entityData,
                               SceneEntity* parent)
{
    ActorSpawnParams params;
    params.name = entityData.name;
    params.parent = parent;

    if (entityData.actorClassName.empty() || entityData.actorClassName == "SceneEntity")
    {
        return sceneManager.SpawnActor(params);
    }

    return sceneManager.SpawnActorByClassName(entityData.actorClassName, params);
}
```

- [x] **Step 5: Add entity-path binding cleanup helper**

Add a helper that removes existing `PrefabActorComponentNameBinding` entries for
an entity path. This is used only when the entity was missing and is being
recreated, so old bindings for the deleted entity cannot shadow the fresh
bindings produced by `CreateComponents(...)`.

---

## Task 3: Implement Structural Revert Restore

- [x] **Step 1: Implement `Prefab::RestoreHierarchyStateTo(...)`**

In `Scene\Private\Prefab.cpp`, add this method after `Prefab::InstantiateInternal(...)`:

```cpp
bool Prefab::RestoreHierarchyStateTo(
    SceneEntity& rootEntity,
    std::vector<PrefabActorComponentNameBinding>& nameBindings) const
{
    SceneManager* sceneManager = rootEntity.GetSceneManager();
    if (!sceneManager || m_entities.empty())
    {
        return false;
    }

    const std::vector<std::string> entityPaths = BuildPrefabEntityPaths(*this);
    std::vector<SceneEntity*> restoredEntities(m_entities.size(), nullptr);
    restoredEntities[0] = &rootEntity;

    for (size_t i = 0; i < m_entities.size(); ++i)
    {
        const PrefabEntityData& entityData = m_entities[i];
        const std::string entityPath = i < entityPaths.size() ? entityPaths[i] : std::string();
        SceneEntity* entity = nullptr;

        if (i == 0)
        {
            entity = &rootEntity;
        }
        else
        {
            entity = FindLiveEntityByPath(rootEntity, entityPath);
            if (!entity)
            {
                const int32 parentIndex = entityData.parentIndex;
                if (parentIndex < 0 ||
                    parentIndex >= static_cast<int32>(restoredEntities.size()))
                {
                    continue;
                }

                SceneEntity* parent = restoredEntities[static_cast<size_t>(parentIndex)];
                if (!parent || !CanCreateMissingEntityForPath(*parent, entityPath, entityData))
                {
                    continue;
                }

                entity = SpawnPrefabEntity(*sceneManager, entityData, parent);
                if (!entity)
                {
                    continue;
                }

                const std::string normalizedPath = NormalizeEntityPath(entityPath);
                RemoveActorComponentNameBindingsForEntityPath(nameBindings, normalizedPath);
                const size_t bindingCountBeforeCreate = nameBindings.size();
                if (!CreateComponents(entity, entityData, &nameBindings, normalizedPath))
                {
                    nameBindings.resize(bindingCountBeforeCreate);
                    sceneManager->DestroyEntity(entity->GetHandle());
                    continue;
                }
            }
        }

        if (!entity)
        {
            continue;
        }

        restoredEntities[i] = entity;
        ApplyAllPrefabEntityProperties(entity, entityData);
        ApplyPrefabEntityComponentPayloads(
            *entity, entityData, nameBindings, entityPath);
    }

    return true;
}
```

- [x] **Step 2: Use the restore helper from `PrefabInstance::RevertAll()`**

Replace the current loop in `PrefabInstance::RevertAll()` with:

```cpp
std::vector<PrefabActorComponentNameBinding> restoredBindings = m_componentNameBindings;
if (m_prefab->RestoreHierarchyStateTo(*owner, restoredBindings))
{
    m_componentNameBindings = std::move(restoredBindings);
    m_overrides.clear();
}
```

Keep the existing early returns for null prefab/root owner. Start the restore
helper with a copy of the existing runtime name bindings so existing live
entities can be restored during the same `RevertAll()` call. The restore helper
may append new bindings for recreated entities, then the merged result becomes
the instance binding list.

---

## Task 4: Validate, Review, And Commit

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
git add Docs\superpowers\plans\2026-05-17-ue-style-actor-component-phase28-prefab-revert-structural-restore.md Scene\Include\Scene\Prefab.h Scene\Private\Prefab.cpp Tests\ResourceInstantiationValidation\main.cpp
git commit -m "Restore missing prefab children on revert"
```

---

## Self-Review

- Spec coverage: The plan covers missing child recreation, nested descendant recreation, extra live child preservation, ambiguity-safe no-create behavior, and preserving existing runtime name bindings while merging recreated entity bindings.
- Compatibility: Existing `RevertAll()` property/payload behavior remains, but missing serialized descendants can now be recreated. `RevertProperty(...)` remains unchanged.
- Risk management: Extra live child deletion is explicitly deferred until stable prefab entity identity exists. Existing live entities do not get missing components created in this phase, keeping behavior narrow.
- Placeholder scan: No TBD/TODO placeholders.
