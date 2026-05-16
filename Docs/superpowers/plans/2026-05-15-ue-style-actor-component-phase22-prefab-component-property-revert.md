# UE-Style Actor Component Phase 22 Prefab Component Property Revert Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make `PrefabInstance::RevertProperty(componentType, "serializedData")` restore root component serialized payloads for existing components.

**Architecture:** Keep the current opaque component payload model. `RevertProperty(...)` will continue to handle root entity state through the existing explicit property path table, and will additionally handle a conventional component payload property named `"serializedData"` by finding a matching root legacy `Component` or actor-only `ActorComponent` and calling `DeserializePrefabData(...)`. Missing components are not created and duplicate same-class actor components remain out of scope until prefab overrides have stable component instance identifiers.

**Tech Stack:** C++20, RenderVerseX Scene module, standalone `ResourceInstantiationValidation`.

---

## Scope

- `PrefabInstance::RevertProperty("PrefabPayloadComponent", "serializedData")` restores the first matching existing root actor-only component payload from `PrefabEntityData::actorComponentData`.
- `PrefabInstance::RevertProperty("PrefabLegacyPayloadComponent", "serializedData")` restores an existing root legacy component payload from `PrefabEntityData::componentData`.
- Successful component payload property revert removes only the exact matching component override.
- Missing live components do not get recreated and their override records stay intact.
- Root entity property revert behavior remains unchanged.
- Do not support child entity component payload revert in this phase.
- Do not support duplicate same-class actor component addressing in this phase.
- Do not add reflection or field-level property deserialization.

## Files

- Modify: `E:\WorkSpace\RenderVerseX\Tests\ResourceInstantiationValidation\main.cpp`
  - Add focused tests for actor-only, legacy, and missing-component component payload property revert.
- Modify: `E:\WorkSpace\RenderVerseX\Scene\Private\Prefab.cpp`
  - Add a helper that reverts exactly one root component payload by `componentType`.
  - Extend `PrefabInstance::RevertProperty(...)` to call that helper for `"serializedData"`.
- Modify: `E:\WorkSpace\RenderVerseX\Docs\superpowers\plans\2026-05-15-ue-style-actor-component-phase22-prefab-component-property-revert.md`
  - Track completion status.

---

## Task 1: Add Failing Component Payload RevertProperty Tests

**Files:**
- Modify: `E:\WorkSpace\RenderVerseX\Tests\ResourceInstantiationValidation\main.cpp`

- [ ] **Step 1: Add actor component payload property revert test**

Add this test after `Test_PrefabInstanceRevertPropertyRestoresOnlyRequestedEntityProperty`:

```cpp
    bool Test_PrefabInstanceRevertPropertyRestoresActorComponentPayload()
    {
        ComponentFactoryClassGuard componentFactoryGuard;
        ComponentFactory::RegisterComponentClass<PrefabPayloadComponent>(
            "PrefabPayloadComponent");
        ComponentFactory::RegisterComponentClass<PrefabLegacyPayloadComponent>(
            "PrefabLegacyPayloadComponent");

        PrefabEntityData data;
        data.name = "RevertPropertyActorPayloadRoot";
        data.actorComponentData.push_back({"PrefabPayloadComponent", "ammo=12;mode=burst"});
        data.componentData["PrefabLegacyPayloadComponent"] = "legacy=enabled";

        auto prefab = Prefab::CreateFromData({data});

        SceneManager sceneManager;
        sceneManager.Initialize();

        SceneEntity* instance = prefab->Instantiate(sceneManager);
        TEST_ASSERT_NOT_NULL(instance);
        auto* prefabInstance = instance->GetComponent<PrefabInstance>();
        TEST_ASSERT_NOT_NULL(prefabInstance);

        auto* actorComponent = static_cast<Actor*>(instance)->GetComponent<PrefabPayloadComponent>();
        TEST_ASSERT_NOT_NULL(actorComponent);
        auto* legacyComponent = instance->GetComponent<PrefabLegacyPayloadComponent>();
        TEST_ASSERT_NOT_NULL(legacyComponent);

        actorComponent->payload = "ammo=1;mode=single";
        legacyComponent->payload = "legacy=disabled";
        prefabInstance->AddOverride({"PrefabPayloadComponent", "serializedData", actorComponent->payload});
        prefabInstance->AddOverride({"PrefabLegacyPayloadComponent", "serializedData", legacyComponent->payload});

        prefabInstance->RevertProperty("PrefabPayloadComponent", "serializedData");

        TEST_ASSERT_EQ(std::string("ammo=12;mode=burst"), actorComponent->payload);
        TEST_ASSERT_EQ(std::string("legacy=disabled"), legacyComponent->payload);
        TEST_ASSERT_FALSE(prefabInstance->IsOverridden("PrefabPayloadComponent", "serializedData"));
        TEST_ASSERT_TRUE(prefabInstance->IsOverridden("PrefabLegacyPayloadComponent", "serializedData"));
        TEST_ASSERT_EQ(static_cast<size_t>(1), prefabInstance->GetOverrides().size());

        sceneManager.Shutdown();
        return true;
    }
```

- [ ] **Step 2: Add legacy component payload property revert test**

Add this test after the actor component payload property revert test:

```cpp
    bool Test_PrefabInstanceRevertPropertyRestoresLegacyComponentPayload()
    {
        ComponentFactoryClassGuard componentFactoryGuard;
        ComponentFactory::RegisterComponentClass<PrefabPayloadComponent>(
            "PrefabPayloadComponent");
        ComponentFactory::RegisterComponentClass<PrefabLegacyPayloadComponent>(
            "PrefabLegacyPayloadComponent");

        PrefabEntityData data;
        data.name = "RevertPropertyLegacyPayloadRoot";
        data.actorComponentData.push_back({"PrefabPayloadComponent", "ammo=12"});
        data.componentData["PrefabLegacyPayloadComponent"] = "legacy=enabled";

        auto prefab = Prefab::CreateFromData({data});

        SceneManager sceneManager;
        sceneManager.Initialize();

        SceneEntity* instance = prefab->Instantiate(sceneManager);
        TEST_ASSERT_NOT_NULL(instance);
        auto* prefabInstance = instance->GetComponent<PrefabInstance>();
        TEST_ASSERT_NOT_NULL(prefabInstance);

        auto* actorComponent = static_cast<Actor*>(instance)->GetComponent<PrefabPayloadComponent>();
        TEST_ASSERT_NOT_NULL(actorComponent);
        auto* legacyComponent = instance->GetComponent<PrefabLegacyPayloadComponent>();
        TEST_ASSERT_NOT_NULL(legacyComponent);

        actorComponent->payload = "ammo=99";
        legacyComponent->payload = "legacy=disabled";
        prefabInstance->AddOverride({"PrefabPayloadComponent", "serializedData", actorComponent->payload});
        prefabInstance->AddOverride({"PrefabLegacyPayloadComponent", "serializedData", legacyComponent->payload});

        prefabInstance->RevertProperty("PrefabLegacyPayloadComponent", "serializedData");

        TEST_ASSERT_EQ(std::string("ammo=99"), actorComponent->payload);
        TEST_ASSERT_EQ(std::string("legacy=enabled"), legacyComponent->payload);
        TEST_ASSERT_TRUE(prefabInstance->IsOverridden("PrefabPayloadComponent", "serializedData"));
        TEST_ASSERT_FALSE(prefabInstance->IsOverridden("PrefabLegacyPayloadComponent", "serializedData"));
        TEST_ASSERT_EQ(static_cast<size_t>(1), prefabInstance->GetOverrides().size());

        sceneManager.Shutdown();
        return true;
    }
```

- [ ] **Step 3: Add missing component payload property revert test**

Add this test after the legacy component payload property revert test:

```cpp
    bool Test_PrefabInstanceRevertPropertyKeepsPayloadOverrideWhenComponentMissing()
    {
        ComponentFactoryClassGuard componentFactoryGuard;
        ComponentFactory::RegisterComponentClass<PrefabPayloadComponent>(
            "PrefabPayloadComponent");

        PrefabEntityData data;
        data.name = "MissingPayloadComponentRoot";
        data.actorComponentData.push_back({"PrefabPayloadComponent", "ammo=12"});

        auto prefab = Prefab::CreateFromData({data});

        SceneManager sceneManager;
        sceneManager.Initialize();

        SceneEntity* instance = prefab->Instantiate(sceneManager);
        TEST_ASSERT_NOT_NULL(instance);
        auto* prefabInstance = instance->GetComponent<PrefabInstance>();
        TEST_ASSERT_NOT_NULL(prefabInstance);

        TEST_ASSERT_TRUE(static_cast<Actor*>(instance)->RemoveComponent<PrefabPayloadComponent>());
        TEST_ASSERT_NULL(static_cast<Actor*>(instance)->GetComponent<PrefabPayloadComponent>());
        prefabInstance->AddOverride({"PrefabPayloadComponent", "serializedData", std::string("ammo=99")});

        prefabInstance->RevertProperty("PrefabPayloadComponent", "serializedData");

        TEST_ASSERT_NULL(static_cast<Actor*>(instance)->GetComponent<PrefabPayloadComponent>());
        TEST_ASSERT_TRUE(prefabInstance->IsOverridden("PrefabPayloadComponent", "serializedData"));
        TEST_ASSERT_EQ(static_cast<size_t>(1), prefabInstance->GetOverrides().size());

        sceneManager.Shutdown();
        return true;
    }
```

- [ ] **Step 4: Register the three tests**

Register them in `main()` after `PrefabInstanceRevertPropertyRestoresOnlyRequestedEntityProperty`:

```cpp
    suite.AddTest("PrefabInstanceRevertPropertyRestoresActorComponentPayload",
                  Test_PrefabInstanceRevertPropertyRestoresActorComponentPayload);
    suite.AddTest("PrefabInstanceRevertPropertyRestoresLegacyComponentPayload",
                  Test_PrefabInstanceRevertPropertyRestoresLegacyComponentPayload);
    suite.AddTest("PrefabInstanceRevertPropertyKeepsPayloadOverrideWhenComponentMissing",
                  Test_PrefabInstanceRevertPropertyKeepsPayloadOverrideWhenComponentMissing);
```

- [ ] **Step 5: Build and confirm RED**

Run:

```powershell
cmake --build build\Debug --config Debug --target ResourceInstantiationValidation
.\build\Debug\Tests\Debug\ResourceInstantiationValidation.exe
```

Expected before implementation: the two payload property revert tests fail because `PrefabInstance::RevertProperty(...)` currently ignores non-root component targets. The missing-component test may pass before implementation; that is acceptable because it guards the no-recreate/no-clear behavior.

---

## Task 2: Implement Single Root Component Payload Revert

**Files:**
- Modify: `E:\WorkSpace\RenderVerseX\Scene\Private\Prefab.cpp`

- [ ] **Step 1: Add actor component payload lookup helper**

Add this helper after `ApplyPrefabEntityComponentPayloads(...)`:

```cpp
    bool ApplyPrefabEntityComponentPayload(SceneEntity& owner,
                                           const PrefabEntityData& data,
                                           const std::string& componentType)
    {
        auto legacyIt = data.componentData.find(componentType);
        if (legacyIt != data.componentData.end())
        {
            if (Component* component = FindLegacyComponentByTypeName(owner, componentType))
            {
                component->DeserializePrefabData(legacyIt->second);
                return true;
            }
            return false;
        }

        for (const auto& componentData : data.actorComponentData)
        {
            if (componentData.className != componentType)
            {
                continue;
            }

            auto matches = FindActorComponentsByClassName(owner, componentType);
            if (matches.empty())
            {
                return false;
            }

            matches[0]->DeserializePrefabData(componentData.serializedData);
            return true;
        }

        return false;
    }
```

- [ ] **Step 2: Extend `PrefabInstance::RevertProperty(...)`**

Replace the unsupported-target early return block with component payload handling:

```cpp
    if (!IsPrefabEntityOverrideTarget(componentType))
    {
        if (propertyPath == "serializedData" &&
            ApplyPrefabEntityComponentPayload(*owner, *rootData, componentType))
        {
            RemoveOverride(componentType, propertyPath);
        }
        return;
    }
```

Keep the existing root entity property logic unchanged below this block.

- [ ] **Step 3: Build and run focused validation**

Run:

```powershell
cmake --build build\Debug --config Debug --target ResourceInstantiationValidation
.\build\Debug\Tests\Debug\ResourceInstantiationValidation.exe
```

Expected result: all `ResourceInstantiationValidation` tests pass, including the three new component payload property revert tests.

---

## Task 3: Validate, Review, And Commit

**Files:**
- No additional source files.

- [ ] **Step 1: Run whitespace check**

```powershell
git diff --check
```

Expected result: exit code 0. Line-ending warnings are acceptable if no whitespace-error lines are reported.

- [ ] **Step 2: Build all validation targets and ModelViewer sequentially**

```powershell
foreach ($target in @('ActorComponentValidation','SpatialComponentValidation','ResourceInstantiationValidation','RenderSceneValidation','SystemIntegrationTest','MaterialSystemValidation','RenderPassBindingValidation','ModelViewer')) {
    Write-Host "=== Building $target ==="
    cmake --build build\Debug --config Debug --target $target
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}
```

Expected result: every target builds.

- [ ] **Step 3: Run validation executables**

```powershell
$tests = @(
    '.\build\Debug\Tests\Debug\ActorComponentValidation.exe',
    '.\build\Debug\Tests\Debug\SpatialComponentValidation.exe',
    '.\build\Debug\Tests\Debug\ResourceInstantiationValidation.exe',
    '.\build\Debug\Tests\Debug\RenderSceneValidation.exe',
    '.\build\Debug\Tests\Debug\SystemIntegrationTest.exe',
    '.\build\Debug\Tests\Debug\MaterialSystemValidation.exe',
    '.\build\Debug\Tests\Debug\RenderPassBindingValidation.exe'
)
foreach ($test in $tests) {
    Write-Host "=== Running $test ==="
    & $test
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}
```

Expected result: every executable returns exit code 0 and reports all tests passing.

- [ ] **Step 4: Run ModelViewer smoke**

```powershell
$stdout = "build_codex\phase22_modelviewer_stdout.log"
$stderr = "build_codex\phase22_modelviewer_stderr.log"
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

- [ ] **Step 5: Request exactly one code review using Dalton**

Use this review context:

```text
Review Phase 22 of the UE-style Actor Component framework.

Requirements:
- PrefabInstance::RevertProperty(componentType, "serializedData") restores root actor-only ActorComponent serialized payloads when the live component exists.
- PrefabInstance::RevertProperty(componentType, "serializedData") restores root legacy Component serialized payloads when the live component exists.
- Successful component payload property revert removes only the exact component serializedData override.
- Missing components are not created during revert and their overrides stay intact.
- Root entity property revert behavior remains unchanged.
- Child entity component revert, duplicate same-class component addressing, and field-level reflection remain out of scope.

Please report only Critical and Important issues, with file/line references.
```

- [ ] **Step 6: Fix any Critical or Important review findings**

If Dalton reports no Critical or Important findings, record that no fix was required and continue.

- [ ] **Step 7: Update every completed checkbox in this plan**

Mark every completed `- [ ]` as `- [x]` before committing.

- [ ] **Step 8: Commit the implementation**

```powershell
git add Docs\superpowers\plans\2026-05-15-ue-style-actor-component-phase22-prefab-component-property-revert.md Scene\Private\Prefab.cpp Tests\ResourceInstantiationValidation\main.cpp
git commit -m "Revert prefab component payload properties"
```

---

## Self-Review

- Spec coverage: The plan closes the single-property gap left after Phase 21 by adding component payload handling to `PrefabInstance::RevertProperty(...)`.
- Placeholder scan: All code-changing steps include exact files and concrete code blocks; validation and commit commands are explicit.
- Type consistency: `PrefabInstance::RevertProperty`, `PropertyOverride`, `PrefabEntityData::componentData`, `PrefabEntityData::actorComponentData`, `ActorComponent`, `Component`, `DeserializePrefabData`, and `"serializedData"` match current code.
- Scope check: Duplicate same-class actor components are explicitly deferred because `PropertyOverride` has no component instance identifier yet.
