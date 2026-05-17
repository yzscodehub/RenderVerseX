# UE-Style Actor Component Phase 18 Prefab Apply Root State Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make `PrefabInstance::ApplyToPrefab()` commit supported root `SceneEntity` state back into the source prefab.

**Architecture:** Keep apply semantics aligned with Phase 17 revert semantics: only root entity properties already represented in `PrefabEntityData` are supported (`position`, `rotation`, `scale`, `layerMask`, and `isActive`). Applying writes the owner's current root state into the prefab root data, clears only supported root entity overrides, and leaves unsupported overrides untouched for later reflection-based phases.

**Tech Stack:** C++20, RenderVerseX Scene module, standalone `ResourceInstantiationValidation`.

---

## Scope

- Implement root entity state capture for `Prefab`.
- Implement `PrefabInstance::ApplyToPrefab()` for supported root entity properties.
- Treat empty component type, `"SceneEntity"`, and `"Actor"` as root entity override targets.
- Clear root overrides only for supported properties after a successful apply.
- Preserve unsupported component/property overrides.
- No component reflection, component payload merging, child-entity apply, nested prefab edits, or prefab file I/O in this phase.

## Files

- Modify: `E:\WorkSpace\RenderVerseX\Scene\Include\Scene\Prefab.h`
  - Add `bool UpdateRootEntityStateFrom(const SceneEntity& entity);`.
- Modify: `E:\WorkSpace\RenderVerseX\Scene\Private\Prefab.cpp`
  - Add helper functions for supported root property detection, root state capture, and clearing applied root overrides.
  - Implement `Prefab::UpdateRootEntityStateFrom(...)`.
  - Implement `PrefabInstance::ApplyToPrefab()`.
- Modify: `E:\WorkSpace\RenderVerseX\Tests\ResourceInstantiationValidation\main.cpp`
  - Add tests for root state apply, alias cleanup, unsupported override preservation, and no-prefab no-op behavior.
- Modify: `E:\WorkSpace\RenderVerseX\Docs\superpowers\plans\2026-05-15-ue-style-actor-component-phase18-prefab-apply-root-state.md`
  - Track completion status.

---

## Task 1: Add Failing ApplyToPrefab Tests

**Files:**
- Modify: `E:\WorkSpace\RenderVerseX\Tests\ResourceInstantiationValidation\main.cpp`

- [x] Add `Test_PrefabInstanceApplyToPrefabWritesRootEntityState` after `Test_PrefabInstanceRevertPropertyKeepsUnsupportedOverride`:

```cpp
    bool Test_PrefabInstanceApplyToPrefabWritesRootEntityState()
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
        TEST_ASSERT_NOT_NULL(instance);
        auto* prefabInstance = instance->GetComponent<PrefabInstance>();
        TEST_ASSERT_NOT_NULL(prefabInstance);

        instance->SetPosition(Vec3(9.0f, 8.0f, 7.0f));
        instance->SetRotation(Quat(0.7071068f, 0.0f, 0.7071068f, 0.0f));
        instance->SetScale(Vec3(3.0f, 4.0f, 5.0f));
        instance->SetLayerMask(0xAu);
        instance->SetActive(false);
        prefabInstance->AddOverride({"SceneEntity", "position", Vec3(9.0f, 8.0f, 7.0f)});
        prefabInstance->AddOverride({"SceneEntity", "scale", Vec3(3.0f, 4.0f, 5.0f)});

        prefabInstance->ApplyToPrefab();

        const PrefabEntityData* rootData = prefab->GetRootData();
        TEST_ASSERT_NOT_NULL(rootData);
        TEST_ASSERT_EQ(Vec3(9.0f, 8.0f, 7.0f), rootData->position);
        TEST_ASSERT_EQ(Quat(0.7071068f, 0.0f, 0.7071068f, 0.0f), rootData->rotation);
        TEST_ASSERT_EQ(Vec3(3.0f, 4.0f, 5.0f), rootData->scale);
        TEST_ASSERT_EQ(static_cast<uint32_t>(0xAu), rootData->layerMask);
        TEST_ASSERT_FALSE(rootData->isActive);
        TEST_ASSERT_TRUE(prefabInstance->GetOverrides().empty());

        sceneManager.Shutdown();
        return true;
    }
```

- [x] Add `Test_PrefabInstanceApplyToPrefabClearsRootAliasOverrides` after the previous test:

```cpp
    bool Test_PrefabInstanceApplyToPrefabClearsRootAliasOverrides()
    {
        PrefabEntityData data;
        data.name = "ApplyAliases";
        data.position = Vec3(1.0f, 2.0f, 3.0f);
        data.scale = Vec3(1.0f, 1.0f, 1.0f);

        auto prefab = Prefab::CreateFromData({data});

        SceneManager sceneManager;
        sceneManager.Initialize();

        SceneEntity* instance = prefab->Instantiate(sceneManager);
        TEST_ASSERT_NOT_NULL(instance);
        auto* prefabInstance = instance->GetComponent<PrefabInstance>();
        TEST_ASSERT_NOT_NULL(prefabInstance);

        instance->SetPosition(Vec3(5.0f, 6.0f, 7.0f));
        instance->SetScale(Vec3(2.0f, 2.0f, 2.0f));
        prefabInstance->AddOverride({"", "position", Vec3(5.0f, 6.0f, 7.0f)});
        prefabInstance->AddOverride({"Actor", "scale", Vec3(2.0f, 2.0f, 2.0f)});

        prefabInstance->ApplyToPrefab();

        TEST_ASSERT_FALSE(prefabInstance->IsOverridden("", "position"));
        TEST_ASSERT_FALSE(prefabInstance->IsOverridden("Actor", "scale"));
        TEST_ASSERT_TRUE(prefabInstance->GetOverrides().empty());

        const PrefabEntityData* rootData = prefab->GetRootData();
        TEST_ASSERT_NOT_NULL(rootData);
        TEST_ASSERT_EQ(Vec3(5.0f, 6.0f, 7.0f), rootData->position);
        TEST_ASSERT_EQ(Vec3(2.0f, 2.0f, 2.0f), rootData->scale);

        sceneManager.Shutdown();
        return true;
    }
```

- [x] Add `Test_PrefabInstanceApplyToPrefabPreservesUnsupportedOverrides` after the previous test:

```cpp
    bool Test_PrefabInstanceApplyToPrefabPreservesUnsupportedOverrides()
    {
        PrefabEntityData data;
        data.name = "ApplyUnsupported";
        data.position = Vec3(1.0f, 2.0f, 3.0f);

        auto prefab = Prefab::CreateFromData({data});

        SceneManager sceneManager;
        sceneManager.Initialize();

        SceneEntity* instance = prefab->Instantiate(sceneManager);
        TEST_ASSERT_NOT_NULL(instance);
        auto* prefabInstance = instance->GetComponent<PrefabInstance>();
        TEST_ASSERT_NOT_NULL(prefabInstance);

        instance->SetPosition(Vec3(4.0f, 5.0f, 6.0f));
        prefabInstance->AddOverride({"SceneEntity", "position", Vec3(4.0f, 5.0f, 6.0f)});
        prefabInstance->AddOverride({"SceneEntity", "custom", std::string("root-custom")});
        prefabInstance->AddOverride({"UnknownComponent", "payload", std::string("component-custom")});

        prefabInstance->ApplyToPrefab();

        TEST_ASSERT_FALSE(prefabInstance->IsOverridden("SceneEntity", "position"));
        TEST_ASSERT_TRUE(prefabInstance->IsOverridden("SceneEntity", "custom"));
        TEST_ASSERT_TRUE(prefabInstance->IsOverridden("UnknownComponent", "payload"));
        TEST_ASSERT_EQ(static_cast<size_t>(2), prefabInstance->GetOverrides().size());

        const PrefabEntityData* rootData = prefab->GetRootData();
        TEST_ASSERT_NOT_NULL(rootData);
        TEST_ASSERT_EQ(Vec3(4.0f, 5.0f, 6.0f), rootData->position);

        sceneManager.Shutdown();
        return true;
    }
```

- [x] Add `Test_PrefabInstanceApplyToPrefabWithoutPrefabKeepsOverrides` after the previous test:

```cpp
    bool Test_PrefabInstanceApplyToPrefabWithoutPrefabKeepsOverrides()
    {
        SceneManager sceneManager;
        sceneManager.Initialize();

        ActorSpawnParams params;
        params.name = "NoPrefabActor";
        SceneEntity* entity = sceneManager.SpawnActor(params);
        TEST_ASSERT_NOT_NULL(entity);

        auto* prefabInstance = entity->AddComponent<PrefabInstance>();
        TEST_ASSERT_NOT_NULL(prefabInstance);
        prefabInstance->AddOverride({"SceneEntity", "position", Vec3(1.0f, 2.0f, 3.0f)});

        prefabInstance->ApplyToPrefab();

        TEST_ASSERT_TRUE(prefabInstance->IsOverridden("SceneEntity", "position"));
        TEST_ASSERT_EQ(static_cast<size_t>(1), prefabInstance->GetOverrides().size());

        sceneManager.Shutdown();
        return true;
    }
```

- [x] Register the four tests in `main()` after `PrefabInstanceRevertPropertyKeepsUnsupportedOverride`:

```cpp
    suite.AddTest("PrefabInstanceApplyToPrefabWritesRootEntityState",
                  Test_PrefabInstanceApplyToPrefabWritesRootEntityState);
    suite.AddTest("PrefabInstanceApplyToPrefabClearsRootAliasOverrides",
                  Test_PrefabInstanceApplyToPrefabClearsRootAliasOverrides);
    suite.AddTest("PrefabInstanceApplyToPrefabPreservesUnsupportedOverrides",
                  Test_PrefabInstanceApplyToPrefabPreservesUnsupportedOverrides);
    suite.AddTest("PrefabInstanceApplyToPrefabWithoutPrefabKeepsOverrides",
                  Test_PrefabInstanceApplyToPrefabWithoutPrefabKeepsOverrides);
```

- [x] Build the focused target and confirm RED:

```powershell
cmake --build build\Debug --config Debug --target ResourceInstantiationValidation
.\build\Debug\Tests\Debug\ResourceInstantiationValidation.exe
```

Expected result before implementation: the executable builds, and at least the root-state apply and root-alias cleanup tests fail because `ApplyToPrefab()` does not write prefab data or clear applied overrides yet.

---

## Task 2: Add Root State Update API To Prefab

**Files:**
- Modify: `E:\WorkSpace\RenderVerseX\Scene\Include\Scene\Prefab.h`
- Modify: `E:\WorkSpace\RenderVerseX\Scene\Private\Prefab.cpp`

- [x] Add this public method declaration after `GetRootData()` in `Prefab.h`:

```cpp
    /// Update root entity data from a live entity. Returns false when the prefab has no root.
    bool UpdateRootEntityStateFrom(const SceneEntity& entity);
```

- [x] Add this helper inside the anonymous namespace in `Prefab.cpp`:

```cpp
    void CapturePrefabEntityProperties(PrefabEntityData& data, const SceneEntity& entity)
    {
        data.position = entity.GetPosition();
        data.rotation = entity.GetRotation();
        data.scale = entity.GetScale();
        data.layerMask = entity.GetLayerMask();
        data.isActive = entity.IsActive();
    }
```

- [x] Add this implementation after `Prefab::GetRootData()`:

```cpp
bool Prefab::UpdateRootEntityStateFrom(const SceneEntity& entity)
{
    if (m_entities.empty())
    {
        return false;
    }

    CapturePrefabEntityProperties(m_entities[0], entity);
    return true;
}
```

- [x] Build and run focused validation:

```powershell
cmake --build build\Debug --config Debug --target ResourceInstantiationValidation
.\build\Debug\Tests\Debug\ResourceInstantiationValidation.exe
```

Expected result: target builds, but apply tests still fail until `PrefabInstance::ApplyToPrefab()` calls the new API and cleans applied overrides.

---

## Task 3: Implement ApplyToPrefab

**Files:**
- Modify: `E:\WorkSpace\RenderVerseX\Scene\Private\Prefab.cpp`

- [x] Add this helper in the anonymous namespace:

```cpp
    bool IsSupportedPrefabEntityProperty(const std::string& propertyPath)
    {
        return propertyPath == "position" ||
               propertyPath == "rotation" ||
               propertyPath == "scale" ||
               propertyPath == "layerMask" ||
               propertyPath == "isActive";
    }
```

- [x] Leave the existing `ApplyPrefabEntityProperty(...)` setter branches unchanged. Use `IsSupportedPrefabEntityProperty(...)` only from the cleanup helper below.

- [x] Add this helper in the anonymous namespace:

```cpp
    void RemoveSupportedPrefabEntityPropertyOverrides(std::vector<PropertyOverride>& overrides)
    {
        overrides.erase(
            std::remove_if(overrides.begin(), overrides.end(),
                [&](const PropertyOverride& override) {
                    return IsPrefabEntityOverrideTarget(override.componentType) &&
                           IsSupportedPrefabEntityProperty(override.propertyPath);
                }),
            overrides.end()
        );
    }
```

- [x] Replace `PrefabInstance::ApplyToPrefab()` with:

```cpp
void PrefabInstance::ApplyToPrefab()
{
    if (!m_prefab)
    {
        return;
    }

    SceneEntity* owner = GetOwner();
    if (!owner)
    {
        return;
    }

    if (m_prefab->UpdateRootEntityStateFrom(*owner))
    {
        RemoveSupportedPrefabEntityPropertyOverrides(m_overrides);
    }
}
```

- [x] Build and run focused validation:

```powershell
cmake --build build\Debug --config Debug --target ResourceInstantiationValidation
.\build\Debug\Tests\Debug\ResourceInstantiationValidation.exe
```

Expected result: all resource instantiation tests pass, including the four new apply tests.

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
$stdout = "build_codex\phase18_modelviewer_stdout.log"
$stderr = "build_codex\phase18_modelviewer_stderr.log"
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
Review Phase 18 of the UE-style Actor Component framework.

Requirements:
- PrefabInstance::ApplyToPrefab writes supported root SceneEntity state into the source prefab root data.
- Supported root properties are position, rotation, scale, layerMask, and isActive.
- Empty component type, SceneEntity, and Actor are treated as root entity override targets.
- Applying clears only supported root entity overrides after a successful root update.
- Unsupported component/property overrides are preserved.
- No component reflection, component payload merging, child-entity apply, nested prefab edits, or prefab file I/O is introduced.

Please report Critical and Important issues first, with file/line references.
```

- [x] Fix any Critical or Important review findings before committing.

- [x] Update every checkbox in this plan that was completed.

- [x] Commit the implementation:

```powershell
git add Docs\superpowers\plans\2026-05-15-ue-style-actor-component-phase18-prefab-apply-root-state.md Scene\Include\Scene\Prefab.h Scene\Private\Prefab.cpp Tests\ResourceInstantiationValidation\main.cpp
git commit -m "Apply prefab instance root state"
```

---

## Self-Review

- Spec coverage: The plan gives `ApplyToPrefab()` a real, scoped behavior matching the root state supported by Phase 17 revert.
- Placeholder scan: The executable steps include exact files, exact code, exact commands, and expected outcomes.
- Type consistency: `Prefab::UpdateRootEntityStateFrom`, `PrefabInstance::ApplyToPrefab`, `PrefabEntityData`, `SceneEntity`, `PropertyOverride`, and root property names match the current codebase.
