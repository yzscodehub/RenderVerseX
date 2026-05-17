# UE-Style Actor Component Phase 30: Prefab Entity Runtime Bindings Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make prefab instance revert operations find inherited child entities by stable runtime identity, so renaming a prefab child does not cause `RevertAll()` to create a duplicate child.

**Architecture:** Add a serialized `prefabEntityId` to each `PrefabEntityData`, then store transient `PrefabEntityRuntimeBinding` records on `PrefabInstance` that map prefab entity ids to live `SceneEntity::Handle` values. Revert paths still use the existing prefab-root-relative path strings for override identity, but entity lookup first resolves the runtime binding and only falls back to path lookup when the bound entity no longer exists.

**Tech Stack:** C++20, RenderVerseX Scene module, standalone `ResourceInstantiationValidation` executable.

---

## File Structure

- Modify `Scene/Include/Scene/Prefab.h`
  - Add `PrefabEntityData::prefabEntityId`.
  - Add `PrefabEntityRuntimeBinding`.
  - Thread runtime bindings through `Prefab::RestoreHierarchyStateTo`.
  - Store runtime bindings inside `PrefabInstance`.

- Modify `Scene/Private/Prefab.cpp`
  - Assign stable prefab entity ids in prefab creation and mutation paths.
  - Capture entity bindings during instantiation.
  - Resolve `RevertAll()` and `RevertProperty()` target entities by binding before path.
  - Restore prefab entity names during full revert.
  - Clear runtime bindings after `ApplyToPrefab()` and `Unpack()`.

- Modify `Tests/ResourceInstantiationValidation/main.cpp`
  - Add regression tests for renamed inherited child entities.
  - Add one targeted `RevertProperty()` regression so property revert also benefits from runtime identity.

---

## Task 1: Add Regression Tests

**Files:**
- Modify: `Tests/ResourceInstantiationValidation/main.cpp`

- [x] **Step 1: Add a `RevertAll()` regression for renamed inherited child entities**

Add this test near the existing prefab revert-all child tests:

```cpp
bool Test_PrefabInstanceRevertAllRestoresRenamedChildEntityByRuntimeBinding()
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
    TEST_ASSERT_NOT_NULL(root);

    auto* prefabInstance = root->GetComponent<PrefabInstance>();
    TEST_ASSERT_NOT_NULL(prefabInstance);
    TEST_ASSERT_EQ(static_cast<size_t>(1), root->GetChildren().size());

    SceneEntity* child = root->GetChildren()[0];
    child->SetName("RenamedChild");
    child->SetPosition(Vec3(9.0f, 9.0f, 9.0f));
    prefabInstance->AddOverride({"SceneEntity", "position", child->GetPosition(), "", "Child"});

    prefabInstance->RevertAll();

    TEST_ASSERT_EQ(static_cast<size_t>(1), root->GetChildren().size());
    TEST_ASSERT_EQ(child, root->GetChildren()[0]);
    TEST_ASSERT_EQ(std::string("Child"), child->GetName());
    TEST_ASSERT_EQ(Vec3(1.0f, 2.0f, 3.0f), child->GetPosition());
    TEST_ASSERT_TRUE(prefabInstance->GetOverrides().empty());

    sceneManager.Shutdown();
    return true;
}
```

- [x] **Step 2: Add a `RevertProperty()` regression for renamed inherited child entities**

Add this test near the existing prefab revert-property child tests:

```cpp
bool Test_PrefabInstanceRevertPropertyRestoresRenamedChildEntityByRuntimeBinding()
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
    TEST_ASSERT_NOT_NULL(root);

    auto* prefabInstance = root->GetComponent<PrefabInstance>();
    TEST_ASSERT_NOT_NULL(prefabInstance);
    TEST_ASSERT_EQ(static_cast<size_t>(1), root->GetChildren().size());

    SceneEntity* child = root->GetChildren()[0];
    child->SetName("RenamedChild");
    child->SetPosition(Vec3(8.0f, 8.0f, 8.0f));
    prefabInstance->AddOverride({"SceneEntity", "position", child->GetPosition(), "", "Child"});

    prefabInstance->RevertProperty("SceneEntity", "position", "", "Child");

    TEST_ASSERT_EQ(static_cast<size_t>(1), root->GetChildren().size());
    TEST_ASSERT_EQ(child, root->GetChildren()[0]);
    TEST_ASSERT_EQ(std::string("RenamedChild"), child->GetName());
    TEST_ASSERT_EQ(Vec3(2.0f, 3.0f, 4.0f), child->GetPosition());
    TEST_ASSERT_FALSE(prefabInstance->IsOverridden("SceneEntity", "position", "", "Child"));

    sceneManager.Shutdown();
    return true;
}
```

- [x] **Step 3: Register the tests**

Add these registrations in the same section as related prefab tests:

```cpp
suite.AddTest("PrefabInstanceRevertAllRestoresRenamedChildEntityByRuntimeBinding",
              Test_PrefabInstanceRevertAllRestoresRenamedChildEntityByRuntimeBinding);
suite.AddTest("PrefabInstanceRevertPropertyRestoresRenamedChildEntityByRuntimeBinding",
              Test_PrefabInstanceRevertPropertyRestoresRenamedChildEntityByRuntimeBinding);
```

- [x] **Step 4: Run the validation test and confirm RED**

Run:

```powershell
cmake --build build\Debug --config Debug --target ResourceInstantiationValidation
.\build\Debug\Tests\Debug\ResourceInstantiationValidation.exe
```

Expected before implementation: the new `RevertAll()` test fails because the path lookup does not find renamed `Child` and a duplicate child is created. The new `RevertProperty()` test fails because path lookup cannot find the renamed child.

---

## Task 2: Add Prefab Entity Ids and Runtime Binding Storage

**Files:**
- Modify: `Scene/Include/Scene/Prefab.h`
- Modify: `Scene/Private/Prefab.cpp`

- [x] **Step 1: Add entity identity data to the public prefab structures**

In `PrefabEntityData`, append:

```cpp
uint64 prefabEntityId = 0;
```

After `PrefabActorComponentNameBinding`, add:

```cpp
/**
 * @brief Transient mapping from prefab entity identity to live instance entity.
 */
struct PrefabEntityRuntimeBinding
{
    uint64 prefabEntityId = 0;
    SceneEntity::Handle instanceHandle = SceneEntity::InvalidHandle;
    std::string entityPath;
};
```

- [x] **Step 2: Add prefab id allocation helpers**

In `Prefab` private declarations, add:

```cpp
uint64 AllocateEntityId();
void EnsureEntityIds();
```

Add a member initialized in-class:

```cpp
uint64 m_nextEntityId = 1;
```

In `Prefab.cpp`, implement:

```cpp
uint64 Prefab::AllocateEntityId()
{
    return m_nextEntityId++;
}

void Prefab::EnsureEntityIds()
{
    std::unordered_set<uint64> usedIds;
    uint64 nextId = 1;

    for (auto& entity : m_entities)
    {
        if (entity.prefabEntityId != 0 &&
            usedIds.find(entity.prefabEntityId) == usedIds.end())
        {
            usedIds.insert(entity.prefabEntityId);
            nextId = std::max(nextId, entity.prefabEntityId + 1);
            continue;
        }

        while (usedIds.find(nextId) != usedIds.end())
        {
            ++nextId;
        }
        entity.prefabEntityId = nextId;
        usedIds.insert(nextId);
        ++nextId;
    }

    m_nextEntityId = nextId;
}
```

- [x] **Step 3: Assign ids whenever prefab data is created or replaced**

Update:

```cpp
Prefab::CreateFromData(...)
Prefab::SerializeEntity(...)
Prefab::AddEntityData(...)
Prefab::Clear()
```

Required behavior:

```cpp
prefab->m_entities = std::move(entityData);
prefab->EnsureEntityIds();
```

```cpp
data.prefabEntityId = AllocateEntityId();
```

```cpp
if (data.prefabEntityId == 0)
{
    data.prefabEntityId = AllocateEntityId();
}
else
{
    m_nextEntityId = std::max(m_nextEntityId, data.prefabEntityId + 1);
}
m_entities.push_back(std::move(data));
```

```cpp
m_entities.clear();
m_nextEntityId = 1;
```

- [x] **Step 4: Store runtime bindings on prefab instances**

In `PrefabInstance` private declarations, add:

```cpp
void SetEntityRuntimeBindings(std::vector<PrefabEntityRuntimeBinding> bindings)
{
    m_entityBindings = std::move(bindings);
}

std::vector<PrefabEntityRuntimeBinding> m_entityBindings;
```

---

## Task 3: Capture Runtime Entity Bindings During Instantiation

**Files:**
- Modify: `Scene/Private/Prefab.cpp`

- [x] **Step 1: Capture bindings in `Prefab::InstantiateInternal()`**

Before the creation loops, add:

```cpp
std::vector<PrefabEntityRuntimeBinding> entityBindings;
entityBindings.reserve(m_entities.size());
```

When each entity has been created and its path is known, push:

```cpp
entityBindings.push_back(
    {entityData.prefabEntityId, entity->GetHandle(), NormalizeEntityPath(entityPath)});
```

After setting component name bindings on the root `PrefabInstance`, add:

```cpp
prefabInstance->SetEntityRuntimeBindings(std::move(entityBindings));
```

- [x] **Step 2: Keep root binding present**

Ensure the root entity receives the empty path binding:

```cpp
{m_entities[0].prefabEntityId, root->GetHandle(), std::string()}
```

---

## Task 4: Resolve Revert Targets by Binding First

**Files:**
- Modify: `Scene/Private/Prefab.cpp`

- [x] **Step 1: Add binding lookup helpers inside the anonymous namespace**

Add:

```cpp
bool IsEntityInsidePrefabRoot(const SceneEntity& root, const SceneEntity& candidate)
{
    return &candidate == &root || candidate.IsDescendantOf(&root);
}

SceneEntity* FindBoundPrefabEntity(SceneManager& sceneManager,
                                   SceneEntity& rootEntity,
                                   const std::vector<PrefabEntityRuntimeBinding>& bindings,
                                   const PrefabEntityData& entityData)
{
    if (entityData.prefabEntityId == 0)
    {
        return nullptr;
    }

    for (const auto& binding : bindings)
    {
        if (binding.prefabEntityId != entityData.prefabEntityId)
        {
            continue;
        }

        SceneEntity* entity = sceneManager.GetEntity(binding.instanceHandle);
        if (entity && IsEntityInsidePrefabRoot(rootEntity, *entity))
        {
            return entity;
        }
    }

    return nullptr;
}

SceneEntity* FindPrefabEntityForRestore(SceneManager& sceneManager,
                                        SceneEntity& rootEntity,
                                        const std::vector<PrefabEntityRuntimeBinding>& bindings,
                                        const PrefabEntityData& entityData,
                                        const std::string& entityPath)
{
    if (SceneEntity* bound = FindBoundPrefabEntity(sceneManager, rootEntity, bindings, entityData))
    {
        return bound;
    }

    return FindLiveEntityByPath(rootEntity, entityPath);
}

void UpsertPrefabEntityRuntimeBinding(
    std::vector<PrefabEntityRuntimeBinding>& bindings,
    uint64 prefabEntityId,
    SceneEntity::Handle instanceHandle,
    const std::string& entityPath)
{
    if (prefabEntityId == 0)
    {
        return;
    }

    const std::string normalizedPath = NormalizeEntityPath(entityPath);
    for (auto& binding : bindings)
    {
        if (binding.prefabEntityId == prefabEntityId)
        {
            binding.instanceHandle = instanceHandle;
            binding.entityPath = normalizedPath;
            return;
        }
    }

    bindings.push_back({prefabEntityId, instanceHandle, normalizedPath});
}
```

- [x] **Step 2: Thread bindings through `RestoreHierarchyStateTo()`**

Change the declaration and definition:

```cpp
bool RestoreHierarchyStateTo(
    SceneEntity& rootEntity,
    std::vector<PrefabActorComponentNameBinding>& nameBindings,
    std::vector<PrefabEntityRuntimeBinding>& entityBindings) const;
```

Update `PrefabInstance::RevertAll()`:

```cpp
std::vector<PrefabActorComponentNameBinding> restoredBindings = m_componentNameBindings;
std::vector<PrefabEntityRuntimeBinding> restoredEntityBindings = m_entityBindings;
if (m_prefab->RestoreHierarchyStateTo(*owner, restoredBindings, restoredEntityBindings))
{
    m_componentNameBindings = std::move(restoredBindings);
    m_entityBindings = std::move(restoredEntityBindings);
    m_overrides.clear();
}
```

- [x] **Step 3: Use binding-aware lookup in preflight**

Change `CanRestoreExistingEntityMissingComponents()` to receive `SceneManager&` and entity bindings. Resolve entities with `FindPrefabEntityForRestore()` instead of `FindLiveEntityByPath()`.

- [x] **Step 4: Use binding-aware lookup in the restore loop**

In non-root restore:

```cpp
entity = FindPrefabEntityForRestore(
    *sceneManager, rootEntity, entityBindings, entityData, entityPath);
```

When an entity is reused or created, call:

```cpp
UpsertPrefabEntityRuntimeBinding(
    entityBindings, entityData.prefabEntityId, entity->GetHandle(), entityPath);
```

- [x] **Step 5: Restore full entity identity during `RevertAll()`**

Before applying transform/layer/active, restore the prefab entity name:

```cpp
entity->SetName(entityData.name);
ApplyAllPrefabEntityProperties(entity, entityData);
```

Do not add `"name"` to `ApplyPrefabEntityProperty()` in this phase; `RevertProperty()` should remain scoped to the requested property and should not rename entities.

- [x] **Step 6: Use binding-aware lookup in `RevertProperty()`**

After resolving `targetData`, look up the target entity with the runtime binding first:

```cpp
SceneManager* sceneManager = owner->GetSceneManager();
SceneEntity* targetEntity = sceneManager
    ? FindPrefabEntityForRestore(
        *sceneManager, *owner, m_entityBindings, *targetData, normalizedPath)
    : FindLiveEntityByPath(*owner, normalizedPath);
```

If the binding lookup succeeds, the override is removed using the existing normalized path, preserving the external override identity.

- [x] **Step 7: Rebuild stale bindings on prefab structural rewrite and clear them on unpack**

In `PrefabInstance::ApplyToPrefab()`, when `report.capturedEntityPaths` is not empty, rebuild runtime entity bindings against the newly rebuilt prefab data:

```cpp
m_entityBindings = BuildPrefabEntityRuntimeBindings(*m_prefab, *owner);
```

In `PrefabInstance::Unpack()`, clear:

```cpp
m_entityBindings.clear();
```

- [x] **Step 8: Add an `ApplyToPrefab()` regression for rebuilt runtime entity bindings**

Add and verify `Test_PrefabInstanceApplyToPrefabRebuildsRuntimeEntityBindings`. It must fail when `ApplyToPrefab()` drops entity bindings, and pass once the method rebuilds bindings from the current live hierarchy.

---

## Task 5: Verify and Review

**Files:**
- Modify: `Docs/superpowers/plans/2026-05-17-ue-style-actor-component-phase30-prefab-entity-runtime-bindings.md`

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

Expected: exit code `0`; Git may still print existing LF-to-CRLF conversion warnings.

- [x] **Step 4: Request one code review agent**

Ask one `gpt-5.3-codex-spark` review agent to review the staged diff for Critical/Important issues only. Fix any Critical/Important findings before proceeding.

Result: multiple review agents timed out without findings and were closed. Local Critical/Important review found an `ApplyToPrefab()` binding lifecycle gap; this plan was updated and the implementation now rebuilds runtime entity bindings after prefab structural capture.

- [x] **Step 5: Commit**

Run:

```powershell
git add Scene/Include/Scene/Prefab.h Scene/Private/Prefab.cpp Tests/ResourceInstantiationValidation/main.cpp Docs/superpowers/plans/2026-05-17-ue-style-actor-component-phase30-prefab-entity-runtime-bindings.md
git commit -m "Stabilize prefab entity revert bindings"
```

---

## Self-Review

- **Spec coverage:** This phase addresses the next prefab identity gap in the UE-style component framework: inherited child entities survive renames and remain tied to prefab data. It intentionally preserves the previous behavior of not deleting extra live child entities.
- **Placeholder scan:** No `TBD`, vague implementation-only steps, or missing commands remain.
- **Type consistency:** `PrefabEntityRuntimeBinding`, `prefabEntityId`, `m_entityBindings`, and `RestoreHierarchyStateTo(..., entityBindings)` are used consistently across declarations, implementation, and tests.
