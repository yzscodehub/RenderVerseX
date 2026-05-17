# UE-Style Actor Component Phase 32: Prefab Revert Restores Bound Entity Parents Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make `PrefabInstance::RevertAll()` restore the prefab parent of inherited child entities that were reparented under extra live nodes.

**Architecture:** Phase 30 added stable runtime bindings from prefab entity ids to live `SceneEntity::Handle` values, and Phase 31 prunes pure-extra child subtrees after hierarchy restore. This phase uses the resolved prefab parent from `PrefabEntityData::parentIndex` during `RestoreHierarchyStateTo()` and reparents already-bound live entities back to their prefab parent before applying properties and pruning extras. Reparenting is conservative: it only runs when the prefab parent has already been restored and avoids cycles or moving the root entity.

**Tech Stack:** C++20, RenderVerseX Scene module, standalone `ResourceInstantiationValidation` executable.

---

## File Structure

- Modify `Scene/Private/Prefab.cpp`
  - Add a helper that restores an inherited live entity's prefab parent when it is safe.
  - Call that helper inside `Prefab::RestoreHierarchyStateTo()` after resolving or recreating each non-root entity.

- Modify `Tests/ResourceInstantiationValidation/main.cpp`
  - Add a simple root-child reparent regression.
  - Add a nested parent-index regression.

- Modify this plan file while executing checkboxes.

---

## Task 1: Add RED Coverage For Bound Entity Reparent Restore

**Files:**
- Modify: `Tests/ResourceInstantiationValidation/main.cpp`

- [x] **Step 1: Add a simple reparent regression**

Add this test near the existing prefab `RevertAll()` hierarchy tests:

```cpp
bool Test_PrefabInstanceRevertAllRestoresReparentedBoundChildAndPrunesExtraParent()
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
    TEST_ASSERT_NOT_NULL(root);
    auto* prefabInstance = root->GetComponent<PrefabInstance>();
    TEST_ASSERT_NOT_NULL(prefabInstance);
    TEST_ASSERT_EQ(static_cast<size_t>(1), root->GetChildren().size());

    SceneEntity* child = root->GetChildren()[0];
    const SceneEntity::Handle childHandle = child->GetHandle();

    ActorSpawnParams extraParams;
    extraParams.name = "ExtraParent";
    extraParams.parent = root;
    SceneEntity* extraParent = sceneManager.SpawnActor(extraParams);
    TEST_ASSERT_NOT_NULL(extraParent);
    const SceneEntity::Handle extraParentHandle = extraParent->GetHandle();

    child->SetParent(extraParent);
    child->SetPosition(Vec3(9.0f, 9.0f, 9.0f));
    prefabInstance->AddOverride(
        {"SceneEntity", "position", child->GetPosition(), "", "Child"});

    prefabInstance->RevertAll();

    SceneEntity* restoredChild = sceneManager.GetEntity(childHandle);
    TEST_ASSERT_NOT_NULL(restoredChild);
    TEST_ASSERT_EQ(root, restoredChild->GetParent());
    TEST_ASSERT_EQ(static_cast<size_t>(1), root->GetChildren().size());
    TEST_ASSERT_EQ(restoredChild, root->GetChildren()[0]);
    TEST_ASSERT_TRUE(sceneManager.GetEntity(extraParentHandle) == nullptr);
    TEST_ASSERT_EQ(Vec3(1.0f, 2.0f, 3.0f), restoredChild->GetPosition());
    TEST_ASSERT_TRUE(prefabInstance->GetOverrides().empty());

    sceneManager.Shutdown();
    return true;
}
```

- [x] **Step 2: Add a nested parent-index regression**

Add this test after the simple reparent test:

```cpp
bool Test_PrefabInstanceRevertAllRestoresReparentedNestedBoundChild()
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
    TEST_ASSERT_NOT_NULL(root);
    auto* prefabInstance = root->GetComponent<PrefabInstance>();
    TEST_ASSERT_NOT_NULL(prefabInstance);
    TEST_ASSERT_EQ(static_cast<size_t>(1), root->GetChildren().size());

    SceneEntity* branch = root->GetChildren()[0];
    TEST_ASSERT_EQ(static_cast<size_t>(1), branch->GetChildren().size());
    SceneEntity* leaf = branch->GetChildren()[0];
    const SceneEntity::Handle leafHandle = leaf->GetHandle();

    ActorSpawnParams extraParams;
    extraParams.name = "ExtraNestedParent";
    extraParams.parent = root;
    SceneEntity* extraParent = sceneManager.SpawnActor(extraParams);
    TEST_ASSERT_NOT_NULL(extraParent);
    const SceneEntity::Handle extraParentHandle = extraParent->GetHandle();

    leaf->SetParent(extraParent);
    leaf->SetScale(Vec3(9.0f, 9.0f, 9.0f));

    prefabInstance->RevertAll();

    SceneEntity* restoredLeaf = sceneManager.GetEntity(leafHandle);
    TEST_ASSERT_NOT_NULL(restoredLeaf);
    TEST_ASSERT_EQ(branch, restoredLeaf->GetParent());
    TEST_ASSERT_EQ(static_cast<size_t>(1), branch->GetChildren().size());
    TEST_ASSERT_EQ(restoredLeaf, branch->GetChildren()[0]);
    TEST_ASSERT_TRUE(sceneManager.GetEntity(extraParentHandle) == nullptr);
    TEST_ASSERT_EQ(Vec3(2.0f, 3.0f, 4.0f), restoredLeaf->GetScale());

    sceneManager.Shutdown();
    return true;
}
```

- [x] **Step 3: Register the two tests**

Register them near the other prefab revert-all hierarchy tests:

```cpp
suite.AddTest("PrefabInstanceRevertAllRestoresReparentedBoundChildAndPrunesExtraParent",
              Test_PrefabInstanceRevertAllRestoresReparentedBoundChildAndPrunesExtraParent);
suite.AddTest("PrefabInstanceRevertAllRestoresReparentedNestedBoundChild",
              Test_PrefabInstanceRevertAllRestoresReparentedNestedBoundChild);
```

- [x] **Step 4: Build and run RED validation**

Run:

```powershell
cmake --build build\Debug --config Debug --target ResourceInstantiationValidation
.\build\Debug\Tests\Debug\ResourceInstantiationValidation.exe
```

Expected before implementation: the two new tests fail because bound entities remain under their extra parent and Phase 31 preserves that extra subtree to avoid deleting a prefab-bound entity.

---

## Task 2: Restore Prefab Parent For Bound Live Entities

**Files:**
- Modify: `Scene/Private/Prefab.cpp`

- [x] **Step 1: Add a conservative reparent helper**

Add this helper in the anonymous namespace near the other prefab hierarchy helpers:

```cpp
void RestorePrefabEntityParent(SceneEntity& rootEntity,
                               SceneEntity& entity,
                               SceneEntity* prefabParent)
{
    if (!prefabParent || &entity == &rootEntity || &entity == prefabParent)
    {
        return;
    }

    if (!IsEntityInsidePrefabRoot(rootEntity, entity) ||
        !IsEntityInsidePrefabRoot(rootEntity, *prefabParent))
    {
        return;
    }

    if (prefabParent->IsDescendantOf(&entity))
    {
        return;
    }

    if (entity.GetParent() != prefabParent)
    {
        entity.SetParent(prefabParent);
    }
}
```

This delegates the actual attach/detach work to `SceneEntity::SetParent()`, which already updates component attachment and transform dirtiness.

- [x] **Step 2: Call the helper after resolving each non-root entity**

Inside `Prefab::RestoreHierarchyStateTo()`, after the `if (!entity) { continue; }` block and before `restoredEntities[i] = entity;`, add:

```cpp
if (i > 0)
{
    const int32_t parentIndex = entityData.parentIndex;
    if (parentIndex >= 0 &&
        parentIndex < static_cast<int32_t>(restoredEntities.size()))
    {
        SceneEntity* prefabParent = restoredEntities[static_cast<size_t>(parentIndex)];
        RestorePrefabEntityParent(rootEntity, *entity, prefabParent);
    }
}
```

The parent is guaranteed to be earlier in valid prefab data, but the null check is still important for partially restored or malformed data.

- [x] **Step 3: Build and run targeted validation**

Run:

```powershell
cmake --build build\Debug --config Debug --target ResourceInstantiationValidation
.\build\Debug\Tests\Debug\ResourceInstantiationValidation.exe
```

Expected after implementation: all `ResourceInstantiationValidation` tests pass, including the two new reparent regressions.

---

## Task 3: Validate, Review, And Commit

**Files:**
- Modify: `Docs/superpowers/plans/2026-05-17-ue-style-actor-component-phase32-prefab-revert-reparent.md`

- [x] **Step 1: Run adjacent validation**

Run:

```powershell
cmake --build build\Debug --config Debug --target ActorComponentValidation
.\build\Debug\Tests\Debug\ActorComponentValidation.exe
cmake --build build\Debug --config Debug --target RenderSceneValidation
.\build\Debug\Tests\Debug\RenderSceneValidation.exe
```

Expected: all adjacent scene/component validations pass.

- [x] **Step 2: Run whitespace check**

Run:

```powershell
git diff --check
```

Expected: exit code `0`; LF-to-CRLF warnings are acceptable.

- [x] **Step 3: Request one code review agent**

Ask one `gpt-5.3-codex-spark` review agent to review Critical/Important issues only. If the agent times out, close it and perform a local Critical/Important review before committing.

- [ ] **Step 4: Commit**

Run:

```powershell
git add Scene/Private/Prefab.cpp Tests/ResourceInstantiationValidation/main.cpp Docs/superpowers/plans/2026-05-17-ue-style-actor-component-phase32-prefab-revert-reparent.md
git commit -m "Restore prefab child parents on revert"
```

---

## Plan Self-Review

- **Spec coverage:** The plan closes the Phase 31 deferred reparent case by restoring bound inherited entities to their prefab parent before pruning extra subtrees.
- **Safety:** The helper refuses to move the root, refuses null/foreign parents, and refuses cycle-forming moves. If a parent cannot be restored, Phase 31's all-entities-restored guard prevents destructive pruning.
- **Placeholder scan:** No placeholder implementation steps remain.
- **Type consistency:** The plan uses existing `SceneEntity::SetParent()`, `SceneEntity::IsDescendantOf()`, `SceneEntity::GetParent()`, `SceneManager::SpawnActor()`, and `Prefab::RestoreHierarchyStateTo()` APIs.
