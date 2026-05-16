# UE-Style Actor Component Phase 9 World Actor Registry Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add World-owned non-spatial `Actor` spawning, ticking, lookup, and destruction while preserving the existing `SceneEntity`/`SceneManager` path for spatial actors.

**Architecture:** `World` becomes the public owner for pure non-spatial actors and keeps delegating `SceneEntity` actors to `SceneManager`. Pure actors live in a new `World` actor registry, get the same BeginPlay/Tick/EndPlay lifecycle semantics as scene actors, and use deferred destruction during lifecycle dispatch. `SceneManager` remains the spatial owner, so render, spatial queries, and picking are not mixed with pure actor storage.

**Tech Stack:** C++20, RenderVerseX Scene and World modules, standalone validation tests, CMake.

---

## Compatibility Decisions

- `SceneManager::SpawnActor<T>()` remains constrained to `T : SceneEntity`.
- `World::SpawnActor<T>()` becomes the general entry point:
  - `T : SceneEntity` delegates to `SceneManager::SpawnActor<T>()`.
  - `T : Actor` and not `SceneEntity` is owned by `World`.
- Pure non-spatial actors must be spawned with an explicit template argument, for example `world.SpawnActor<MyPureActor>(params)`. The non-template `world.SpawnActor(params)` intentionally remains the default `SceneEntity` path for compatibility.
- Pure actors do not support `ActorSpawnParams::parent` in this phase because the parent pointer is a `SceneEntity*` hierarchy contract.
- Pure actor spawn transform is applied through the existing `Actor` transform forwarding. This means the transform takes effect when the actor class creates a root `SceneComponent`; for actors without a root component, the current `Actor` behavior remains a no-op transform.
- `World::DestroyActor()` handles both pure actors and scene actors. Scene actors continue to delegate to `SceneManager::DestroyActor()`.
- `World::Tick()` updates pure actors and then updates the scene manager. This keeps scene/render behavior unchanged and avoids double-ticking `SceneEntity` actors.
- `World::Unload()` and `World::Shutdown()` destroy pure actors before resetting or shutting down scene content.
- Pure world-managed actors enable component auto-registration after spawn so components added after spawn follow the same register/initialize path expected from managed actors.

---

## File Structure

- Modify `Scene/Include/Scene/Actor.h`
  - Add a small auto-registration flag API for world-managed pure actors.

- Modify `Scene/Private/Actor.cpp`
  - Make `Actor::ShouldAutoRegisterComponent()` return the new flag.

- Modify `World/Include/World/World.h`
  - Broaden `SpawnActor<T>()` from `SceneEntity`-only to `Actor`-based dispatch.
  - Add pure actor lookup/iteration/count APIs.
  - Add private pure actor registry helpers.

- Modify `World/Private/World.cpp`
  - Implement pure actor lifecycle update, deferred destroy, unload/shutdown clearing, and mixed actor destroy routing.

- Modify `Tests/ActorComponentValidation/main.cpp`
  - Add validation coverage for pure actor spawn, lifecycle, lookup, destroy, deferred destroy, and `SceneEntity` delegation preservation.

---

## Task 1: Add Failing Pure Actor Registry Tests

**Files:**
- Modify: `Tests/ActorComponentValidation/main.cpp`

- [x] Add a helper `WorldManagedPureActor : public RVX::Actor` with a constructor that creates a root `SceneComponent`:

```cpp
class WorldManagedPureActor : public RVX::Actor
{
public:
    explicit WorldManagedPureActor(const std::string& name)
        : Actor(name)
    {
        auto* root = AddComponent<RVX::SceneComponent>();
        SetRootComponent(root);
    }

    const char* GetClassName() const override { return "WorldManagedPureActor"; }
};
```

- [x] Add a helper component that records registration, initialization, begin, tick, end, unregister, and destroy counts:

```cpp
struct WorldActorLifecycleCounters
{
    int registered = 0;
    int initialized = 0;
    int beganPlay = 0;
    int ticked = 0;
    int endedPlay = 0;
    int unregistered = 0;
    int destroyed = 0;
};

class WorldActorLifecycleComponent : public RVX::ActorComponent
{
public:
    explicit WorldActorLifecycleComponent(WorldActorLifecycleCounters* counters)
        : m_counters(counters)
    {
        SetCanEverTick(true);
        SetTickEnabled(true);
    }

    const char* GetClassName() const override { return "WorldActorLifecycleComponent"; }
    void OnRegister() override { if (m_counters) ++m_counters->registered; }
    void InitializeComponent() override { if (m_counters) ++m_counters->initialized; }
    void BeginPlay() override { if (m_counters) ++m_counters->beganPlay; }
    void TickComponent(float) override { if (m_counters) ++m_counters->ticked; }
    void EndPlay() override { if (m_counters) ++m_counters->endedPlay; }
    void OnUnregister() override { if (m_counters) ++m_counters->unregistered; }
    void OnComponentDestroyed() override { if (m_counters) ++m_counters->destroyed; }

private:
    WorldActorLifecycleCounters* m_counters = nullptr;
};
```

- [x] Add a helper component that requests world destruction from inside `TickComponent()`:

```cpp
class WorldActorSelfDestroyComponent : public RVX::ActorComponent
{
public:
    WorldActorSelfDestroyComponent(RVX::World* world, bool* firstResult, bool* secondResult = nullptr)
        : m_world(world)
        , m_firstResult(firstResult)
        , m_secondResult(secondResult)
    {
        SetCanEverTick(true);
        SetTickEnabled(true);
    }

    const char* GetClassName() const override { return "WorldActorSelfDestroyComponent"; }

    void TickComponent(float) override
    {
        if (m_hasTicked || !m_world)
            return;

        m_hasTicked = true;
        if (m_firstResult)
        {
            *m_firstResult = m_world->DestroyActor(GetOwner());
        }
        if (m_secondResult)
        {
            *m_secondResult = m_world->DestroyActor(GetOwner());
        }
    }

private:
    RVX::World* m_world = nullptr;
    bool* m_firstResult = nullptr;
    bool* m_secondResult = nullptr;
    bool m_hasTicked = false;
};
```

- [x] Add `Test_WorldSpawnPureActorOwnsAndLooksUpActor`:
  - Initialize `World`.
  - Spawn `WorldManagedPureActor` with name, position, rotation, and scale using the explicit template call `world.SpawnActor<WorldManagedPureActor>(params)`.
  - Assert returned pointer is non-null.
  - Assert `world.GetActor(actor->GetHandle()) == actor`.
  - Assert `world.GetActorCount() == 1`.
  - Assert `world.GetSceneManager()->GetEntity(actor->GetHandle()) == nullptr`.
  - Assert local/world transform matches spawn params because this helper creates a root scene component.

- [x] Add `Test_WorldSpawnSceneEntityStillDelegatesToSceneManager`:
  - Spawn `SpawnableSceneActor` through `World::SpawnActor<SpawnableSceneActor>()`.
  - Assert it is owned by `world.GetSceneManager()`.
  - Assert pure actor count remains 0.

- [x] Add `Test_WorldPureActorLifecycleBeginsTicksAndDestroys`:
  - Spawn `WorldManagedPureActor`.
  - Add `WorldActorLifecycleComponent` after spawn.
  - Tick world once.
  - Assert the component registered, initialized, began play, and ticked once.
  - Destroy through `world.DestroyActor(actor)`.
  - Assert `EndPlay`, `OnUnregister`, and `OnComponentDestroyed` ran once.

- [x] Add `Test_WorldDestroyPureActorDefersDuringLifecycleDispatch`:
  - Spawn `WorldManagedPureActor`.
  - Add `WorldActorSelfDestroyComponent`.
  - Tick world once.
  - Assert the first destroy result is true.
  - Assert actor count is 0 after tick flushes pending destroy.

- [x] Add `Test_WorldDestroyPureActorRejectsDuplicatePendingDestroy`:
  - Spawn `WorldManagedPureActor`.
  - Add `WorldActorSelfDestroyComponent` with first and second result pointers.
  - Tick world once.
  - Assert first result true, second result false, actor count 0.

- [x] Add `Test_WorldPureActorRejectsSceneParent`:
  - Spawn a `SpawnableSceneActor` scene parent.
  - Try to spawn `WorldManagedPureActor` with `params.parent = sceneParent`.
  - Assert null and pure actor count remains 0.

- [x] Add `Test_WorldUnloadClearsPureActorsAndSceneActors`:
  - Spawn one `WorldManagedPureActor` and one `SpawnableSceneActor`.
  - Call `world.Unload()`.
  - Assert pure actor count is 0 and scene entity count is 0.

- [x] Register the new tests in `main()`.

- [x] Run `ActorComponentValidation` and confirm it fails to compile before implementation because `World::GetActor`, `GetActorCount`, and pure `SpawnActor<T>()` are not implemented.

---

## Task 2: Add Actor Auto-Registration Flag

**Files:**
- Modify: `Scene/Include/Scene/Actor.h`
- Modify: `Scene/Private/Actor.cpp`

- [x] Add public methods to `Actor`:

```cpp
void SetAutoRegisterComponents(bool enabled) { m_autoRegisterComponents = enabled; }
bool ShouldAutoRegisterComponents() const { return m_autoRegisterComponents; }
```

- [x] Add the private member:

```cpp
bool m_autoRegisterComponents = false;
```

- [x] Update `Actor::ShouldAutoRegisterComponent()`:

```cpp
bool Actor::ShouldAutoRegisterComponent(ActorComponent* component) const
{
    (void)component;
    return m_autoRegisterComponents;
}
```

- [x] Preserve `SceneEntity::ShouldAutoRegisterComponent()` unchanged. Scene-managed entities should continue using `m_sceneManager != nullptr`.

- [x] Run:

```powershell
cmake --build build\Debug --config Debug --target ActorComponentValidation
```

Expected: still fails because `World` registry APIs are not implemented yet.

---

## Task 3: Add World Pure Actor Registry

**Files:**
- Modify: `World/Include/World/World.h`
- Modify: `World/Private/World.cpp`

- [x] Add includes to `World.h`:

```cpp
#include "Scene/Actor.h"
#include <functional>
#include <type_traits>
#include <vector>
```

- [x] Add public actor registry APIs to `World`:

```cpp
Actor* GetActor(Actor::Handle handle) const;
size_t GetActorCount() const { return m_actors.size(); }
void ForEachActor(const std::function<void(Actor*)>& callback);
```

- [x] Replace the existing `World::SpawnActor<T>()` template with an Actor-based dispatch:

```cpp
template<typename T = SceneEntity>
T* SpawnActor(const ActorSpawnParams& params = {})
{
    static_assert(std::is_base_of_v<Actor, T>, "T must derive from Actor");

    if (!m_initialized)
        return nullptr;

    if constexpr (std::is_base_of_v<SceneEntity, T>)
    {
        return m_sceneManager ? m_sceneManager->SpawnActor<T>(params) : nullptr;
    }
    else
    {
        return SpawnPureActor<T>(params);
    }
}
```

- [x] Add private registry fields to `World`:

```cpp
std::unordered_map<Actor::Handle, std::unique_ptr<Actor>> m_actors;
std::vector<Actor::Handle> m_pendingDestroyActors;
bool m_isDispatchingActorLifecycles = false;
```

- [x] Add private helper declarations:

```cpp
template<typename T>
T* SpawnPureActor(const ActorSpawnParams& params)
{
    static_assert(std::is_base_of_v<Actor, T>, "T must derive from Actor");
    static_assert(!std::is_base_of_v<SceneEntity, T>, "SceneEntity actors must use SceneManager");

    if (params.parent)
        return nullptr;

    auto actor = std::make_unique<T>(params.name);
    auto* spawned = actor.get();
    spawned->SetAutoRegisterComponents(true);
    spawned->RegisterAllComponents();
    spawned->SetPosition(params.localPosition);
    spawned->SetRotation(params.localRotation);
    spawned->SetScale(params.localScale);

    const auto handle = spawned->GetHandle();
    m_actors[handle] = std::move(actor);
    return spawned;
}

bool IsActorDestroyPending(Actor::Handle handle) const;
void QueuePendingActorDestroy(Actor::Handle handle);
void FlushPendingActorDestroys();
void DestroyPureActorImmediate(Actor::Handle handle);
void UpdatePureActorLifecycles(float deltaTime);
void ClearPureActors();
```

- [x] Update non-template `World::SpawnActor(const ActorSpawnParams&)` to continue spawning a `SceneEntity`.

- [x] Implement `World::GetActor()`:
  - First check `m_actors`.
  - If not found and `m_sceneManager` is non-null, check `m_sceneManager->GetEntity(handle)` and return that pointer as `Actor*`.
  - Return null when the handle is not found or `m_sceneManager` is null.

- [x] Implement `World::ForEachActor()`:
  - Visit all pure actors in `m_actors`.
  - Visit all scene entities from `m_sceneManager->GetEntities()` only when `m_sceneManager` is non-null.

- [x] Replace `World::DestroyActor(Actor*)`:
  - Reject null or uninitialized world.
  - If `dynamic_cast<SceneEntity*>(actor)` succeeds, delegate to `m_sceneManager->DestroyActor(actor)`.
  - Otherwise verify the actor handle exists in `m_actors` and the stored pointer matches.
  - Reject if already pending.
  - Queue during `m_isDispatchingActorLifecycles`, otherwise destroy immediately.

- [x] Implement `UpdatePureActorLifecycles(float deltaTime)`:
  - Snapshot pure actor handles.
  - Set `m_isDispatchingActorLifecycles = true`.
  - For each live, active, non-pending actor: `BeginPlay()`, revalidate, `Tick(deltaTime)`.
  - Set `m_isDispatchingActorLifecycles = false`.
  - Flush pending actor destroys.

- [x] Update `World::Tick(float deltaTime)`:

```cpp
UpdatePureActorLifecycles(deltaTime);
if (m_sceneManager)
{
    m_sceneManager->Update(deltaTime);
}
m_subsystems.TickAll(deltaTime);
```

- [x] Implement `DestroyPureActorImmediate()`:
  - Find actor in `m_actors`.
  - Call `EndPlay()`.
  - Call `UnregisterAllComponents()`.
  - Erase from `m_actors`, letting the destructor call `OnComponentDestroyed()`.

- [x] Implement `ClearPureActors()`:
  - Clear dispatch state and pending list.
  - For every actor: `EndPlay()` then `UnregisterAllComponents()`.
  - Clear `m_actors`.

- [x] Call `ClearPureActors()` in `World::Unload()` before resetting the scene manager.

- [x] Call `ClearPureActors()` in `World::Shutdown()` before subsystem deinitialization and scene manager shutdown.

---

## Task 4: Verify Phase 9

**Files:**
- No source edits unless validation exposes issues.

- [x] Build:

```powershell
cmake --build build\Debug --config Debug --target ActorComponentValidation SpatialComponentValidation ResourceInstantiationValidation RenderSceneValidation SystemIntegrationTest MaterialSystemValidation RenderPassBindingValidation ModelViewer
```

- [x] Run:

```powershell
.\build\Debug\Tests\Debug\ActorComponentValidation.exe
.\build\Debug\Tests\Debug\SpatialComponentValidation.exe
.\build\Debug\Tests\Debug\ResourceInstantiationValidation.exe
.\build\Debug\Tests\Debug\RenderSceneValidation.exe
.\build\Debug\Tests\Debug\SystemIntegrationTest.exe
.\build\Debug\Tests\Debug\MaterialSystemValidation.exe
.\build\Debug\Tests\Debug\RenderPassBindingValidation.exe
```

- [x] Run ModelViewer smoke with `C:\Users\yinzs\Desktop\DamagedHelmet.glb`; confirm model loaded, instantiated, uploaded to GPU, RenderGraph stats emitted, stderr empty, and no error/critical/assert.

- [x] Run one GPT-5.3-Codex-Spark code review after validation. Fix any Critical/Important findings before committing.

---

## Review Checkpoints

- [x] Before implementation, run one GPT-5.3-Codex-Spark review of this plan. Fix any Critical/Important plan issues before coding.
- [x] After implementation and validation, run one GPT-5.3-Codex-Spark code review. Fix any Critical/Important findings before committing.

---

## Self-Review

- Spec coverage: The plan covers pure `Actor` ownership, `SceneEntity` delegation preservation, lifecycle tick, deferred destroy, lookup/iteration, unload/shutdown cleanup, and validation.
- Placeholder scan: No TBD/TODO/fill-in placeholders are present in executable steps.
- Type consistency: `ActorSpawnParams`, `Actor`, `SceneEntity`, `World`, and helper method names match existing project types or are introduced before use.
