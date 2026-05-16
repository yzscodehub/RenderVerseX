# UE-Style Actor Component Phase 21 Prefab Root Component Payload Apply/Revert Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make `PrefabInstance::RevertAll()` and `PrefabInstance::ApplyToPrefab()` include root component serialized payloads, not just root entity transform/layer/active state.

**Architecture:** Treat component prefab data as opaque serialized payload strings owned by each component class. Revert applies the root prefab's `componentData` and `actorComponentData` back into matching live root components through `DeserializePrefabData()`, while apply captures current live root component payloads through `SerializePrefabData()` back into the prefab root data. This phase does not create missing components, delete extra components, recurse into child entities, or interpret property paths beyond the existing root entity properties and a conventional component `"serializedData"` override marker.

**Tech Stack:** C++20, RenderVerseX Scene module, standalone `ResourceInstantiationValidation`.

---

## Scope

- `PrefabInstance::RevertAll()` restores payloads for existing root legacy `Component` instances listed in `PrefabEntityData::componentData`.
- `PrefabInstance::RevertAll()` restores payloads for existing root actor-only `ActorComponent` instances listed in `PrefabEntityData::actorComponentData`.
- `PrefabInstance::ApplyToPrefab()` writes current root legacy and actor-only component payloads into the prefab root data.
- Applying component payloads clears root component overrides whose `propertyPath` is `"serializedData"` and whose component type is present in the updated root prefab data.
- `Prefab::CreateFromEntity(...)` and root update share the same component capture helper so serialization behavior stays consistent.
- Do not create missing components during revert.
- Do not remove extra live components during revert or apply.
- Do not support child entity component apply/revert in this phase.
- Do not add reflection or field-level property deserialization.

## Files

- Modify: `E:\WorkSpace\RenderVerseX\Tests\ResourceInstantiationValidation\main.cpp`
  - Add tests for `RevertAll()` restoring actor and legacy root component payloads.
  - Add tests for `ApplyToPrefab()` writing actor and legacy root component payloads and clearing supported component overrides.
- Modify: `E:\WorkSpace\RenderVerseX\Scene\Private\Prefab.cpp`
  - Add helper functions to capture root component payloads.
  - Add helper functions to apply root component payloads to matching live components.
  - Extend `Prefab::UpdateRootEntityStateFrom(...)`.
  - Extend `PrefabInstance::RevertAll()` and `PrefabInstance::ApplyToPrefab()`.
- Modify: `E:\WorkSpace\RenderVerseX\Docs\superpowers\plans\2026-05-15-ue-style-actor-component-phase21-prefab-root-component-payloads.md`
  - Track completion status.

---

## Task 1: Add Failing Prefab Root Component Payload Tests

**Files:**
- Modify: `E:\WorkSpace\RenderVerseX\Tests\ResourceInstantiationValidation\main.cpp`

- [x] **Step 1: Add `RevertAll()` actor component payload test**

Add this test after `Test_PrefabInstanceRevertAllRestoresRootEntityState`:

```cpp
    bool Test_PrefabInstanceRevertAllRestoresActorComponentPayload()
    {
        ComponentFactoryClassGuard componentFactoryGuard;
        ComponentFactory::RegisterComponentClass<PrefabPayloadComponent>(
            "PrefabPayloadComponent");

        PrefabEntityData data;
        data.name = "RevertActorPayloadRoot";
        data.actorComponentData.push_back({"PrefabPayloadComponent", "ammo=12;mode=burst"});

        auto prefab = Prefab::CreateFromData({data});

        SceneManager sceneManager;
        sceneManager.Initialize();

        SceneEntity* instance = prefab->Instantiate(sceneManager);
        TEST_ASSERT_NOT_NULL(instance);
        auto* prefabInstance = instance->GetComponent<PrefabInstance>();
        TEST_ASSERT_NOT_NULL(prefabInstance);

        auto* component = static_cast<Actor*>(instance)->GetComponent<PrefabPayloadComponent>();
        TEST_ASSERT_NOT_NULL(component);
        component->payload = "ammo=1;mode=single";
        prefabInstance->AddOverride({"PrefabPayloadComponent", "serializedData", component->payload});

        prefabInstance->RevertAll();

        TEST_ASSERT_EQ(std::string("ammo=12;mode=burst"), component->payload);
        TEST_ASSERT_TRUE(prefabInstance->GetOverrides().empty());

        sceneManager.Shutdown();
        return true;
    }
```

- [x] **Step 2: Add `RevertAll()` legacy component payload test**

Add this test after the actor payload revert test:

```cpp
    bool Test_PrefabInstanceRevertAllRestoresLegacyComponentPayload()
    {
        ComponentFactoryClassGuard componentFactoryGuard;
        ComponentFactory::RegisterComponentClass<PrefabLegacyPayloadComponent>(
            "PrefabLegacyPayloadComponent");

        PrefabEntityData data;
        data.name = "RevertLegacyPayloadRoot";
        data.componentData["PrefabLegacyPayloadComponent"] = "legacy=enabled";

        auto prefab = Prefab::CreateFromData({data});

        SceneManager sceneManager;
        sceneManager.Initialize();

        SceneEntity* instance = prefab->Instantiate(sceneManager);
        TEST_ASSERT_NOT_NULL(instance);
        auto* prefabInstance = instance->GetComponent<PrefabInstance>();
        TEST_ASSERT_NOT_NULL(prefabInstance);

        auto* component = instance->GetComponent<PrefabLegacyPayloadComponent>();
        TEST_ASSERT_NOT_NULL(component);
        component->payload = "legacy=disabled";
        prefabInstance->AddOverride({"PrefabLegacyPayloadComponent", "serializedData", component->payload});

        prefabInstance->RevertAll();

        TEST_ASSERT_EQ(std::string("legacy=enabled"), component->payload);
        TEST_ASSERT_TRUE(prefabInstance->GetOverrides().empty());

        sceneManager.Shutdown();
        return true;
    }
```

- [x] **Step 3: Add `ApplyToPrefab()` component payload test**

Add this test after `Test_PrefabInstanceApplyToPrefabWritesRootEntityState`:

```cpp
    bool Test_PrefabInstanceApplyToPrefabWritesRootComponentPayloads()
    {
        ComponentFactoryClassGuard componentFactoryGuard;
        ComponentFactory::RegisterComponentClass<PrefabPayloadComponent>(
            "PrefabPayloadComponent");
        ComponentFactory::RegisterComponentClass<PrefabLegacyPayloadComponent>(
            "PrefabLegacyPayloadComponent");

        PrefabEntityData data;
        data.name = "ApplyComponentPayloadsRoot";
        data.actorComponentData.push_back({"PrefabPayloadComponent", "ammo=12"});
        data.componentData["PrefabLegacyPayloadComponent"] = "legacy=old";

        auto prefab = Prefab::CreateFromData({data});

        SceneManager sceneManager;
        sceneManager.Initialize();

        SceneEntity* instance = prefab->Instantiate(sceneManager);
        TEST_ASSERT_NOT_NULL(instance);
        auto* prefabInstance = instance->GetComponent<PrefabInstance>();
        TEST_ASSERT_NOT_NULL(prefabInstance);

        auto* actorComponent = static_cast<Actor*>(instance)->GetComponent<PrefabPayloadComponent>();
        TEST_ASSERT_NOT_NULL(actorComponent);
        actorComponent->payload = "ammo=99;mode=auto";

        auto* legacyComponent = instance->GetComponent<PrefabLegacyPayloadComponent>();
        TEST_ASSERT_NOT_NULL(legacyComponent);
        legacyComponent->payload = "legacy=new";

        prefabInstance->AddOverride({"PrefabPayloadComponent", "serializedData", actorComponent->payload});
        prefabInstance->AddOverride({"PrefabLegacyPayloadComponent", "serializedData", legacyComponent->payload});
        prefabInstance->AddOverride({"UnknownComponent", "serializedData", std::string("keep")});

        prefabInstance->ApplyToPrefab();

        const PrefabEntityData* rootData = prefab->GetRootData();
        TEST_ASSERT_NOT_NULL(rootData);
        TEST_ASSERT_EQ(static_cast<size_t>(1), rootData->actorComponentData.size());
        TEST_ASSERT_EQ(std::string("PrefabPayloadComponent"),
                       rootData->actorComponentData[0].className);
        TEST_ASSERT_EQ(std::string("ammo=99;mode=auto"),
                       rootData->actorComponentData[0].serializedData);

        auto it = rootData->componentData.find("PrefabLegacyPayloadComponent");
        TEST_ASSERT_TRUE(it != rootData->componentData.end());
        TEST_ASSERT_EQ(std::string("legacy=new"), it->second);

        TEST_ASSERT_FALSE(prefabInstance->IsOverridden("PrefabPayloadComponent", "serializedData"));
        TEST_ASSERT_FALSE(prefabInstance->IsOverridden("PrefabLegacyPayloadComponent", "serializedData"));
        TEST_ASSERT_TRUE(prefabInstance->IsOverridden("UnknownComponent", "serializedData"));

        sceneManager.Shutdown();
        return true;
    }
```

- [x] **Step 4: Register the three tests**

Register them in `main()` near the existing prefab instance tests:

```cpp
    suite.AddTest("PrefabInstanceRevertAllRestoresActorComponentPayload",
                  Test_PrefabInstanceRevertAllRestoresActorComponentPayload);
    suite.AddTest("PrefabInstanceRevertAllRestoresLegacyComponentPayload",
                  Test_PrefabInstanceRevertAllRestoresLegacyComponentPayload);
    suite.AddTest("PrefabInstanceApplyToPrefabWritesRootComponentPayloads",
                  Test_PrefabInstanceApplyToPrefabWritesRootComponentPayloads);
```

- [x] **Step 5: Build and confirm RED**

Run:

```powershell
cmake --build build\Debug --config Debug --target ResourceInstantiationValidation
.\build\Debug\Tests\Debug\ResourceInstantiationValidation.exe
```

Expected before implementation: the new revert/apply component payload tests fail because `PrefabInstance::RevertAll()` currently only restores root entity state and `ApplyToPrefab()` currently only writes root entity state.

---

## Task 2: Capture And Apply Root Component Payloads

**Files:**
- Modify: `E:\WorkSpace\RenderVerseX\Scene\Private\Prefab.cpp`

- [x] **Step 1: Add component capture helper**

Add this helper in the anonymous namespace after `CapturePrefabEntityProperties(...)`:

```cpp
    void CapturePrefabEntityComponents(PrefabEntityData& data, const SceneEntity& entity)
    {
        data.componentData.clear();
        data.actorComponentData.clear();

        for (const auto& [typeIndex, component] : entity.GetComponents())
        {
            (void)typeIndex;
            if (!component)
                continue;

            if (std::string(component->GetTypeName()) == "PrefabInstance")
                continue;

            data.componentData[component->GetTypeName()] = component->SerializePrefabData();
        }

        for (const auto& actorComponent : entity.GetActorComponents())
        {
            if (!actorComponent)
                continue;

            const std::string className = actorComponent->GetClassName();
            if (className == "SceneComponent" || className == "PrefabInstance")
                continue;

            if (dynamic_cast<Component*>(actorComponent.get()))
                continue;

            data.actorComponentData.push_back({className, actorComponent->SerializePrefabData()});
        }
    }
```

- [x] **Step 2: Add payload apply helpers**

Add these helpers after the capture helper:

```cpp
    Component* FindLegacyComponentByTypeName(SceneEntity& owner, const std::string& typeName)
    {
        for (const auto& [typeIndex, component] : owner.GetComponents())
        {
            (void)typeIndex;
            if (component && component->GetTypeName() == typeName)
            {
                return component.get();
            }
        }
        return nullptr;
    }

    std::vector<ActorComponent*> FindActorComponentsByClassName(SceneEntity& owner,
                                                                 const std::string& className)
    {
        std::vector<ActorComponent*> matches;
        for (const auto& component : owner.GetActorComponents())
        {
            if (!component)
                continue;

            if (dynamic_cast<Component*>(component.get()))
                continue;

            if (component->GetClassName() == className)
            {
                matches.push_back(component.get());
            }
        }
        return matches;
    }

    void ApplyPrefabEntityComponentPayloads(SceneEntity& owner, const PrefabEntityData& data)
    {
        for (const auto& [typeName, serializedData] : data.componentData)
        {
            if (Component* component = FindLegacyComponentByTypeName(owner, typeName))
            {
                component->DeserializePrefabData(serializedData);
            }
        }

        std::unordered_map<std::string, size_t> consumedActorComponents;
        for (const auto& componentData : data.actorComponentData)
        {
            auto matches = FindActorComponentsByClassName(owner, componentData.className);
            size_t& consumed = consumedActorComponents[componentData.className];
            if (consumed < matches.size())
            {
                matches[consumed]->DeserializePrefabData(componentData.serializedData);
            }
            ++consumed;
        }
    }
```

- [x] **Step 3: Add override cleanup helper**

Add this helper after `RemoveSupportedPrefabEntityPropertyOverrides(...)`:

```cpp
    bool HasPrefabRootComponentPayload(const PrefabEntityData& data, const std::string& componentType)
    {
        if (data.componentData.find(componentType) != data.componentData.end())
        {
            return true;
        }

        return std::any_of(data.actorComponentData.begin(), data.actorComponentData.end(),
            [&](const PrefabActorComponentData& componentData) {
                return componentData.className == componentType;
            });
    }

    void RemoveSupportedRootComponentPayloadOverrides(std::vector<PropertyOverride>& overrides,
                                                      const PrefabEntityData& data)
    {
        overrides.erase(
            std::remove_if(overrides.begin(), overrides.end(),
                [&](const PropertyOverride& override) {
                    return override.propertyPath == "serializedData" &&
                           HasPrefabRootComponentPayload(data, override.componentType);
                }),
            overrides.end()
        );
    }
```

- [x] **Step 4: Reuse component capture in `SerializeEntity(...)`**

Replace the legacy and actor component serialization loops in `Prefab::SerializeEntity(...)` with:

```cpp
    CapturePrefabEntityComponents(data, *entity);
```

- [x] **Step 5: Extend `Prefab::UpdateRootEntityStateFrom(...)`**

Update the method body to capture component payloads:

```cpp
bool Prefab::UpdateRootEntityStateFrom(const SceneEntity& entity)
{
    if (m_entities.empty())
    {
        return false;
    }

    CapturePrefabEntityProperties(m_entities[0], entity);
    CapturePrefabEntityComponents(m_entities[0], entity);
    return true;
}
```

- [x] **Step 6: Extend `PrefabInstance::RevertAll()`**

After `ApplyAllPrefabEntityProperties(owner, *rootData);`, add:

```cpp
    ApplyPrefabEntityComponentPayloads(*owner, *rootData);
```

Keep `m_overrides.clear();` after both root state and component payloads are reverted.

- [x] **Step 7: Extend `PrefabInstance::ApplyToPrefab()`**

Replace the current `if` body with:

```cpp
    if (m_prefab->UpdateRootEntityStateFrom(*owner))
    {
        const PrefabEntityData* rootData = m_prefab->GetRootData();
        RemoveSupportedPrefabEntityPropertyOverrides(m_overrides);
        if (rootData)
        {
            RemoveSupportedRootComponentPayloadOverrides(m_overrides, *rootData);
        }
    }
```

- [x] **Step 8: Build and run focused validation**

Run:

```powershell
cmake --build build\Debug --config Debug --target ResourceInstantiationValidation
.\build\Debug\Tests\Debug\ResourceInstantiationValidation.exe
```

Expected result: all `ResourceInstantiationValidation` tests pass, including the three new root component payload apply/revert tests.

---

## Task 3: Validate, Review, And Commit

**Files:**
- No additional source files.

- [x] **Step 1: Run whitespace check**

```powershell
git diff --check
```

Expected result: exit code 0. Line-ending warnings are acceptable if no whitespace-error lines are reported.

- [x] **Step 2: Build all validation targets and ModelViewer sequentially**

```powershell
foreach ($target in @('ActorComponentValidation','SpatialComponentValidation','ResourceInstantiationValidation','RenderSceneValidation','SystemIntegrationTest','MaterialSystemValidation','RenderPassBindingValidation','ModelViewer')) {
    Write-Host "=== Building $target ==="
    cmake --build build\Debug --config Debug --target $target
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}
```

Expected result: every target builds.

- [x] **Step 3: Run validation executables**

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

- [x] **Step 4: Run ModelViewer smoke**

```powershell
$stdout = "build_codex\phase21_modelviewer_stdout.log"
$stderr = "build_codex\phase21_modelviewer_stderr.log"
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

- [x] **Step 5: Request exactly one code review using Dalton**

Use this review context:

```text
Review Phase 21 of the UE-style Actor Component framework.

Requirements:
- PrefabInstance::RevertAll restores root legacy Component serialized payloads.
- PrefabInstance::RevertAll restores root actor-only ActorComponent serialized payloads.
- PrefabInstance::ApplyToPrefab captures root legacy and actor-only component payloads into the prefab root data.
- ApplyToPrefab clears supported root component serializedData overrides but preserves unsupported overrides.
- Missing components are not created during revert.
- Child entity component apply/revert and field-level reflection remain out of scope.

Please report only Critical and Important issues, with file/line references.
```

- [x] **Step 6: Fix any Critical or Important review findings**

If Dalton reports no Critical or Important findings, record that no fix was required and continue.

- [x] **Step 7: Update every completed checkbox in this plan**

Mark every completed `- [ ]` as `- [x]` before committing.

- [x] **Step 8: Commit the implementation**

```powershell
git add Docs\superpowers\plans\2026-05-15-ue-style-actor-component-phase21-prefab-root-component-payloads.md Scene\Private\Prefab.cpp Tests\ResourceInstantiationValidation\main.cpp
git commit -m "Apply prefab root component payloads"
```

---

## Self-Review

- Spec coverage: This plan connects Phase 19's restored component payload serialization to Phase 17/18's instance apply/revert workflow for root components.
- Placeholder scan: All code-changing steps include exact files and concrete code blocks; validation and commit commands are explicit.
- Type consistency: `PrefabEntityData::componentData`, `PrefabEntityData::actorComponentData`, `PrefabActorComponentData`, `ActorComponent`, `Component`, `SceneEntity`, `PrefabInstance::RevertAll`, and `PrefabInstance::ApplyToPrefab` match current code.
- Risk check: The plan intentionally avoids structural prefab edits during revert; existing components receive payload updates, missing components stay missing, and child entity payloads remain out of scope for a later phase.
