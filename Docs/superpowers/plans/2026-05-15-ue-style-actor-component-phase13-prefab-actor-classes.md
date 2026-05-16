# UE-Style Actor Component Phase 13 Prefab Actor Classes Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Preserve actor subclass identity when creating and instantiating prefabs so registered `SceneEntity` subclasses do not collapse back into plain `SceneEntity` instances.

**Architecture:** `PrefabEntityData` gains an optional actor class-name field. `Prefab::CreateFromEntity()` records each entity actor class, while `Prefab::InstantiateInternal()` uses `SceneManager::SpawnActorByClassName()` for custom registered `SceneEntity` subclasses and keeps the existing plain `SpawnActor()` fallback for old data with no class name. Missing custom classes fail instantiation and clean up any entities already created instead of silently losing type information.

**Tech Stack:** C++20, RenderVerseX Scene/Prefab modules, ActorFactory, standalone `ResourceInstantiationValidation`.

---

## Scope

- Add `PrefabEntityData::actorClassName`.
- Add `SceneEntity::GetClassName()` override returning `"SceneEntity"` so default scene actors serialize as scene actors, not pure `"Actor"`.
- Serialize actor class names from `Prefab::CreateFromEntity()`.
- Instantiate custom scene actor classes through `SceneManager::SpawnActorByClassName()`.
- Keep existing prefab data with empty actor class names working through plain `SceneManager::SpawnActor()`.
- Fail prefab instantiation when a non-empty custom actor class cannot be created as a `SceneEntity`.
- Do not implement property serialization or prefab override application in this phase.
- Do not implement `World::Load()` in this phase.

## Files

- Modify: `E:\WorkSpace\RenderVerseX\Scene\Include\Scene\SceneEntity.h`
  - Add `GetClassName()` override.
- Modify: `E:\WorkSpace\RenderVerseX\Scene\Include\Scene\Prefab.h`
  - Add `actorClassName` to `PrefabEntityData`.
- Modify: `E:\WorkSpace\RenderVerseX\Scene\Private\Prefab.cpp`
  - Account for actor class-name memory usage.
  - Serialize actor class names.
  - Route custom actor class instantiation through `SpawnActorByClassName`.
- Modify: `E:\WorkSpace\RenderVerseX\Tests\ResourceInstantiationValidation\main.cpp`
  - Add ActorFactory include, custom prefab actor test class, and focused tests.
- Modify: `E:\WorkSpace\RenderVerseX\Docs\superpowers\plans\2026-05-15-ue-style-actor-component-phase13-prefab-actor-classes.md`
  - Track completion status.

---

## Task 1: Add Failing Prefab Actor-Class Tests

**Files:**
- Modify: `E:\WorkSpace\RenderVerseX\Tests\ResourceInstantiationValidation\main.cpp`

- [ ] Add the ActorFactory include near other Scene includes:

```cpp
#include "Scene/ActorFactory.h"
```

- [ ] Add this default-constructible actor class inside the anonymous namespace near `TestMaterialResource`:

```cpp
class PrefabCustomSceneActor : public SceneEntity
{
public:
    PrefabCustomSceneActor()
        : SceneEntity("PrefabCustomSceneActorDefault")
    {
    }

    const char* GetClassName() const override { return "PrefabCustomSceneActor"; }
};
```

- [ ] Add `Test_PrefabSerializesActorClassNames` after `Test_PrefabSerializesActorComponentClassNames`:

```cpp
bool Test_PrefabSerializesActorClassNames()
{
    SceneManager sceneManager;
    sceneManager.Initialize();

    ActorSpawnParams rootParams;
    rootParams.name = "CustomPrefabRoot";
    auto* root = sceneManager.SpawnActor<PrefabCustomSceneActor>(rootParams);
    TEST_ASSERT_NOT_NULL(root);

    ActorSpawnParams childParams;
    childParams.name = "CustomPrefabChild";
    childParams.parent = root;
    auto* child = sceneManager.SpawnActor<PrefabCustomSceneActor>(childParams);
    TEST_ASSERT_NOT_NULL(child);

    auto prefab = Prefab::CreateFromEntity(root);
    TEST_ASSERT_NOT_NULL(prefab.get());
    TEST_ASSERT_EQ(static_cast<size_t>(2), prefab->GetEntityCount());

    const auto* rootData = prefab->GetEntityData(0);
    const auto* childData = prefab->GetEntityData(1);
    TEST_ASSERT_NOT_NULL(rootData);
    TEST_ASSERT_NOT_NULL(childData);
    TEST_ASSERT_EQ(std::string("PrefabCustomSceneActor"), rootData->actorClassName);
    TEST_ASSERT_EQ(std::string("PrefabCustomSceneActor"), childData->actorClassName);

    sceneManager.Shutdown();
    return true;
}
```

- [ ] Add `Test_PrefabInstantiatesRegisteredActorClasses` after the previous test:

```cpp
bool Test_PrefabInstantiatesRegisteredActorClasses()
{
    ActorFactory::ClearAll();
    ActorFactory::Register<PrefabCustomSceneActor>("PrefabCustomSceneActor");

    PrefabEntityData rootData;
    rootData.name = "RegisteredPrefabRoot";
    rootData.actorClassName = "PrefabCustomSceneActor";
    rootData.position = Vec3(1.0f, 2.0f, 3.0f);

    PrefabEntityData childData;
    childData.name = "RegisteredPrefabChild";
    childData.actorClassName = "PrefabCustomSceneActor";
    childData.parentIndex = 0;
    childData.position = Vec3(4.0f, 5.0f, 6.0f);

    auto prefab = Prefab::CreateFromData({rootData, childData});

    SceneManager sceneManager;
    sceneManager.Initialize();

    SceneEntity* root = prefab->Instantiate(sceneManager);
    TEST_ASSERT_NOT_NULL(root);
    TEST_ASSERT_NOT_NULL(dynamic_cast<PrefabCustomSceneActor*>(root));
    TEST_ASSERT_EQ(std::string("RegisteredPrefabRoot"), root->GetName());
    TEST_ASSERT_EQ(Vec3(1.0f, 2.0f, 3.0f), root->GetPosition());
    TEST_ASSERT_EQ(static_cast<size_t>(1), root->GetChildren().size());
    TEST_ASSERT_EQ(static_cast<size_t>(2), sceneManager.GetEntityCount());

    SceneEntity* child = root->GetChildren()[0];
    TEST_ASSERT_NOT_NULL(child);
    TEST_ASSERT_NOT_NULL(dynamic_cast<PrefabCustomSceneActor*>(child));
    TEST_ASSERT_EQ(root, child->GetParent());
    TEST_ASSERT_EQ(std::string("RegisteredPrefabChild"), child->GetName());
    TEST_ASSERT_EQ(Vec3(4.0f, 5.0f, 6.0f), child->GetPosition());

    sceneManager.Shutdown();
    ActorFactory::ClearAll();
    return true;
}
```

- [ ] Add `Test_PrefabInstantiateFailsForMissingActorClassAndCleansUp` after the previous test:

```cpp
bool Test_PrefabInstantiateFailsForMissingActorClassAndCleansUp()
{
    ActorFactory::ClearAll();

    PrefabEntityData rootData;
    rootData.name = "PlainRootBeforeMissingChild";

    PrefabEntityData childData;
    childData.name = "MissingClassChild";
    childData.actorClassName = "MissingPrefabActorClass";
    childData.parentIndex = 0;

    auto prefab = Prefab::CreateFromData({rootData, childData});

    SceneManager sceneManager;
    sceneManager.Initialize();

    SceneEntity* root = prefab->Instantiate(sceneManager);
    TEST_ASSERT_EQ(nullptr, root);
    TEST_ASSERT_EQ(static_cast<size_t>(0), sceneManager.GetEntityCount());

    sceneManager.Shutdown();
    return true;
}
```

- [ ] Register the three tests in `main()` after the current prefab tests:

```cpp
suite.AddTest("PrefabSerializesActorClassNames",
              Test_PrefabSerializesActorClassNames);
suite.AddTest("PrefabInstantiatesRegisteredActorClasses",
              Test_PrefabInstantiatesRegisteredActorClasses);
suite.AddTest("PrefabInstantiateFailsForMissingActorClassAndCleansUp",
              Test_PrefabInstantiateFailsForMissingActorClassAndCleansUp);
```

- [ ] Build the focused target and confirm RED:

```powershell
cmake --build build\Debug --config Debug --target ResourceInstantiationValidation
```

Expected result before implementation: build fails because `PrefabEntityData` has no `actorClassName` member.

---

## Task 2: Add Actor Class Metadata

**Files:**
- Modify: `E:\WorkSpace\RenderVerseX\Scene\Include\Scene\SceneEntity.h`
- Modify: `E:\WorkSpace\RenderVerseX\Scene\Include\Scene\Prefab.h`
- Modify: `E:\WorkSpace\RenderVerseX\Scene\Private\Prefab.cpp`

- [ ] In `SceneEntity.h`, add this override in the public class interface near other type methods:

```cpp
const char* GetClassName() const override { return "SceneEntity"; }
```

- [ ] In `Prefab.h`, add this field near the top of `PrefabEntityData`, immediately after `name`:

```cpp
std::string actorClassName;
```

- [ ] In `Prefab.cpp`, update `GetMemoryUsage()` inside the entity loop:

```cpp
size += entity.name.capacity();
size += entity.actorClassName.capacity();
```

- [ ] In `Prefab::SerializeEntity`, record the entity actor class immediately after the name:

```cpp
data.name = entity->GetName();
data.actorClassName = entity->GetClassName();
```

---

## Task 3: Instantiate Prefab Actor Classes

**Files:**
- Modify: `E:\WorkSpace\RenderVerseX\Scene\Private\Prefab.cpp`

- [ ] In `Prefab::InstantiateInternal`, replace the entity creation block inside the first loop with class-aware creation:

```cpp
ActorSpawnParams params;
params.name = entityData.name;

SceneEntity* entity = nullptr;
if (entityData.actorClassName.empty() || entityData.actorClassName == "SceneEntity")
{
    entity = sceneManager.SpawnActor(params);
}
else
{
    entity = sceneManager.SpawnActorByClassName(entityData.actorClassName, params);
}

if (!entity)
{
    for (auto h : createdHandles)
    {
        sceneManager.DestroyEntity(h);
    }
    return nullptr;
}
```

- [ ] Keep the second pass unchanged for transform, hierarchy, layer mask, active state, and component reconstruction.

- [ ] Build and run focused validation:

```powershell
cmake --build build\Debug --config Debug --target ResourceInstantiationValidation
.\build\Debug\Tests\Debug\ResourceInstantiationValidation.exe
```

Expected result: all resource instantiation tests pass, including the new prefab actor-class tests.

---

## Task 4: Validate, Review, And Commit

**Files:**
- No additional source files.

- [ ] Build all validation targets and ModelViewer sequentially:

```powershell
foreach ($target in @('ActorComponentValidation','SpatialComponentValidation','ResourceInstantiationValidation','RenderSceneValidation','SystemIntegrationTest','MaterialSystemValidation','RenderPassBindingValidation','ModelViewer')) {
    cmake --build build\Debug --config Debug --target $target
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}
```

Expected result: every target builds. If the single multi-target `cmake --build ... --target A B C` command hits `LNK1163`, use this sequential form; the current build tree has shown intermittent MSVC incremental linker COMDAT errors only in the combined target invocation.

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
$stdout = "build_codex\phase13_modelviewer_stdout.log"
$stderr = "build_codex\phase13_modelviewer_stderr.log"
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
Review Phase 13 of the UE-style Actor Component framework.

Requirements:
- Prefab serializes actor class names for SceneEntity subclasses.
- Plain prefab data with empty actor class names continues to instantiate as SceneEntity.
- Registered custom SceneEntity subclasses instantiate through SceneManager::SpawnActorByClassName.
- Missing custom actor classes fail instantiation and clean up already created entities.
- Existing actor component prefab reconstruction still works.

Please report Critical and Important issues first, with file/line references.
```

- [ ] Fix any Critical or Important review findings before committing.

- [ ] Update every checkbox in this plan that was completed.

- [ ] Commit the implementation:

```powershell
git add Docs\superpowers\plans\2026-05-15-ue-style-actor-component-phase13-prefab-actor-classes.md Scene\Include\Scene\SceneEntity.h Scene\Include\Scene\Prefab.h Scene\Private\Prefab.cpp Tests\ResourceInstantiationValidation\main.cpp
git commit -m "Preserve prefab actor class names"
```

---

## Self-Review

- Spec coverage: This plan closes the prefab actor-type gap created by the previous class-name spawn primitive. It covers serialization, instantiation, backward-compatible empty class names, missing-class cleanup, and validation.
- Gap scan: Executable steps are concrete. Deferred property serialization and World loading are explicitly out of scope.
- Type consistency: `PrefabEntityData`, `SceneEntity::GetClassName`, `SceneManager::SpawnActorByClassName`, `ActorFactory::Register`, `Prefab::CreateFromData`, and the validation test framework all exist in the current codebase.
