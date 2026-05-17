# UE-Style Actor Component Phase 15 Prefab Component Payload Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make prefab actor component `serializedData` meaningful by adding an opt-in `ActorComponent` serialization hook and replaying that data during prefab instantiation.

**Architecture:** Keep prefab persistence class-name based and minimal: every actor component can return a string payload and consume a string payload, with empty defaults for components that do not participate. `Prefab` stores the payload beside the component class name and applies it before the component is added to the actor, so state is restored before registration and initialization side effects. This phase deliberately avoids a full reflection/property system and keeps legacy `Component` serialization unchanged.

**Tech Stack:** C++20, RenderVerseX Scene module, standalone `ResourceInstantiationValidation`.

---

## Scope

- Add default `ActorComponent::SerializePrefabData()` and `ActorComponent::DeserializePrefabData(...)` hooks.
- Store non-legacy actor component payloads in `PrefabActorComponentData::serializedData`.
- Replay `PrefabActorComponentData::serializedData` when instantiating prefab actor components.
- Verify payloads round-trip through `Prefab::CreateFromEntity()` and `Prefab::Instantiate()`.
- Do not implement full reflection, property path editing, prefab file I/O, or legacy `Component` payload reconstruction in this phase.
- Do not change `StaticMeshComponent` or resource-handle serialization in this phase.

## Files

- Modify: `E:\WorkSpace\RenderVerseX\Scene\Include\Scene\ActorComponent.h`
  - Include `<string>`.
  - Add two virtual prefab payload hooks with empty default behavior.
- Modify: `E:\WorkSpace\RenderVerseX\Scene\Private\Prefab.cpp`
  - Store `actorComponent->SerializePrefabData()` in `PrefabActorComponentData`.
  - Call `component->DeserializePrefabData(...)` before `Actor::AddOwnedComponent(...)`.
- Modify: `E:\WorkSpace\RenderVerseX\Tests\ResourceInstantiationValidation\main.cpp`
  - Add a focused serializable actor component test double.
  - Add tests proving serialization and instantiation replay payloads.
- Modify: `E:\WorkSpace\RenderVerseX\Docs\superpowers\plans\2026-05-15-ue-style-actor-component-phase15-prefab-component-payloads.md`
  - Track completion status.

---

## Task 1: Add Failing Prefab Payload Tests

**Files:**
- Modify: `E:\WorkSpace\RenderVerseX\Tests\ResourceInstantiationValidation\main.cpp`

- [x] Add this test component inside the anonymous namespace after `PrefabCustomSceneActor`:

```cpp
    class PrefabPayloadComponent : public ActorComponent
    {
    public:
        const char* GetClassName() const override { return "PrefabPayloadComponent"; }

        std::string SerializePrefabData() const override
        {
            return payload;
        }

        void DeserializePrefabData(const std::string& data) override
        {
            payload = data;
        }

        void OnRegister() override
        {
            payloadObservedOnRegister = payload;
        }

        void InitializeComponent() override
        {
            payloadObservedOnInitialize = payload;
        }

        std::string payload;
        std::string payloadObservedOnRegister;
        std::string payloadObservedOnInitialize;
    };
```

- [x] Add `Test_PrefabSerializesActorComponentPayloads` after `Test_PrefabSerializesActorComponentClassNames`:

```cpp
    bool Test_PrefabSerializesActorComponentPayloads()
    {
        SceneManager sceneManager;
        sceneManager.Initialize();

        ActorSpawnParams params;
        params.name = "PayloadSource";
        SceneEntity* entity = sceneManager.SpawnActor(params);
        TEST_ASSERT_NOT_NULL(entity);

        auto* component = static_cast<Actor*>(entity)->AddComponent<PrefabPayloadComponent>();
        TEST_ASSERT_NOT_NULL(component);
        component->payload = "health=75;tag=elite";

        auto prefab = Prefab::CreateFromEntity(entity);
        TEST_ASSERT_NOT_NULL(prefab);
        const PrefabEntityData* data = prefab->GetRootData();
        TEST_ASSERT_NOT_NULL(data);
        TEST_ASSERT_EQ(static_cast<size_t>(1), data->actorComponentData.size());
        TEST_ASSERT_EQ(std::string("PrefabPayloadComponent"),
                       data->actorComponentData[0].className);
        TEST_ASSERT_EQ(std::string("health=75;tag=elite"),
                       data->actorComponentData[0].serializedData);

        sceneManager.Shutdown();
        return true;
    }
```

- [x] Add `Test_PrefabInstantiatesActorComponentPayloads` after `Test_PrefabInstantiatesRegisteredActorComponentClasses`:

```cpp
    bool Test_PrefabInstantiatesActorComponentPayloads()
    {
        ComponentFactory::ClearComponentClasses();
        ComponentFactory::RegisterComponentClass<PrefabPayloadComponent>("PrefabPayloadComponent");

        PrefabEntityData data;
        data.name = "PayloadPrefab";
        data.actorComponentData.push_back({"PrefabPayloadComponent", "ammo=12;mode=burst"});

        auto prefab = Prefab::CreateFromData({data});

        SceneManager sceneManager;
        sceneManager.Initialize();

        SceneEntity* instance = prefab->Instantiate(sceneManager);
        TEST_ASSERT_NOT_NULL(instance);

        auto* component = static_cast<Actor*>(instance)->GetComponent<PrefabPayloadComponent>();
        TEST_ASSERT_NOT_NULL(component);
        TEST_ASSERT_EQ(std::string("ammo=12;mode=burst"), component->payload);
        TEST_ASSERT_EQ(std::string("ammo=12;mode=burst"),
                       component->payloadObservedOnRegister);
        TEST_ASSERT_EQ(std::string("ammo=12;mode=burst"),
                       component->payloadObservedOnInitialize);
        TEST_ASSERT_TRUE(component->IsRegistered());
        TEST_ASSERT_TRUE(component->IsInitialized());

        sceneManager.Shutdown();
        ComponentFactory::ClearComponentClasses();
        return true;
    }
```

- [x] Register the two tests in `main()` near the existing prefab actor-component tests:

```cpp
    suite.AddTest("PrefabSerializesActorComponentPayloads",
                  Test_PrefabSerializesActorComponentPayloads);
    suite.AddTest("PrefabInstantiatesActorComponentPayloads",
                  Test_PrefabInstantiatesActorComponentPayloads);
```

- [x] Build the focused target and confirm RED:

```powershell
cmake --build build\Debug --config Debug --target ResourceInstantiationValidation
.\build\Debug\Tests\Debug\ResourceInstantiationValidation.exe
```

Expected result before implementation: the target fails to compile because `ActorComponent` does not yet define `SerializePrefabData()` or `DeserializePrefabData(...)`.

---

## Task 2: Add ActorComponent Payload Hooks

**Files:**
- Modify: `E:\WorkSpace\RenderVerseX\Scene\Include\Scene\ActorComponent.h`

- [x] Add the standard library include after `#pragma once` and the file comment:

```cpp
#include <string>
```

- [x] Add this public section after `GetClassName()`:

```cpp
        // =====================================================================
        // Prefab Serialization
        // =====================================================================

        virtual std::string SerializePrefabData() const { return {}; }
        virtual void DeserializePrefabData(const std::string& data) { (void)data; }
```

- [x] Build the focused target and confirm the tests now compile but payload behavior still fails:

```powershell
cmake --build build\Debug --config Debug --target ResourceInstantiationValidation
.\build\Debug\Tests\Debug\ResourceInstantiationValidation.exe
```

Expected result after only this task: the executable builds, but at least one new payload test fails because `Prefab` still writes and replays empty strings.

---

## Task 3: Store And Replay Prefab Actor Component Payloads

**Files:**
- Modify: `E:\WorkSpace\RenderVerseX\Scene\Private\Prefab.cpp`

- [x] In `Prefab::SerializeEntity`, replace:

```cpp
        data.actorComponentData.push_back({className, ""});
```

with:

```cpp
        data.actorComponentData.push_back({className, actorComponent->SerializePrefabData()});
```

- [x] In `Prefab::CreateComponents`, replace the unused serialized-data cast and move payload replay before insertion:

```cpp
    for (const auto& componentData : data.actorComponentData)
    {
        (void)componentData.serializedData;

        auto component = ComponentFactory::CreateComponentByClassName(componentData.className);
        if (!component)
            continue;

        auto* raw = component.get();
        ActorComponent* inserted = static_cast<Actor*>(entity)->AddOwnedComponent(std::move(component));
```

with:

```cpp
    for (const auto& componentData : data.actorComponentData)
    {
        auto component = ComponentFactory::CreateComponentByClassName(componentData.className);
        if (!component)
            continue;

        component->DeserializePrefabData(componentData.serializedData);

        auto* raw = component.get();
        ActorComponent* inserted = static_cast<Actor*>(entity)->AddOwnedComponent(std::move(component));
```

- [x] Build and run focused validation:

```powershell
cmake --build build\Debug --config Debug --target ResourceInstantiationValidation
.\build\Debug\Tests\Debug\ResourceInstantiationValidation.exe
```

Expected result: all resource instantiation tests pass, including the two new payload tests.

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
$stdout = "build_codex\phase15_modelviewer_stdout.log"
$stderr = "build_codex\phase15_modelviewer_stderr.log"
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
Review Phase 15 of the UE-style Actor Component framework.

Requirements:
- ActorComponent has default empty prefab payload serialization hooks.
- Prefab::CreateFromEntity stores actor component payloads in PrefabActorComponentData::serializedData.
- Prefab::Instantiate replays serializedData into newly created actor components.
- Payload replay happens before component insertion into the actor.
- Existing class-name-only prefab behavior remains compatible.
- No full reflection/property system, prefab file I/O, or legacy Component payload reconstruction is introduced.

Please report Critical and Important issues first, with file/line references.
```

- [x] Fix any Critical or Important review findings before committing.

- [x] Update every checkbox in this plan that was completed.

- [x] Commit the implementation:

```powershell
git add Docs\superpowers\plans\2026-05-15-ue-style-actor-component-phase15-prefab-component-payloads.md Scene\Include\Scene\ActorComponent.h Scene\Private\Prefab.cpp Tests\ResourceInstantiationValidation\main.cpp
git commit -m "Replay prefab actor component payloads"
```

---

## Self-Review

- Spec coverage: This plan closes the obvious `PrefabActorComponentData::serializedData` gap without expanding scope into file formats or reflection.
- Placeholder scan: The executable steps include exact code, exact files, exact commands, and expected outcomes.
- Type consistency: `ActorComponent`, `SerializePrefabData`, `DeserializePrefabData`, `PrefabActorComponentData::serializedData`, `ComponentFactory::CreateComponentByClassName`, and `Actor::AddOwnedComponent` are the existing/current names used by the codebase.
