# UE-Style Actor Component Phase 27 Prefab Apply Structural Sync Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [x]`) syntax for tracking.

**Goal:** Make `PrefabInstance::ApplyToPrefab()` write live hierarchy structure back into the prefab, including added, removed, and nested child entities.

**Architecture:** Keep Phase 26 path-aware overrides as the identity layer, then add a structural apply path that rebuilds prefab entity data from the live root hierarchy. Rebuilding uses the existing `SerializeEntity(...)` capture path, then derives a capture report from the rebuilt prefab paths so overrides on live/captured paths are cleared and overrides targeting deleted paths are discarded as obsolete.

**Tech Stack:** C++20, RenderVerseX Scene module, standalone validation executable `ResourceInstantiationValidation`.

---

## Scope

- Extend prefab apply semantics only; do not make `RevertAll()` create or destroy entities in this phase.
- `ApplyToPrefab()` should rebuild `Prefab::m_entities` from the live instance root and all current descendants.
- Added live children should become new `PrefabEntityData` entries with correct `parentIndex`, properties, actor class name, legacy component payloads, and actor component payloads/names.
- Deleted live children should be removed from the prefab data when apply runs.
- Nested live hierarchy should preserve parent indices after apply.
- Overrides whose normalized `entityPath` no longer exists after the rebuild should be removed because their target no longer exists.
- Supported property/component payload overrides on captured paths should still clear through the Phase 26 capture report rules.
- Unsupported overrides on still-existing paths should remain.
- Do not add a general editor reflection/property metadata system in this phase.

## Files

- Modify: `E:\WorkSpace\RenderVerseX\Scene\Include\Scene\Prefab.h`
- Modify: `E:\WorkSpace\RenderVerseX\Scene\Private\Prefab.cpp`
- Modify: `E:\WorkSpace\RenderVerseX\Tests\ResourceInstantiationValidation\main.cpp`
- Modify: `E:\WorkSpace\RenderVerseX\Docs\superpowers\plans\2026-05-17-ue-style-actor-component-phase27-prefab-apply-structural-sync.md`

---

## Task 1: Add Minimal Failing Structural Apply Coverage

- [x] **Step 1: Add live child addition apply test**

In `Tests\ResourceInstantiationValidation\main.cpp`, add this test near the existing `ApplyToPrefab` tests:

```cpp
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
```

- [x] **Step 2: Replace missing-live-child preservation test with deletion apply test**

In `Tests\ResourceInstantiationValidation\main.cpp`, replace the existing
`Test_PrefabInstanceApplyToPrefabPreservesChildOverrideWhenLiveChildMissing`
function with this deletion-oriented test. Do not keep the old preservation test,
because Phase 27 changes the apply semantics from "preserve missing child
overrides" to "the missing live child was deleted from the prefab instance and
should be removed from prefab data plus override state":

```cpp
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
```

- [x] **Step 3: Add nested hierarchy parent-index apply test**

Add this test near the new structural apply tests:

```cpp
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
```

- [x] **Step 4: Add unsupported override preservation test for existing path**

Add a guard that structural rebuild does not clear unsupported overrides on paths that still exist:

```cpp
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
```

- [x] **Step 5: Register the tests**

Register the four tests in `main()` near the existing `ApplyToPrefab` group,
and replace the old registration:

```cpp
suite.AddTest("PrefabInstanceApplyToPrefabPreservesChildOverrideWhenLiveChildMissing",
              Test_PrefabInstanceApplyToPrefabPreservesChildOverrideWhenLiveChildMissing);
```

with:

```cpp
suite.AddTest("PrefabInstanceApplyToPrefabAddsLiveChildEntity",
              Test_PrefabInstanceApplyToPrefabAddsLiveChildEntity);
suite.AddTest("PrefabInstanceApplyToPrefabRemovesDeletedChildEntityAndOverrides",
              Test_PrefabInstanceApplyToPrefabRemovesDeletedChildEntityAndOverrides);
suite.AddTest("PrefabInstanceApplyToPrefabPreservesNestedParentIndices",
              Test_PrefabInstanceApplyToPrefabPreservesNestedParentIndices);
suite.AddTest("PrefabInstanceApplyToPrefabPreservesUnsupportedOverrideOnExistingChild",
              Test_PrefabInstanceApplyToPrefabPreservesUnsupportedOverrideOnExistingChild);
```

- [x] **Step 6: Confirm RED**

Run:

```powershell
cmake --build build\Debug --config Debug --target ResourceInstantiationValidation
.\build\Debug\Tests\Debug\ResourceInstantiationValidation.exe
```

Expected before implementation: the build succeeds, but the new structural apply tests fail because `ApplyToPrefab()` only updates existing prefab entity paths and does not rebuild `m_entities`.

---

## Task 2: Add Structural Capture API

- [x] **Step 1: Declare structural rebuild API**

In `Scene\Include\Scene\Prefab.h`, add a new method immediately after `UpdateHierarchyStateFrom(...)`:

```cpp
/// Rebuild prefab entity data from a live hierarchy and report captured paths.
PrefabApplyCaptureReport RebuildHierarchyStateFrom(const SceneEntity& rootEntity);
```

- [x] **Step 2: Add per-entity capture report helper**

In `Scene\Private\Prefab.cpp`, add this helper in the anonymous namespace after
`HasCapturedComponentPayload(...)`. This helper appends one already-captured
entity, so it is safe for both partial hierarchy updates and full structural
rebuilds:

```cpp
void AppendEntityCaptureToReport(PrefabApplyCaptureReport& report,
                                 const std::string& entityPath,
                                 const PrefabEntityData& entityData)
{
    const std::string normalizedPath = NormalizeEntityPath(entityPath);
    report.capturedEntityPaths.push_back(normalizedPath);

    for (const auto& [componentType, serializedData] : entityData.componentData)
    {
        (void)serializedData;
        report.capturedComponentPayloads.push_back({normalizedPath, componentType, ""});
    }

    for (const auto& componentData : entityData.actorComponentData)
    {
        report.capturedComponentPayloads.push_back({normalizedPath, componentData.className, ""});
        if (!componentData.name.empty())
        {
            report.capturedComponentPayloads.push_back(
                {normalizedPath, componentData.className, componentData.name});
        }
    }
}
```

- [x] **Step 3: Add full-prefab capture report helper**

Add this helper after `AppendEntityCaptureToReport(...)`. It must only be used
after a full structural rebuild or in other places that intentionally report all
prefab paths:

```cpp
PrefabApplyCaptureReport BuildCaptureReportForPrefab(const Prefab& prefab)
{
    PrefabApplyCaptureReport report;
    const auto paths = BuildPrefabEntityPaths(prefab);
    for (size_t i = 0; i < prefab.GetEntityCount(); ++i)
    {
        const PrefabEntityData* entityData = prefab.GetEntityData(i);
        if (!entityData)
        {
            continue;
        }

        const std::string entityPath = i < paths.size() ? NormalizeEntityPath(paths[i]) : std::string();
        AppendEntityCaptureToReport(report, entityPath, *entityData);
    }

    return report;
}
```

- [x] **Step 4: Use the per-entity helper in existing hierarchy update**

Replace the manual report-building body inside `Prefab::UpdateHierarchyStateFrom(...)`
with `AppendEntityCaptureToReport(report, entityPath, m_entities[i])` immediately
after `CapturePrefabEntityProperties(...)` and `CapturePrefabEntityComponents(...)`.

Do not call `BuildCaptureReportForPrefab(...)` from `UpdateHierarchyStateFrom(...)`,
because that would report missing live paths as captured and would break the
Phase 26 partial-update semantics.

- [x] **Step 5: Implement full rebuild method**

Add this method in `Scene\Private\Prefab.cpp` after `UpdateHierarchyStateFrom(...)`:

```cpp
PrefabApplyCaptureReport Prefab::RebuildHierarchyStateFrom(const SceneEntity& rootEntity)
{
    m_entities.clear();
    SerializeEntity(&rootEntity, -1);
    return BuildCaptureReportForPrefab(*this);
}
```

---

## Task 3: Remove Obsolete Path Overrides After Structural Apply

- [x] **Step 1: Include unordered set support**

In `Scene\Private\Prefab.cpp`, add:

```cpp
#include <unordered_set>
```

- [x] **Step 2: Add captured path lookup helper**

In the anonymous namespace near other override helpers, add:

```cpp
std::unordered_set<std::string> MakeCapturedEntityPathSet(const PrefabApplyCaptureReport& report)
{
    std::unordered_set<std::string> paths;
    for (const std::string& path : report.capturedEntityPaths)
    {
        paths.insert(NormalizeEntityPath(path));
    }
    return paths;
}
```

- [x] **Step 3: Add obsolete override removal helper**

Add this helper near `RemoveSupportedComponentPayloadOverrides(...)`:

```cpp
void RemoveOverridesOutsideCapturedEntityPaths(std::vector<PropertyOverride>& overrides,
                                               const PrefabApplyCaptureReport& report)
{
    const std::unordered_set<std::string> capturedPaths = MakeCapturedEntityPathSet(report);
    overrides.erase(
        std::remove_if(overrides.begin(), overrides.end(),
            [&](const PropertyOverride& override) {
                return capturedPaths.find(NormalizeEntityPath(override.entityPath)) == capturedPaths.end();
            }),
        overrides.end()
    );
}
```

- [x] **Step 4: Switch `ApplyToPrefab()` to structural rebuild**

In `PrefabInstance::ApplyToPrefab()`, replace:

```cpp
const PrefabApplyCaptureReport report = m_prefab->UpdateHierarchyStateFrom(*owner);
```

with:

```cpp
const PrefabApplyCaptureReport report = m_prefab->RebuildHierarchyStateFrom(*owner);
```

Then, before clearing supported property/payload overrides, call:

```cpp
RemoveOverridesOutsideCapturedEntityPaths(m_overrides, report);
```

The final cleanup block should clear transient component name bindings, remove obsolete path overrides, remove supported entity property overrides on captured paths, and remove supported component payload overrides on captured payload keys.

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
git add Docs\superpowers\plans\2026-05-17-ue-style-actor-component-phase27-prefab-apply-structural-sync.md Scene\Include\Scene\Prefab.h Scene\Private\Prefab.cpp Tests\ResourceInstantiationValidation\main.cpp
git commit -m "Apply prefab hierarchy structural changes"
```

---

## Self-Review

- Spec coverage: The plan covers apply-time addition, deletion, nested parent index preservation, override cleanup for deleted paths, and unsupported override preservation on existing paths.
- Compatibility: `RebuildHierarchyStateFrom(...)` is additive; `UpdateHierarchyStateFrom(...)` can remain available for non-structural use. `RevertAll()` semantics are intentionally untouched and deferred to a separate phase.
- Scope: The plan avoids editor reflection metadata and avoids runtime structural revert/destruction semantics in this phase.
- Placeholder scan: No TBD/TODO placeholders.
