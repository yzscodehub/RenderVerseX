# UE-Style Actor Component Phase 31: Prefab Revert Prunes Extra Child Entities Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make `PrefabInstance::RevertAll()` remove live child entities that were added to an instance but are not part of the prefab hierarchy.

**Architecture:** Phase 30 introduced runtime bindings from prefab entity identity to live `SceneEntity::Handle`. This phase uses those restored prefab handles as the keep-set after a successful hierarchy restore, then destroys pure-extra child subtrees that do not contain any prefab-bound entity. It intentionally keeps subtrees that contain a prefab-bound entity, because inherited entity reparent restore is a separate higher-risk phase.

**Tech Stack:** C++20, RenderVerseX Scene module, standalone `ResourceInstantiationValidation` executable.

---

## File Structure

- Modify `Scene/Private/Prefab.cpp`
  - Add helper functions to collect extra child subtree handles safely.
  - Prune pure-extra live child subtrees after successful `RestoreHierarchyStateTo()`.

- Modify `Tests/ResourceInstantiationValidation/main.cpp`
  - Update the old extra-child preservation test to require pruning.
  - Add nested extra subtree coverage.
  - Update duplicate-extra-child behavior now that runtime bindings can identify the inherited child.

- Modify this plan file while executing checkboxes.

---

## Task 1: Add RED Coverage For Extra Entity Pruning

**Files:**
- Modify: `Tests/ResourceInstantiationValidation/main.cpp`

- [x] **Step 1: Rename and invert the simple extra child test**

Replace `Test_PrefabInstanceRevertAllPreservesExtraLiveChildEntity()` with:

```cpp
bool Test_PrefabInstanceRevertAllPrunesExtraLiveChildEntity()
{
    PrefabEntityData rootData;
    rootData.name = "PruneExtraRoot";

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
    const SceneEntity::Handle extraHandle = extra->GetHandle();

    prefabInstance->RevertAll();

    TEST_ASSERT_TRUE(root->GetChildren().empty());
    TEST_ASSERT_TRUE(sceneManager.GetEntity(extraHandle) == nullptr);

    sceneManager.Shutdown();
    return true;
}
```

- [x] **Step 2: Add nested pure-extra subtree pruning coverage**

Add this test near the simple extra child test:

```cpp
bool Test_PrefabInstanceRevertAllPrunesNestedExtraLiveChildSubtree()
{
    PrefabEntityData rootData;
    rootData.name = "PruneNestedExtraRoot";

    auto prefab = Prefab::CreateFromData({rootData});

    SceneManager sceneManager;
    sceneManager.Initialize();

    SceneEntity* root = prefab->Instantiate(sceneManager);
    TEST_ASSERT_NOT_NULL(root);
    auto* prefabInstance = root->GetComponent<PrefabInstance>();
    TEST_ASSERT_NOT_NULL(prefabInstance);

    ActorSpawnParams branchParams;
    branchParams.name = "ExtraBranch";
    branchParams.parent = root;
    SceneEntity* branch = sceneManager.SpawnActor(branchParams);
    TEST_ASSERT_NOT_NULL(branch);

    ActorSpawnParams leafParams;
    leafParams.name = "ExtraLeaf";
    leafParams.parent = branch;
    SceneEntity* leaf = sceneManager.SpawnActor(leafParams);
    TEST_ASSERT_NOT_NULL(leaf);

    const SceneEntity::Handle branchHandle = branch->GetHandle();
    const SceneEntity::Handle leafHandle = leaf->GetHandle();

    prefabInstance->RevertAll();

    TEST_ASSERT_TRUE(root->GetChildren().empty());
    TEST_ASSERT_TRUE(sceneManager.GetEntity(branchHandle) == nullptr);
    TEST_ASSERT_TRUE(sceneManager.GetEntity(leafHandle) == nullptr);

    sceneManager.Shutdown();
    return true;
}
```

- [x] **Step 3: Update duplicate child behavior**

Rename `Test_PrefabInstanceRevertAllDoesNotCreateWhenUniquePathIsAmbiguous()` to:

```cpp
bool Test_PrefabInstanceRevertAllPrunesDuplicateExtraChildAfterBoundRestore()
```

Keep the same setup, but after `RevertAll()` assert:

```cpp
TEST_ASSERT_EQ(static_cast<size_t>(1), root->GetChildren().size());
TEST_ASSERT_TRUE(sceneManager.GetEntity(duplicate->GetHandle()) == nullptr);
TEST_ASSERT_EQ(std::string("Child"), root->GetChildren()[0]->GetName());
```

This test documents the new behavior: because the inherited child remains bound by prefab identity, the same-name duplicate is now clearly an extra child.

- [x] **Step 4: Update test registrations**

Replace/add registrations:

```cpp
suite.AddTest("PrefabInstanceRevertAllPrunesExtraLiveChildEntity",
              Test_PrefabInstanceRevertAllPrunesExtraLiveChildEntity);
suite.AddTest("PrefabInstanceRevertAllPrunesNestedExtraLiveChildSubtree",
              Test_PrefabInstanceRevertAllPrunesNestedExtraLiveChildSubtree);
suite.AddTest("PrefabInstanceRevertAllPrunesDuplicateExtraChildAfterBoundRestore",
              Test_PrefabInstanceRevertAllPrunesDuplicateExtraChildAfterBoundRestore);
```

- [x] **Step 5: Run targeted validation and confirm RED**

Run:

```powershell
cmake --build build\Debug --config Debug --target ResourceInstantiationValidation
.\build\Debug\Tests\Debug\ResourceInstantiationValidation.exe
```

Expected before implementation: the new/updated pruning tests fail because `RestoreHierarchyStateTo()` currently preserves extra live child entities.

---

## Task 2: Implement Safe Extra Entity Pruning

**Files:**
- Modify: `Scene/Private/Prefab.cpp`

- [x] **Step 1: Add postorder subtree handle collection helper**

Add this helper in the anonymous namespace near the prefab entity binding helpers:

```cpp
void CollectEntitySubtreeHandlesPostOrder(
    const SceneEntity& entity,
    std::vector<SceneEntity::Handle>& handles)
{
    const std::vector<SceneEntity*> children = entity.GetChildren();
    for (const SceneEntity* child : children)
    {
        if (child)
        {
            CollectEntitySubtreeHandlesPostOrder(*child, handles);
        }
    }

    handles.push_back(entity.GetHandle());
}
```

Destroying descendants before parents matters because `SceneManager::DestroyEntityImmediate()` destroys only the requested entity; `SceneEntity::~SceneEntity()` detaches children instead of recursively destroying them.

- [x] **Step 2: Add subtree keep-set detection**

Add:

```cpp
bool EntitySubtreeContainsAnyHandle(
    const SceneEntity& entity,
    const std::unordered_set<SceneEntity::Handle>& handles)
{
    if (handles.find(entity.GetHandle()) != handles.end())
    {
        return true;
    }

    for (const SceneEntity* child : entity.GetChildren())
    {
        if (child && EntitySubtreeContainsAnyHandle(*child, handles))
        {
            return true;
        }
    }

    return false;
}
```

- [x] **Step 3: Add extra entity collection helper**

Add:

```cpp
void CollectExtraLivePrefabEntityHandles(
    const SceneEntity& entity,
    const std::unordered_set<SceneEntity::Handle>& prefabEntityHandles,
    std::vector<SceneEntity::Handle>& handlesToDestroy)
{
    const std::vector<SceneEntity*> children = entity.GetChildren();
    for (const SceneEntity* child : children)
    {
        if (!child)
        {
            continue;
        }

        if (prefabEntityHandles.find(child->GetHandle()) == prefabEntityHandles.end())
        {
            if (!EntitySubtreeContainsAnyHandle(*child, prefabEntityHandles))
            {
                CollectEntitySubtreeHandlesPostOrder(*child, handlesToDestroy);
            }
            continue;
        }

        CollectExtraLivePrefabEntityHandles(*child, prefabEntityHandles, handlesToDestroy);
    }
}
```

The `EntitySubtreeContainsAnyHandle()` check is the conservative guard that prevents accidentally destroying an inherited prefab entity if a user reparented it under an extra live node. That reparent case remains intentionally deferred.

- [x] **Step 4: Add prune function**

Add:

```cpp
void PruneExtraLivePrefabEntities(
    SceneManager& sceneManager,
    SceneEntity& rootEntity,
    const std::vector<SceneEntity*>& restoredEntities)
{
    std::unordered_set<SceneEntity::Handle> prefabEntityHandles;
    for (const SceneEntity* entity : restoredEntities)
    {
        if (entity)
        {
            prefabEntityHandles.insert(entity->GetHandle());
        }
    }

    std::vector<SceneEntity::Handle> handlesToDestroy;
    CollectExtraLivePrefabEntityHandles(rootEntity, prefabEntityHandles, handlesToDestroy);

    for (SceneEntity::Handle handle : handlesToDestroy)
    {
        if (sceneManager.GetEntity(handle))
        {
            sceneManager.DestroyEntity(handle);
        }
    }
}
```

- [x] **Step 5: Call pruning after successful restore**

At the end of `Prefab::RestoreHierarchyStateTo()`, after the loop and before `return true`, add:

```cpp
PruneExtraLivePrefabEntities(*sceneManager, rootEntity, restoredEntities);
```

Do not prune when restore fails. Existing behavior that preserves overrides on failed restore must stay intact.

---

## Task 3: Verify And Review

**Files:**
- Modify: `Docs/superpowers/plans/2026-05-17-ue-style-actor-component-phase31-prefab-revert-prune-extras.md`

- [x] **Step 1: Run targeted validation**

Run:

```powershell
cmake --build build\Debug --config Debug --target ResourceInstantiationValidation
.\build\Debug\Tests\Debug\ResourceInstantiationValidation.exe
```

Expected after implementation: all `ResourceInstantiationValidation` tests pass.

- [x] **Step 2: Run adjacent validation**

Run:

```powershell
cmake --build build\Debug --config Debug --target ActorComponentValidation
.\build\Debug\Tests\Debug\ActorComponentValidation.exe
cmake --build build\Debug --config Debug --target RenderSceneValidation
.\build\Debug\Tests\Debug\RenderSceneValidation.exe
```

Expected: all adjacent scene/component validations pass.

- [x] **Step 3: Run whitespace check**

Run:

```powershell
git diff --check
```

Expected: exit code `0`; LF-to-CRLF warnings are acceptable.

- [x] **Step 4: Request one code review agent**

Ask one `gpt-5.3-codex-spark` review agent to review Critical/Important issues only. If the agent times out, close it and perform a local Critical/Important review before committing.

- [ ] **Step 5: Commit**

Run:

```powershell
git add Scene/Private/Prefab.cpp Tests/ResourceInstantiationValidation/main.cpp Docs/superpowers/plans/2026-05-17-ue-style-actor-component-phase31-prefab-revert-prune-extras.md
git commit -m "Prune extra prefab children on revert"
```

---

## Self-Review

- **Spec coverage:** The plan makes full revert delete added child entities while preserving previously completed missing-child and renamed inherited-child behavior.
- **Safety:** It destroys pure-extra subtrees postorder and refuses to destroy any extra subtree that contains a prefab-bound entity, deferring inherited reparent repair to a later phase.
- **Placeholder scan:** No TBD or vague implementation-only steps remain.
- **Type consistency:** The plan uses existing `SceneEntity::Handle`, `SceneManager::DestroyEntity()`, `SceneEntity::GetChildren()`, and `RestoreHierarchyStateTo()` types.
