# UE-Style Actor Component Phase 12 Class-Name Spawn Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Connect `ActorFactory` to `SceneManager` and `World` so registered actor classes can be spawned by class name through the same ownership, transform, parent, and lifecycle paths as typed `SpawnActor<T>()`.

**Architecture:** This phase adds explicit class-name spawn APIs instead of overloading existing template behavior. `SceneManager` accepts only factory-created `SceneEntity` subclasses and adopts them into scene ownership. `World` creates exactly one factory actor, routes `SceneEntity` subclasses into the scene and pure `Actor` subclasses into the world-owned actor registry.

**Tech Stack:** C++20, RenderVerseX Scene/World modules, standalone validation executables.

---

## Scope

- Add `SceneManager::SpawnActorByClassName(const std::string&, const ActorSpawnParams&)`.
- Add `World::SpawnActorByClassName(const std::string&, const ActorSpawnParams&)`.
- Use `ActorFactory::Create()` as the only construction source for class-name spawn.
- Preserve `ActorSpawnParams` semantics: name, local transform, scale, and parent.
- Reject pure `Actor` classes in `SceneManager`.
- Reject parented pure actors in `World`.
- Keep prefab actor-class serialization out of this phase; this phase only supplies the spawn primitive needed by future prefab/script loading.

## Files

- Modify: `E:\WorkSpace\RenderVerseX\Scene\Include\Scene\SceneManager.h`
  - Declare `SpawnActorByClassName`.
- Modify: `E:\WorkSpace\RenderVerseX\Scene\Private\SceneManager.cpp`
  - Include `Scene/ActorFactory.h`.
  - Implement scene-owned class-name adoption.
- Modify: `E:\WorkSpace\RenderVerseX\World\Include\World\World.h`
  - Declare `SpawnActorByClassName`.
- Modify: `E:\WorkSpace\RenderVerseX\World\Private\World.cpp`
  - Include `Scene/ActorFactory.h`.
  - Implement scene/pure routing from a single factory-created actor.
- Modify: `E:\WorkSpace\RenderVerseX\Tests\ActorComponentValidation\main.cpp`
  - Add class-name spawn test actors and focused tests.
- Modify: `E:\WorkSpace\RenderVerseX\Docs\superpowers\plans\2026-05-15-ue-style-actor-component-phase12-class-name-spawn.md`
  - Track completion status.

---

## Task 1: Add Failing Class-Name Spawn Tests

**Files:**
- Modify: `E:\WorkSpace\RenderVerseX\Tests\ActorComponentValidation\main.cpp`

- [ ] Add these default-constructible factory test actors near the existing `SpawnableSceneActor` and `WorldManagedPureActor` test classes:

```cpp
struct FactoryLifecycleCounters
{
    static inline int registered = 0;
    static inline int initialized = 0;
    static inline int beganPlay = 0;
    static inline int ticked = 0;
    static inline int unregistered = 0;

    static void Reset()
    {
        registered = 0;
        initialized = 0;
        beganPlay = 0;
        ticked = 0;
        unregistered = 0;
    }
};

class FactoryLifecycleComponent : public RVX::ActorComponent
{
public:
    FactoryLifecycleComponent()
    {
        SetCanEverTick(true);
        SetTickEnabled(true);
    }

    const char* GetClassName() const override { return "FactoryLifecycleComponent"; }
    void OnRegister() override { ++FactoryLifecycleCounters::registered; }
    void InitializeComponent() override { ++FactoryLifecycleCounters::initialized; }
    void BeginPlay() override { ++FactoryLifecycleCounters::beganPlay; }
    void TickComponent(float) override { ++FactoryLifecycleCounters::ticked; }
    void OnUnregister() override { ++FactoryLifecycleCounters::unregistered; }
};

class FactorySpawnSceneActor : public RVX::SceneEntity
{
public:
    FactorySpawnSceneActor()
        : SceneEntity("FactorySpawnSceneActorDefault")
    {
        AddComponent<FactoryLifecycleComponent>();
    }

    const char* GetClassName() const override { return "FactorySpawnSceneActor"; }
};

class FactorySpawnPureActor : public RVX::Actor
{
public:
    FactorySpawnPureActor()
        : Actor("FactorySpawnPureActorDefault")
    {
        auto* root = AddComponent<RVX::SceneComponent>();
        SetRootComponent(root);
        AddComponent<FactoryLifecycleComponent>();
    }

    const char* GetClassName() const override { return "FactorySpawnPureActor"; }
};
```

- [ ] Add `Test_SceneManagerSpawnActorByClassNameCreatesSceneOwnedActor` after the existing `SceneManagerSpawnActorRejectsForeignParent` test:

```cpp
bool Test_SceneManagerSpawnActorByClassNameCreatesSceneOwnedActor()
{
    RVX::ActorFactory::ClearAll();
    RVX::ActorFactory::Register("FactorySpawnSceneActor", []() {
        return std::make_unique<FactorySpawnSceneActor>();
    });
    FactoryLifecycleCounters::Reset();

    RVX::SceneManager sceneManager;
    sceneManager.Initialize();

    RVX::ActorSpawnParams parentParams;
    parentParams.name = "FactoryParent";
    RVX::SceneEntity* parent = sceneManager.SpawnActor(parentParams);
    TEST_ASSERT_NOT_NULL(parent);

    RVX::ActorSpawnParams params;
    params.name = "FactorySceneSpawn";
    params.localPosition = RVX::Vec3(3.0f, 4.0f, 5.0f);
    params.localScale = RVX::Vec3(2.0f, 2.0f, 2.0f);
    params.parent = parent;

    RVX::SceneEntity* spawned = sceneManager.SpawnActorByClassName("FactorySpawnSceneActor", params);

    TEST_ASSERT_NOT_NULL(spawned);
    TEST_ASSERT_NOT_NULL(dynamic_cast<FactorySpawnSceneActor*>(spawned));
    TEST_ASSERT_EQ(std::string("FactorySceneSpawn"), spawned->GetName());
    TEST_ASSERT_EQ(parent, spawned->GetParent());
    TEST_ASSERT_EQ(RVX::Vec3(3.0f, 4.0f, 5.0f), spawned->GetPosition());
    TEST_ASSERT_EQ(RVX::Vec3(2.0f, 2.0f, 2.0f), spawned->GetScale());
    TEST_ASSERT_EQ(spawned, sceneManager.GetEntity(spawned->GetHandle()));
    TEST_ASSERT_EQ(static_cast<size_t>(2), sceneManager.GetEntityCount());
    TEST_ASSERT_EQ(1, FactoryLifecycleCounters::registered);
    TEST_ASSERT_EQ(1, FactoryLifecycleCounters::initialized);

    sceneManager.Shutdown();
    TEST_ASSERT_EQ(1, FactoryLifecycleCounters::unregistered);
    RVX::ActorFactory::ClearAll();
    return true;
}
```

- [ ] Add `Test_SceneManagerSpawnActorByClassNameRejectsInvalidClasses` after the previous test:

```cpp
bool Test_SceneManagerSpawnActorByClassNameRejectsInvalidClasses()
{
    RVX::ActorFactory::ClearAll();
    RVX::ActorFactory::Register("FactorySpawnPureActor", []() {
        return std::make_unique<FactorySpawnPureActor>();
    });
    RVX::ActorFactory::Register("FactorySpawnSceneActor", []() {
        return std::make_unique<FactorySpawnSceneActor>();
    });

    RVX::SceneManager sceneManager;
    sceneManager.Initialize();
    RVX::SceneManager foreignSceneManager;
    foreignSceneManager.Initialize();

    RVX::ActorSpawnParams foreignParentParams;
    foreignParentParams.name = "ForeignParent";
    RVX::SceneEntity* foreignParent = foreignSceneManager.SpawnActor(foreignParentParams);
    TEST_ASSERT_NOT_NULL(foreignParent);

    TEST_ASSERT_NULL(sceneManager.SpawnActorByClassName("MissingActor", {}));
    TEST_ASSERT_NULL(sceneManager.SpawnActorByClassName("FactorySpawnPureActor", {}));

    RVX::ActorSpawnParams childParams;
    childParams.name = "RejectedChild";
    childParams.parent = foreignParent;
    TEST_ASSERT_NULL(sceneManager.SpawnActorByClassName("FactorySpawnSceneActor", childParams));
    TEST_ASSERT_EQ(static_cast<size_t>(0), sceneManager.GetEntityCount());

    foreignSceneManager.Shutdown();
    sceneManager.Shutdown();
    RVX::ActorFactory::ClearAll();
    return true;
}
```

- [ ] Add `Test_WorldSpawnActorByClassNameRoutesSceneAndPureActors` near the existing world spawn tests:

```cpp
bool Test_WorldSpawnActorByClassNameRoutesSceneAndPureActors()
{
    RVX::ActorFactory::ClearAll();
    RVX::ActorFactory::Register("FactorySpawnSceneActor", []() {
        return std::make_unique<FactorySpawnSceneActor>();
    });
    RVX::ActorFactory::Register("FactorySpawnPureActor", []() {
        return std::make_unique<FactorySpawnPureActor>();
    });

    RVX::World world;
    world.Initialize();
    FactoryLifecycleCounters::Reset();

    RVX::ActorSpawnParams sceneParams;
    sceneParams.name = "WorldSceneFactoryActor";
    RVX::Actor* sceneActor = world.SpawnActorByClassName("FactorySpawnSceneActor", sceneParams);
    TEST_ASSERT_NOT_NULL(sceneActor);
    TEST_ASSERT_NOT_NULL(dynamic_cast<FactorySpawnSceneActor*>(sceneActor));
    TEST_ASSERT_EQ(sceneActor, world.GetActor(sceneActor->GetHandle()));
    auto* sceneEntity = dynamic_cast<RVX::SceneEntity*>(sceneActor);
    TEST_ASSERT_NOT_NULL(sceneEntity);
    TEST_ASSERT_EQ(sceneEntity, world.GetSceneManager()->GetEntity(sceneEntity->GetHandle()));
    TEST_ASSERT_EQ(static_cast<size_t>(1), world.GetSceneManager()->GetEntityCount());

    RVX::ActorSpawnParams pureParams;
    pureParams.name = "WorldPureFactoryActor";
    pureParams.localPosition = RVX::Vec3(7.0f, 8.0f, 9.0f);
    RVX::Actor* pureActor = world.SpawnActorByClassName("FactorySpawnPureActor", pureParams);
    TEST_ASSERT_NOT_NULL(pureActor);
    TEST_ASSERT_NOT_NULL(dynamic_cast<FactorySpawnPureActor*>(pureActor));
    TEST_ASSERT_EQ(std::string("WorldPureFactoryActor"), pureActor->GetName());
    TEST_ASSERT_EQ(RVX::Vec3(7.0f, 8.0f, 9.0f), pureActor->GetWorldPosition());
    TEST_ASSERT_EQ(pureActor, world.GetActor(pureActor->GetHandle()));
    TEST_ASSERT_NULL(world.GetSceneManager()->GetEntity(pureActor->GetHandle()));
    TEST_ASSERT_EQ(static_cast<size_t>(1), world.GetActorCount());
    TEST_ASSERT_EQ(2, FactoryLifecycleCounters::registered);
    TEST_ASSERT_EQ(2, FactoryLifecycleCounters::initialized);

    world.Tick(0.016f);
    TEST_ASSERT_EQ(2, FactoryLifecycleCounters::beganPlay);
    TEST_ASSERT_EQ(2, FactoryLifecycleCounters::ticked);

    RVX::ActorSpawnParams parentedPureParams;
    parentedPureParams.name = "RejectedPureFactoryActor";
    parentedPureParams.parent = sceneEntity;
    TEST_ASSERT_NULL(world.SpawnActorByClassName("FactorySpawnPureActor", parentedPureParams));

    RVX::SceneManager foreignSceneManager;
    foreignSceneManager.Initialize();
    RVX::ActorSpawnParams foreignParentParams;
    foreignParentParams.name = "ForeignWorldParent";
    RVX::SceneEntity* foreignParent = foreignSceneManager.SpawnActor(foreignParentParams);
    TEST_ASSERT_NOT_NULL(foreignParent);

    RVX::ActorSpawnParams foreignChildParams;
    foreignChildParams.name = "RejectedWorldSceneFactoryActor";
    foreignChildParams.parent = foreignParent;
    TEST_ASSERT_NULL(world.SpawnActorByClassName("FactorySpawnSceneActor", foreignChildParams));

    TEST_ASSERT_NULL(world.SpawnActorByClassName("MissingActor", {}));

    foreignSceneManager.Shutdown();
    world.Shutdown();
    TEST_ASSERT_EQ(2, FactoryLifecycleCounters::unregistered);
    RVX::ActorFactory::ClearAll();
    return true;
}
```

- [ ] Register the three tests in `main()`:

```cpp
suite.AddTest("SceneManagerSpawnActorByClassNameCreatesSceneOwnedActor",
              Test_SceneManagerSpawnActorByClassNameCreatesSceneOwnedActor);
suite.AddTest("SceneManagerSpawnActorByClassNameRejectsInvalidClasses",
              Test_SceneManagerSpawnActorByClassNameRejectsInvalidClasses);
suite.AddTest("WorldSpawnActorByClassNameRoutesSceneAndPureActors",
              Test_WorldSpawnActorByClassNameRoutesSceneAndPureActors);
```

- [ ] Build the focused target and confirm the RED failure is a compile failure caused by missing APIs:

```powershell
cmake --build build\Debug --config Debug --target ActorComponentValidation
```

Expected result before implementation: build fails with missing `SpawnActorByClassName` members on `SceneManager` and `World`.

---

## Task 2: Add SceneManager Class-Name Spawn API

**Files:**
- Modify: `E:\WorkSpace\RenderVerseX\Scene\Include\Scene\SceneManager.h`
- Modify: `E:\WorkSpace\RenderVerseX\Scene\Private\SceneManager.cpp`

- [ ] In `SceneManager.h`, declare the method immediately after non-template `SpawnActor`:

```cpp
/// Spawn a scene-owned actor by registered ActorFactory class name.
SceneEntity* SpawnActorByClassName(const std::string& className,
                                   const ActorSpawnParams& params = {});
```

- [ ] In `SceneManager.cpp`, add the include:

```cpp
#include "Scene/ActorFactory.h"
```

- [ ] In `SceneManager.cpp`, implement the method immediately after `SceneManager::SpawnActor`:

```cpp
SceneEntity* SceneManager::SpawnActorByClassName(const std::string& className,
                                                 const ActorSpawnParams& params)
{
    if (className.empty())
        return nullptr;

    if (params.parent && GetEntity(params.parent->GetHandle()) != params.parent)
        return nullptr;

    auto actor = ActorFactory::Create(className);
    if (!actor)
        return nullptr;

    auto* sceneActor = dynamic_cast<SceneEntity*>(actor.get());
    if (!sceneActor)
        return nullptr;

    actor.release();
    SceneEntity::Ptr entity(sceneActor);
    sceneActor->SetName(params.name);

    SceneEntity* spawned = AddEntity(std::move(entity));
    if (!spawned)
        return nullptr;

    if (params.parent)
    {
        params.parent->AddChild(spawned);
    }

    spawned->SetPosition(params.localPosition);
    spawned->SetRotation(params.localRotation);
    spawned->SetScale(params.localScale);
    return spawned;
}
```

---

## Task 3: Add World Class-Name Spawn Routing

**Files:**
- Modify: `E:\WorkSpace\RenderVerseX\World\Include\World\World.h`
- Modify: `E:\WorkSpace\RenderVerseX\World\Private\World.cpp`

- [ ] In `World.h`, declare the method immediately after non-template `SpawnActor`:

```cpp
/// Spawn an actor by registered ActorFactory class name.
Actor* SpawnActorByClassName(const std::string& className,
                             const ActorSpawnParams& params = {});
```

- [ ] In `World.cpp`, add the include:

```cpp
#include "Scene/ActorFactory.h"
```

- [ ] In `World.cpp`, implement the method immediately after `World::SpawnActor`:

```cpp
Actor* World::SpawnActorByClassName(const std::string& className,
                                    const ActorSpawnParams& params)
{
    if (!m_initialized || className.empty())
        return nullptr;

    auto actor = ActorFactory::Create(className);
    if (!actor)
        return nullptr;

    if (auto* sceneEntity = dynamic_cast<SceneEntity*>(actor.get()))
    {
        if (!m_sceneManager)
            return nullptr;

        if (params.parent && m_sceneManager->GetEntity(params.parent->GetHandle()) != params.parent)
            return nullptr;

        actor.release();
        SceneEntity::Ptr entity(sceneEntity);
        sceneEntity->SetName(params.name);

        SceneEntity* spawned = m_sceneManager->AddEntity(std::move(entity));
        if (!spawned)
            return nullptr;

        if (params.parent)
        {
            params.parent->AddChild(spawned);
        }

        spawned->SetPosition(params.localPosition);
        spawned->SetRotation(params.localRotation);
        spawned->SetScale(params.localScale);
        return static_cast<Actor*>(spawned);
    }

    if (params.parent)
        return nullptr;

    actor->SetName(params.name);
    actor->SetAutoRegisterComponents(true);
    actor->RegisterAllComponents();
    actor->SetPosition(params.localPosition);
    actor->SetRotation(params.localRotation);
    actor->SetScale(params.localScale);

    Actor* spawned = actor.get();
    m_actors[spawned->GetHandle()] = std::move(actor);
    return spawned;
}
```

---

## Task 4: Validate, Review, And Commit

**Files:**
- No additional source files.

- [ ] Build and run the focused validation:

```powershell
cmake --build build\Debug --config Debug --target ActorComponentValidation
.\build\Debug\Tests\Debug\ActorComponentValidation.exe
```

Expected result: all actor component tests pass.

- [ ] Build validation targets and ModelViewer:

```powershell
cmake --build build\Debug --config Debug --target ActorComponentValidation SpatialComponentValidation ResourceInstantiationValidation RenderSceneValidation SystemIntegrationTest MaterialSystemValidation RenderPassBindingValidation ModelViewer
```

Expected result: build succeeds.

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
$stdout = "build_codex\phase12_modelviewer_stdout.log"
$stderr = "build_codex\phase12_modelviewer_stderr.log"
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
Review Phase 12 of the UE-style Actor Component framework.

Requirements:
- SceneManager can spawn registered SceneEntity subclasses by ActorFactory class name.
- SceneManager rejects missing classes, pure Actor classes, and foreign parents.
- World can spawn registered SceneEntity subclasses into the SceneManager.
- World can spawn registered pure Actor subclasses into its pure actor registry.
- Pure actors reject non-null SceneEntity parents.
- Class-name spawn applies ActorSpawnParams name and local transform.

Please report Critical and Important issues first, with file/line references.
```

- [ ] Fix any Critical or Important review findings before committing.

- [ ] Update every checkbox in this plan that was completed.

- [ ] Commit the implementation:

```powershell
git add Docs\superpowers\plans\2026-05-15-ue-style-actor-component-phase12-class-name-spawn.md Scene\Include\Scene\SceneManager.h Scene\Private\SceneManager.cpp World\Include\World\World.h World\Private\World.cpp Tests\ActorComponentValidation\main.cpp
git commit -m "Add actor factory class-name spawning"
```

---

## Self-Review

- Spec coverage: This plan wires the existing `ActorFactory` into both scene-owned and world-owned actor lifecycles while preserving current typed spawn APIs.
- Placeholder scan: No TBD/TODO/fill-in placeholders are present in executable steps.
- Type consistency: `ActorFactory::Create`, `ActorSpawnParams`, `SceneManager::AddEntity`, `SceneEntity::Ptr`, `World::GetActor`, and `World::GetSceneManager` are present in the current codebase.
