# UE-Style Actor Component Phase 16 Prefab Component Atomicity Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make prefab actor component reconstruction atomic so missing or rejected actor component classes fail instantiation and clean up partial scene entities.

**Architecture:** `Prefab::InstantiateInternal()` already treats missing actor classes as fatal and destroys entities created earlier in the same instantiate call. This phase extends that rule to actor component reconstruction by making `CreateComponents(...)` return success/failure and by cleaning up the whole prefab instance when any required actor component cannot be created or inserted. Legacy `Component` subclasses remain unsupported in `actorComponentData`; if one is registered by class-name factory and routed through actor-owned reconstruction, instantiation fails instead of silently producing an incomplete actor.

**Tech Stack:** C++20, RenderVerseX Scene module, standalone `ResourceInstantiationValidation`.

---

## Scope

- Change `Prefab::CreateComponents(...)` from `void` to `bool`.
- Fail prefab instantiation when an actor component class name cannot be created by `ComponentFactory`.
- Fail prefab instantiation when `Actor::AddOwnedComponent(...)` rejects the created actor component.
- Destroy all entities created by the current prefab instantiate call on actor component reconstruction failure.
- Preserve existing successful prefab component reconstruction behavior.
- Do not add optional component semantics, editor repair behavior, reflection, prefab file I/O, or legacy `Component` reconstruction in this phase.

## Files

- Modify: `E:\WorkSpace\RenderVerseX\Scene\Include\Scene\Prefab.h`
  - Change the private `CreateComponents(...)` declaration to return `bool`.
- Modify: `E:\WorkSpace\RenderVerseX\Scene\Private\Prefab.cpp`
  - Make `CreateComponents(...)` return false on missing class or insertion rejection.
  - Make `InstantiateInternal(...)` destroy `createdHandles` and return null if component creation fails.
- Modify: `E:\WorkSpace\RenderVerseX\Tests\ResourceInstantiationValidation\main.cpp`
  - Add a legacy component test double and a component class registry guard.
  - Add tests for missing actor component class cleanup and rejected legacy component cleanup.
- Modify: `E:\WorkSpace\RenderVerseX\Docs\superpowers\plans\2026-05-15-ue-style-actor-component-phase16-prefab-component-atomicity.md`
  - Track completion status.

---

## Task 1: Add Failing Atomicity Tests

**Files:**
- Modify: `E:\WorkSpace\RenderVerseX\Tests\ResourceInstantiationValidation\main.cpp`

- [ ] Add this legacy component test double after `PrefabPayloadComponent`:

```cpp
    class PrefabLegacyPayloadComponent : public Component
    {
    public:
        const char* GetTypeName() const override { return "PrefabLegacyPayloadComponent"; }
    };
```

- [ ] Add this component class registry guard near the existing test guards:

```cpp
    class ComponentFactoryClassGuard
    {
    public:
        ComponentFactoryClassGuard()
        {
            ComponentFactory::ClearComponentClasses();
        }

        ~ComponentFactoryClassGuard()
        {
            ComponentFactory::ClearComponentClasses();
        }
    };
```

- [ ] Add `Test_PrefabInstantiateFailsForMissingActorComponentClassAndCleansUp` after `Test_PrefabInstantiateFailsForMissingActorClassAndCleansUp`:

```cpp
    bool Test_PrefabInstantiateFailsForMissingActorComponentClassAndCleansUp()
    {
        ComponentFactoryClassGuard componentFactoryGuard;

        PrefabEntityData data;
        data.name = "MissingComponentActor";
        data.actorComponentData.push_back({"MissingActorComponentClass", ""});

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

- [ ] Add `Test_PrefabInstantiateFailsForRejectedActorComponentAndCleansUp` after the previous test:

```cpp
    bool Test_PrefabInstantiateFailsForRejectedActorComponentAndCleansUp()
    {
        ComponentFactoryClassGuard componentFactoryGuard;
        ComponentFactory::RegisterComponentClass<PrefabLegacyPayloadComponent>(
            "PrefabLegacyPayloadComponent");

        PrefabEntityData data;
        data.name = "RejectedComponentActor";
        data.actorComponentData.push_back({"PrefabLegacyPayloadComponent", ""});

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

- [ ] Register both tests in `main()` near the existing prefab failure tests:

```cpp
    suite.AddTest("PrefabInstantiateFailsForMissingActorComponentClassAndCleansUp",
                  Test_PrefabInstantiateFailsForMissingActorComponentClassAndCleansUp);
    suite.AddTest("PrefabInstantiateFailsForRejectedActorComponentAndCleansUp",
                  Test_PrefabInstantiateFailsForRejectedActorComponentAndCleansUp);
```

- [ ] Build the focused target and confirm RED:

```powershell
cmake --build build\Debug --config Debug --target ResourceInstantiationValidation
.\build\Debug\Tests\Debug\ResourceInstantiationValidation.exe
```

Expected result before implementation: the executable builds, but both new tests fail because `Prefab::CreateComponents()` currently skips missing component classes and ignored insertion failures.

---

## Task 2: Make CreateComponents Report Failure

**Files:**
- Modify: `E:\WorkSpace\RenderVerseX\Scene\Include\Scene\Prefab.h`
- Modify: `E:\WorkSpace\RenderVerseX\Scene\Private\Prefab.cpp`

- [ ] In `Prefab.h`, replace:

```cpp
    void CreateComponents(SceneEntity* entity, const PrefabEntityData& data) const;
```

with:

```cpp
    bool CreateComponents(SceneEntity* entity, const PrefabEntityData& data) const;
```

- [ ] In `Prefab.cpp`, replace the whole `CreateComponents` function with:

```cpp
bool Prefab::CreateComponents(SceneEntity* entity, const PrefabEntityData& data) const
{
    if (!entity)
        return false;

    for (const auto& componentData : data.actorComponentData)
    {
        auto component = ComponentFactory::CreateComponentByClassName(componentData.className);
        if (!component)
            return false;

        component->DeserializePrefabData(componentData.serializedData);

        auto* raw = component.get();
        ActorComponent* inserted = static_cast<Actor*>(entity)->AddOwnedComponent(std::move(component));
        if (!inserted)
            return false;

        if (auto* sceneComponent = dynamic_cast<SceneComponent*>(raw))
        {
            sceneComponent->AttachToComponent(entity->GetRootComponent());
        }
    }

    return true;
}
```

- [ ] Build and run focused validation:

```powershell
cmake --build build\Debug --config Debug --target ResourceInstantiationValidation
.\build\Debug\Tests\Debug\ResourceInstantiationValidation.exe
```

Expected result after only this task: the two new tests still fail because `InstantiateInternal(...)` does not yet act on the false result.

---

## Task 3: Roll Back Prefab Instantiation On Component Failure

**Files:**
- Modify: `E:\WorkSpace\RenderVerseX\Scene\Private\Prefab.cpp`

- [ ] In `Prefab::InstantiateInternal`, replace:

```cpp
        // Create components
        CreateComponents(entity, entityData);
```

with:

```cpp
        // Create components
        if (!CreateComponents(entity, entityData))
        {
            for (auto h : createdHandles)
            {
                sceneManager.DestroyEntity(h);
            }
            return nullptr;
        }
```

- [ ] Build and run focused validation:

```powershell
cmake --build build\Debug --config Debug --target ResourceInstantiationValidation
.\build\Debug\Tests\Debug\ResourceInstantiationValidation.exe
```

Expected result: all resource instantiation tests pass, including missing and rejected actor component cleanup.

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
$stdout = "build_codex\phase16_modelviewer_stdout.log"
$stderr = "build_codex\phase16_modelviewer_stderr.log"
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
Review Phase 16 of the UE-style Actor Component framework.

Requirements:
- Prefab actor component reconstruction is atomic.
- Missing actor component class names cause Prefab::Instantiate to return null.
- Actor components rejected by Actor::AddOwnedComponent cause Prefab::Instantiate to return null.
- Partial scene entities created during the failed instantiate call are destroyed.
- Existing valid prefab actor component reconstruction still works.
- No optional component semantics, reflection, prefab file I/O, or legacy Component reconstruction is introduced.

Please report Critical and Important issues first, with file/line references.
```

- [ ] Fix any Critical or Important review findings before committing.

- [ ] Update every checkbox in this plan that was completed.

- [ ] Commit the implementation:

```powershell
git add Docs\superpowers\plans\2026-05-15-ue-style-actor-component-phase16-prefab-component-atomicity.md Scene\Include\Scene\Prefab.h Scene\Private\Prefab.cpp Tests\ResourceInstantiationValidation\main.cpp
git commit -m "Make prefab component reconstruction atomic"
```

---

## Self-Review

- Spec coverage: This plan enforces prefab component reconstruction failure for missing classes and actor insertion rejection, and it verifies cleanup by entity count.
- Placeholder scan: The executable steps include exact files, exact code, exact commands, and expected outcomes.
- Type consistency: `Prefab::CreateComponents`, `PrefabEntityData::actorComponentData`, `ComponentFactory::CreateComponentByClassName`, `Actor::AddOwnedComponent`, and `SceneManager::DestroyEntity` match the current codebase.
