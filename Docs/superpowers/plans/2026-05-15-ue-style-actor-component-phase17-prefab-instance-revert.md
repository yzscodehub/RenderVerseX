# UE-Style Actor Component Phase 17 Prefab Instance Revert Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make `PrefabInstance::RevertAll()` and `PrefabInstance::RevertProperty(...)` actually restore supported root entity state instead of only clearing override records.

**Architecture:** Keep this phase narrow and explicit: `PrefabInstance` can restore root `SceneEntity` properties already stored in `PrefabEntityData` (`position`, `rotation`, `scale`, `layerMask`, and `isActive`). Unsupported component/property overrides remain for future reflection-based phases and are not silently removed by `RevertProperty(...)`. Full component property reflection, prefab file persistence, nested prefab edits, and `ApplyToPrefab()` remain out of scope.

**Tech Stack:** C++20, RenderVerseX Scene module, standalone `ResourceInstantiationValidation`.

---

## Scope

- Implement root entity revert for `PrefabInstance::RevertAll()`.
- Implement root entity single-property revert for `PrefabInstance::RevertProperty(...)`.
- Treat empty component type, `"SceneEntity"`, and `"Actor"` as entity-level override targets.
- Preserve unsupported single-property overrides when `RevertProperty(...)` cannot apply the requested property.
- Do not implement component reflection, component payload merging, nested prefab edits, prefab file I/O, or `ApplyToPrefab()` in this phase.

## Files

- Modify: `E:\WorkSpace\RenderVerseX\Scene\Private\Prefab.cpp`
  - Add local helper functions for applying supported root entity prefab properties.
  - Implement `PrefabInstance::RevertAll()` and `PrefabInstance::RevertProperty(...)`.
- Modify: `E:\WorkSpace\RenderVerseX\Tests\ResourceInstantiationValidation\main.cpp`
  - Add tests for full root entity revert, single-property revert, and unsupported override preservation.
- Modify: `E:\WorkSpace\RenderVerseX\Docs\superpowers\plans\2026-05-15-ue-style-actor-component-phase17-prefab-instance-revert.md`
  - Track completion status.

---

## Task 1: Add Failing PrefabInstance Revert Tests

**Files:**
- Modify: `E:\WorkSpace\RenderVerseX\Tests\ResourceInstantiationValidation\main.cpp`

- [x] Add `Test_PrefabInstanceRevertAllRestoresRootEntityState` after `Test_PrefabInstantiateAsChildBuildsSpawnedHierarchy`:

```cpp
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
```

- [x] Add `Test_PrefabInstanceRevertPropertyRestoresOnlyRequestedEntityProperty` after the previous test:

```cpp
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
```

- [x] Add `Test_PrefabInstanceRevertPropertyAcceptsEmptyRootTarget` after the previous test to cover the empty-string root target alias.

- [x] Add `Test_PrefabInstanceRevertPropertyClearsRootTargetAliases` after the previous test to cover `Actor` root-target alias cleanup.

- [x] Add `Test_PrefabInstanceRevertPropertyKeepsUnsupportedOverride` after the previous test:

```cpp
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
```

- [x] Register the revert tests in `main()` after `PrefabInstantiateAsChildBuildsSpawnedHierarchy`:

```cpp
    suite.AddTest("PrefabInstanceRevertAllRestoresRootEntityState",
                  Test_PrefabInstanceRevertAllRestoresRootEntityState);
    suite.AddTest("PrefabInstanceRevertPropertyRestoresOnlyRequestedEntityProperty",
                  Test_PrefabInstanceRevertPropertyRestoresOnlyRequestedEntityProperty);
    suite.AddTest("PrefabInstanceRevertPropertyKeepsUnsupportedOverride",
                  Test_PrefabInstanceRevertPropertyKeepsUnsupportedOverride);
```

- [x] Build the focused target and confirm RED:

```powershell
cmake --build build\Debug --config Debug --target ResourceInstantiationValidation
.\build\Debug\Tests\Debug\ResourceInstantiationValidation.exe
```

Expected result before implementation: the executable builds, but at least the full-revert and single-property-revert tests fail because `PrefabInstance` currently clears override records without restoring root entity state.

---

## Task 2: Implement Root Entity Revert Helpers

**Files:**
- Modify: `E:\WorkSpace\RenderVerseX\Scene\Private\Prefab.cpp`

- [x] Add these helper functions inside an anonymous namespace after `namespace RVX`:

```cpp
namespace
{
    bool IsPrefabEntityOverrideTarget(const std::string& componentType)
    {
        return componentType.empty() || componentType == "SceneEntity" || componentType == "Actor";
    }

    void ApplyAllPrefabEntityProperties(SceneEntity* owner, const PrefabEntityData& data)
    {
        owner->SetPosition(data.position);
        owner->SetRotation(data.rotation);
        owner->SetScale(data.scale);
        owner->SetLayerMask(data.layerMask);
        owner->SetActive(data.isActive);
    }

    bool ApplyPrefabEntityProperty(SceneEntity* owner,
                                   const PrefabEntityData& data,
                                   const std::string& propertyPath)
    {
        if (propertyPath == "position")
        {
            owner->SetPosition(data.position);
            return true;
        }

        if (propertyPath == "rotation")
        {
            owner->SetRotation(data.rotation);
            return true;
        }

        if (propertyPath == "scale")
        {
            owner->SetScale(data.scale);
            return true;
        }

        if (propertyPath == "layerMask")
        {
            owner->SetLayerMask(data.layerMask);
            return true;
        }

        if (propertyPath == "isActive")
        {
            owner->SetActive(data.isActive);
            return true;
        }

        return false;
    }
} // namespace
```

- [x] Build the focused target:

```powershell
cmake --build build\Debug --config Debug --target ResourceInstantiationValidation
```

Expected result: target builds; tests still fail until `PrefabInstance` methods call the helpers.

---

## Task 3: Implement RevertAll And RevertProperty

**Files:**
- Modify: `E:\WorkSpace\RenderVerseX\Scene\Private\Prefab.cpp`

- [x] Replace `PrefabInstance::RevertAll()` with:

```cpp
void PrefabInstance::RevertAll()
{
    if (!m_prefab)
    {
        return;
    }

    SceneEntity* owner = GetOwner();
    const PrefabEntityData* rootData = m_prefab->GetRootData();
    if (!owner || !rootData)
    {
        return;
    }

    ApplyAllPrefabEntityProperties(owner, *rootData);
    m_overrides.clear();
}
```

- [x] Replace `PrefabInstance::RevertProperty(...)` with:

```cpp
void PrefabInstance::RevertProperty(const std::string& componentType, const std::string& propertyPath)
{
    if (!m_prefab)
    {
        return;
    }

    SceneEntity* owner = GetOwner();
    const PrefabEntityData* rootData = m_prefab->GetRootData();
    if (!owner || !rootData)
    {
        return;
    }

    if (!IsPrefabEntityOverrideTarget(componentType))
    {
        return;
    }

    if (ApplyPrefabEntityProperty(owner, *rootData, propertyPath))
    {
        RemoveOverride(componentType, propertyPath);
    }
}
```

- [x] Build and run focused validation:

```powershell
cmake --build build\Debug --config Debug --target ResourceInstantiationValidation
.\build\Debug\Tests\Debug\ResourceInstantiationValidation.exe
```

Expected result: all resource instantiation tests pass, including the three new PrefabInstance revert tests.

---

## Task 4: Validate, Review, And Commit

**Files:**
- No additional source files.

- [x] Run whitespace check:

```powershell
git diff --check
```

Expected result: exit code 0. Line-ending warnings are acceptable if no whitespace-error lines are reported.

- [x] Build all validation targets and ModelViewer sequentially:

```powershell
foreach ($target in @('ActorComponentValidation','SpatialComponentValidation','ResourceInstantiationValidation','RenderSceneValidation','SystemIntegrationTest','MaterialSystemValidation','RenderPassBindingValidation','ModelViewer')) {
    cmake --build build\Debug --config Debug --target $target
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}
```

Expected result: every target builds.

- [x] Run validation executables:

```powershell
.\build\Debug\Tests\Debug\ActorComponentValidation.exe
.\build\Debug\Tests\Debug\SpatialComponentValidation.exe
.\build\Debug\Tests\Debug\ResourceInstantiationValidation.exe
.\build\Debug\Tests\Debug\RenderSceneValidation.exe
.\build\Debug\Tests\Debug\SystemIntegrationTest.exe
.\build\Debug\Tests\Debug\MaterialSystemValidation.exe
.\build\Debug\Tests\Debug\RenderPassBindingValidation.exe
```

Expected result: every executable returns exit code 0 and reports all tests passing.

- [x] Run ModelViewer smoke:

```powershell
$stdout = "build_codex\phase17_modelviewer_stdout.log"
$stderr = "build_codex\phase17_modelviewer_stderr.log"
New-Item -ItemType Directory -Force -Path "build_codex" | Out-Null
Remove-Item -LiteralPath $stdout -ErrorAction SilentlyContinue
Remove-Item -LiteralPath $stderr -ErrorAction SilentlyContinue
$process = Start-Process -FilePath ".\build\Debug\Samples\ModelViewer\Debug\ModelViewer.exe" -ArgumentList "C:\Users\yinzs\Desktop\DamagedHelmet.glb" -RedirectStandardOutput $stdout -RedirectStandardError $stderr -PassThru
Start-Sleep -Seconds 8
if (-not $process.HasExited) { Stop-Process -Id $process.Id }
Get-Content -Path $stdout | Select-String -Pattern "Model loaded successfully|Model instantiated as SceneEntity|Scene entity ready|Uploaded mesh to GPU|RenderGraph stats"
Get-Content -Path $stderr
```

Expected result: stdout includes model loaded, model instantiated, scene entity ready, GPU mesh upload, and render graph stats; stderr is empty.

- [x] Request exactly one code review using Dalton with this context:

```text
Review Phase 17 of the UE-style Actor Component framework.

Requirements:
- PrefabInstance::RevertAll restores supported root SceneEntity state from the prefab root data.
- PrefabInstance::RevertProperty restores only the requested supported root property.
- Supported root properties are position, rotation, scale, layerMask, and isActive.
- Empty component type, SceneEntity, and Actor are treated as root entity override targets.
- Unsupported single-property overrides are not silently removed.
- No component reflection, component payload merging, nested prefab edits, prefab file I/O, or ApplyToPrefab implementation is introduced.

Please report Critical and Important issues first, with file/line references.
```

- [x] Fix any Critical or Important review findings before committing.

- [x] Update every checkbox in this plan that was completed.

- [x] Commit the implementation:

```powershell
git add Docs\superpowers\plans\2026-05-15-ue-style-actor-component-phase17-prefab-instance-revert.md Scene\Private\Prefab.cpp Tests\ResourceInstantiationValidation\main.cpp
git commit -m "Restore root state from prefab instances"
```

---

## Self-Review

- Spec coverage: This plan makes the existing revert APIs real for root entity state while explicitly keeping the broader property/reflection system out of scope.
- Placeholder scan: The executable steps include exact files, exact code, exact commands, and expected outcomes.
- Type consistency: `PrefabInstance::RevertAll`, `PrefabInstance::RevertProperty`, `PropertyOverride`, `PrefabEntityData`, `SceneEntity::SetPosition`, `SetRotation`, `SetScale`, `SetLayerMask`, and `SetActive` match the current codebase.
