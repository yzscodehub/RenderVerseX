# UE-Style Actor Component Phase 19 Prefab Legacy Component Data Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Preserve legacy `Component` subclasses through prefab serialization and instantiation using the existing `PrefabEntityData::componentData` map.

**Architecture:** Keep legacy component support explicit and type-safe: prefab `componentData` may only reconstruct registered classes that actually derive from legacy `Component`. Actor-only components remain in `actorComponentData`, and any missing or mismatched legacy component class makes prefab instantiation fail atomically with already-created entities cleaned up.

**Tech Stack:** C++20, RenderVerseX Scene module, standalone `ResourceInstantiationValidation`.

---

## Scope

- Serialize legacy `Component` payloads into `PrefabEntityData::componentData`.
- Instantiate registered legacy `Component` classes from `componentData`.
- Replay legacy component prefab payload before `OnAttach()`.
- Fail atomically when a `componentData` entry is missing from the class registry.
- Fail atomically when a `componentData` entry resolves to an actor-only component instead of legacy `Component`.
- Do not add reflection, file I/O, duplicate legacy component support, or child-specific apply/revert behavior in this phase.

## Files

- Modify: `E:\WorkSpace\RenderVerseX\Scene\Include\Scene\SceneEntity.h`
  - Add `Component* AddOwnedComponent(std::unique_ptr<Component> component);` for type-erased legacy component insertion.
- Modify: `E:\WorkSpace\RenderVerseX\Scene\Private\SceneEntity.cpp`
  - Implement `SceneEntity::AddOwnedComponent(...)` with the same lifecycle order as `AddComponent<T>()`.
- Modify: `E:\WorkSpace\RenderVerseX\Scene\Private\Prefab.cpp`
  - Serialize legacy component payloads.
  - Reconstruct `componentData` entries before actor components.
- Modify: `E:\WorkSpace\RenderVerseX\Tests\ResourceInstantiationValidation\main.cpp`
  - Add tests for legacy component payload serialization, instantiation, and atomic failure cases.
- Modify: `E:\WorkSpace\RenderVerseX\Docs\superpowers\plans\2026-05-15-ue-style-actor-component-phase19-prefab-legacy-component-data.md`
  - Track completion status.

---

## Task 1: Add Failing Legacy Component Data Tests

**Files:**
- Modify: `E:\WorkSpace\RenderVerseX\Tests\ResourceInstantiationValidation\main.cpp`

- [ ] Replace the existing `PrefabLegacyPayloadComponent` test class with:

```cpp
    class PrefabLegacyPayloadComponent : public Component
    {
    public:
        const char* GetTypeName() const override { return "PrefabLegacyPayloadComponent"; }

        std::string SerializePrefabData() const override
        {
            return payload;
        }

        void DeserializePrefabData(const std::string& data) override
        {
            payload = data;
        }

        void OnAttach() override
        {
            payloadObservedOnAttach = payload;
        }

        std::string payload;
        std::string payloadObservedOnAttach;
    };
```

- [ ] Add `Test_PrefabSerializesLegacyComponentPayloads` after `Test_PrefabSerializesActorComponentPayloads`:

```cpp
    bool Test_PrefabSerializesLegacyComponentPayloads()
    {
        SceneManager sceneManager;
        sceneManager.Initialize();

        const auto handle = sceneManager.CreateEntity("LegacyPayloadSource");
        SceneEntity* entity = sceneManager.GetEntity(handle);
        TEST_ASSERT_NOT_NULL(entity);

        auto* component = entity->AddComponent<PrefabLegacyPayloadComponent>();
        TEST_ASSERT_NOT_NULL(component);
        component->payload = "legacy=enabled";

        auto prefab = Prefab::CreateFromEntity(entity);
        TEST_ASSERT_NOT_NULL(prefab);

        const PrefabEntityData* rootData = prefab->GetRootData();
        TEST_ASSERT_NOT_NULL(rootData);
        auto it = rootData->componentData.find("PrefabLegacyPayloadComponent");
        TEST_ASSERT_TRUE(it != rootData->componentData.end());
        TEST_ASSERT_EQ(std::string("legacy=enabled"), it->second);

        sceneManager.Shutdown();
        return true;
    }
```

- [ ] Add `Test_PrefabInstantiatesRegisteredLegacyComponentPayloads` after `Test_PrefabInstantiatesActorComponentPayloads`:

```cpp
    bool Test_PrefabInstantiatesRegisteredLegacyComponentPayloads()
    {
        ComponentFactoryClassGuard componentFactoryGuard;
        ComponentFactory::RegisterComponentClass<PrefabLegacyPayloadComponent>(
            "PrefabLegacyPayloadComponent");

        PrefabEntityData data;
        data.name = "LegacyPayloadPrefab";
        data.componentData["PrefabLegacyPayloadComponent"] = "legacy=enabled";

        auto prefab = Prefab::CreateFromData({data});

        SceneManager sceneManager;
        sceneManager.Initialize();

        SceneEntity* instance = prefab->Instantiate(sceneManager);
        TEST_ASSERT_NOT_NULL(instance);

        auto* component = instance->GetComponent<PrefabLegacyPayloadComponent>();
        TEST_ASSERT_NOT_NULL(component);
        TEST_ASSERT_EQ(std::string("legacy=enabled"), component->payload);
        TEST_ASSERT_EQ(std::string("legacy=enabled"), component->payloadObservedOnAttach);

        sceneManager.Shutdown();
        return true;
    }
```

- [ ] Add `Test_PrefabInstantiateFailsForMissingLegacyComponentClassAndCleansUp` after `Test_PrefabInstantiateFailsForMissingActorComponentClassAndCleansUp`:

```cpp
    bool Test_PrefabInstantiateFailsForMissingLegacyComponentClassAndCleansUp()
    {
        ComponentFactoryClassGuard componentFactoryGuard;

        PrefabEntityData data;
        data.name = "MissingLegacyComponentActor";
        data.componentData["MissingLegacyComponentClass"] = "";

        auto prefab = Prefab::CreateFromData({data});

        SceneManager sceneManager;
        sceneManager.Initialize();

        SceneEntity* instance = prefab->Instantiate(sceneManager);
        TEST_ASSERT_EQ(nullptr, instance);
        TEST_ASSERT_EQ(static_cast<size_t>(0), sceneManager.GetEntityCount());

        sceneManager.Shutdown();
        return true;
    }
```

- [ ] Add `Test_PrefabInstantiateFailsForActorOnlyComponentDataAndCleansUp` after the previous test:

```cpp
    bool Test_PrefabInstantiateFailsForActorOnlyComponentDataAndCleansUp()
    {
        ComponentFactoryClassGuard componentFactoryGuard;
        ComponentFactory::RegisterComponentClass<PrefabPayloadComponent>(
            "PrefabPayloadComponent");

        PrefabEntityData data;
        data.name = "ActorOnlyComponentData";
        data.componentData["PrefabPayloadComponent"] = "";

        auto prefab = Prefab::CreateFromData({data});

        SceneManager sceneManager;
        sceneManager.Initialize();

        SceneEntity* instance = prefab->Instantiate(sceneManager);
        TEST_ASSERT_EQ(nullptr, instance);
        TEST_ASSERT_EQ(static_cast<size_t>(0), sceneManager.GetEntityCount());

        sceneManager.Shutdown();
        return true;
    }
```

- [ ] Register the four tests in `main()`:

```cpp
    suite.AddTest("PrefabSerializesLegacyComponentPayloads",
                  Test_PrefabSerializesLegacyComponentPayloads);
    suite.AddTest("PrefabInstantiateFailsForMissingLegacyComponentClassAndCleansUp",
                  Test_PrefabInstantiateFailsForMissingLegacyComponentClassAndCleansUp);
    suite.AddTest("PrefabInstantiateFailsForActorOnlyComponentDataAndCleansUp",
                  Test_PrefabInstantiateFailsForActorOnlyComponentDataAndCleansUp);
    suite.AddTest("PrefabInstantiatesRegisteredLegacyComponentPayloads",
                  Test_PrefabInstantiatesRegisteredLegacyComponentPayloads);
```

- [ ] Build the focused target and confirm RED:

```powershell
cmake --build build\Debug --config Debug --target ResourceInstantiationValidation
.\build\Debug\Tests\Debug\ResourceInstantiationValidation.exe
```

Expected result before implementation: all four new tests fail. The current code writes an empty legacy payload, ignores `componentData` during instantiation, and therefore also fails to reject missing or actor-only classes stored in `componentData`.

---

## Task 2: Add Type-Erased Legacy Component Insertion

**Files:**
- Modify: `E:\WorkSpace\RenderVerseX\Scene\Include\Scene\SceneEntity.h`
- Modify: `E:\WorkSpace\RenderVerseX\Scene\Private\SceneEntity.cpp`

- [ ] Add this public method declaration after `AddComponent(...)` in `SceneEntity.h`:

```cpp
        /// Add an already-created legacy component and transfer ownership.
        Component* AddOwnedComponent(std::unique_ptr<Component> component);
```

- [ ] Add this implementation in `SceneEntity.cpp` after the constructor:

```cpp
Component* SceneEntity::AddOwnedComponent(std::unique_ptr<Component> component)
{
    if (!component)
    {
        return nullptr;
    }

    std::type_index typeIndex(typeid(*component));
    if (m_components.find(typeIndex) != m_components.end())
    {
        return nullptr;
    }

    Component* ptr = component.get();
    component->SetOwner(this);
    component->OnComponentCreated();
    component->OnAttach();
    m_components[typeIndex] = std::move(component);
    MarkBoundsDirty();
    return ptr;
}
```

- [ ] Build the focused target:

```powershell
cmake --build build\Debug --config Debug --target ResourceInstantiationValidation
```

Expected result: target builds; registered legacy component instantiation still fails until `Prefab` uses the new API.

---

## Task 3: Reconstruct Prefab componentData

**Files:**
- Modify: `E:\WorkSpace\RenderVerseX\Scene\Private\Prefab.cpp`

- [ ] Replace the legacy component serialization block in `Prefab::SerializeEntity(...)` with:

```cpp
    for (const auto& [typeIndex, component] : entity->GetComponents())
    {
        if (std::string(component->GetTypeName()) == "PrefabInstance")
        {
            continue;
        }

        data.componentData[component->GetTypeName()] = component->SerializePrefabData();
    }
```

- [ ] Add this `componentData` reconstruction block at the start of `Prefab::CreateComponents(...)`, before `actorComponentData` is processed:

```cpp
    for (const auto& [className, serializedData] : data.componentData)
    {
        auto component = ComponentFactory::CreateComponentByClassName(className);
        if (!component)
        {
            return false;
        }

        auto* legacyRaw = dynamic_cast<Component*>(component.get());
        if (!legacyRaw)
        {
            return false;
        }

        legacyRaw->DeserializePrefabData(serializedData);
        component.release();

        std::unique_ptr<Component> legacyComponent(legacyRaw);
        if (!entity->AddOwnedComponent(std::move(legacyComponent)))
        {
            return false;
        }
    }
```

- [ ] Build and run focused validation:

```powershell
cmake --build build\Debug --config Debug --target ResourceInstantiationValidation
.\build\Debug\Tests\Debug\ResourceInstantiationValidation.exe
```

Expected result: all resource instantiation tests pass, including the four new legacy component data tests.

---

## Task 4: Validate, Review, And Commit

**Files:**
- No additional source files.

- [ ] Run whitespace check:

```powershell
git diff --check
```

Expected result: exit code 0. Line-ending warnings are acceptable if no whitespace-error lines are reported.

- [ ] Build all validation targets and ModelViewer sequentially:

```powershell
foreach ($target in @('ActorComponentValidation','SpatialComponentValidation','ResourceInstantiationValidation','RenderSceneValidation','SystemIntegrationTest','MaterialSystemValidation','RenderPassBindingValidation','ModelViewer')) {
    cmake --build build\Debug --config Debug --target $target
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}
```

Expected result: every target builds.

- [ ] Run validation executables:

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

- [ ] Run ModelViewer smoke:

```powershell
$stdout = "build_codex\phase19_modelviewer_stdout.log"
$stderr = "build_codex\phase19_modelviewer_stderr.log"
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

- [ ] Request exactly one code review using Dalton with this context:

```text
Review Phase 19 of the UE-style Actor Component framework.

Requirements:
- PrefabEntityData::componentData serializes legacy Component payloads.
- Prefab instantiation reconstructs registered legacy Component classes from componentData.
- DeserializePrefabData runs before OnAttach for legacy components.
- Missing legacy component classes fail prefab instantiation atomically.
- Actor-only component classes stored in componentData fail atomically.
- ActorComponent-only data remains handled by actorComponentData.
- No reflection, file I/O, duplicate legacy component support, or child-specific apply/revert behavior is introduced.

Please report Critical and Important issues first, with file/line references.
```

- [ ] Fix any Critical or Important review findings before committing.

- [ ] Update every checkbox in this plan that was completed.

- [ ] Commit the implementation:

```powershell
git add Docs\superpowers\plans\2026-05-15-ue-style-actor-component-phase19-prefab-legacy-component-data.md Scene\Include\Scene\SceneEntity.h Scene\Private\SceneEntity.cpp Scene\Private\Prefab.cpp Tests\ResourceInstantiationValidation\main.cpp
git commit -m "Rebuild legacy prefab components"
```

---

## Self-Review

- Spec coverage: This plan closes the gap where legacy component payloads were serialized into `componentData` but ignored at instantiation time.
- Placeholder scan: The executable steps include exact files, exact code, exact commands, and expected outcomes.
- Type consistency: `Component`, `ActorComponent`, `SceneEntity::AddOwnedComponent`, `ComponentFactory::CreateComponentByClassName`, `PrefabEntityData::componentData`, and `Prefab::CreateComponents` match current codebase types.
