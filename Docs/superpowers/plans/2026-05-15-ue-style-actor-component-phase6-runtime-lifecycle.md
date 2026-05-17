# UE-Style Actor Component Phase 6 Runtime Lifecycle Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make `SceneManager::Update()` drive UE-style actor component runtime lifecycle without breaking the legacy `SceneEntity` component tick path.

**Architecture:** `SceneEntity` remains the compatibility object stored by `SceneManager`, while pure `ActorComponent` instances are owned and ticked by `Actor`. `SceneManager` becomes the runtime lifecycle coordinator: active entities get `BeginPlay()` once, pure actor components tick through `Actor::Tick()`, legacy `Component` subclasses keep ticking through `SceneEntity::TickComponents()`, and entity removal/shutdown sends `EndPlay()` before unregistering components. Runtime lifecycle dispatch snapshots entity handles and defers `DestroyEntity()` requests made inside component callbacks until dispatch completes, so component-driven scene mutation cannot invalidate iteration or destroy the currently executing actor. Actor lifecycle is also tightened so late-added components receive `BeginPlay()` on the next update and `SceneEntity` active state is respected through `Actor*` calls.

**Tech Stack:** C++20, RenderVerseX Scene/World modules, custom validation executables, CMake.

## Compatibility Decisions

- Do not move legacy `Component` subclasses into `Actor::m_components` in this phase. They still live in `SceneEntity::m_components` and are ticked by `SceneEntity::TickComponents(float)`.
- Do not make `Component::Tick(float)` depend on `ActorComponent::CanEverTick()`. Existing legacy components ticked every frame when enabled, so Phase 6 preserves that behavior.
- Pure `ActorComponent` instances tick only when `CanEverTick()` and `IsTickEnabled()` are both true.
- Inactive `SceneEntity` instances do not call `BeginPlay()`, pure actor component tick, or legacy component tick during `SceneManager::Update()`. Re-activating an entity later starts or resumes lifecycle on the next update.
- Active state remains per entity in Phase 6. Parent-child active inheritance is deliberately not introduced because existing hierarchy APIs do not currently define cascade activation semantics; this can be added later as an explicit scene policy.
- `DestroyEntity()` called during `SceneManager::Update()` lifecycle dispatch is deferred until the current lifecycle pass finishes. This matches common engine practice and avoids invalidating component iteration, while ensuring the entity is gone before the frame's spatial dirty update continues.
- `EndPlay()` is not a deactivate event. It runs on entity destruction, scene shutdown, and actor destruction.

---

## File Structure

- Modify `Tests/ActorComponentValidation/main.cpp`
  - Add runtime lifecycle tests for scene-managed pure actor components, late-added components, legacy tick compatibility, active-state gating, lifecycle event ordering, deferred destroy during tick, and destroy-time `EndPlay()`.

- Modify `Scene/Include/Scene/Actor.h`
  - Add `bool HasBegunPlay() const` for lifecycle state introspection.

- Modify `Scene/Private/Actor.cpp`
  - Make `BeginPlay()` idempotently begin any component that has not begun yet, even after the actor itself has begun.
  - Make `Tick()` use virtual `IsActive()` so `SceneEntity` active state is respected through an `Actor*`.

- Modify `Scene/Include/Scene/SceneEntity.h`
  - Mark `GetName()`, `SetName()`, `IsActive()`, and `SetActive()` as `override`.
  - Route `SetActive()` through `Actor::SetActive(active)` so base and compatibility active flags stay coherent.

- Modify `Scene/Private/SceneManager.cpp`
  - Add `UpdateEntityLifecycles(float deltaTime)` and call it before spatial dirty collection.
  - Add deferred destroy helpers so lifecycle callbacks can call `DestroyEntity()` safely.
  - Call `EndPlay()` before `UnregisterAllComponents()` in `DestroyEntity()` and `Shutdown()`.

- Modify `Scene/Include/Scene/SceneManager.h`
  - Declare the private `UpdateEntityLifecycles(float deltaTime)`, `DestroyEntityImmediate(...)`, `FlushPendingDestroyEntities()`, and `QueuePendingDestroy(...)` helpers.
  - Add lifecycle-dispatch state and pending-destroy storage.

---

## Task 1: Add Failing Runtime Lifecycle Tests

**Files:**
- Modify: `Tests/ActorComponentValidation/main.cpp`

- [ ] **Step 1: Add test helper structs and components**

Add these includes near the existing standard-library includes:

```cpp
#include <cmath>
#include <vector>
```

Add these helpers inside the anonymous namespace after `CountingComponent`:

```cpp
bool IsNear(float a, float b, float epsilon = 0.0001f)
{
    return std::abs(a - b) <= epsilon;
}

struct RuntimeLifecycleCounters
{
    int beganPlay = 0;
    int ticked = 0;
    int endedPlay = 0;
    int unregistered = 0;
    float lastDeltaTime = 0.0f;
    std::vector<std::string> events;
};

class SceneManagedActorComponent : public RVX::ActorComponent
{
public:
    explicit SceneManagedActorComponent(RuntimeLifecycleCounters* counters)
        : m_counters(counters)
    {
        SetCanEverTick(true);
        SetTickEnabled(true);
    }

    const char* GetClassName() const override { return "SceneManagedActorComponent"; }

    void BeginPlay() override
    {
        if (m_counters)
        {
            ++m_counters->beganPlay;
            m_counters->events.push_back("BeginPlay");
        }
    }

    void TickComponent(float deltaTime) override
    {
        if (m_counters)
        {
            ++m_counters->ticked;
            m_counters->lastDeltaTime = deltaTime;
            m_counters->events.push_back("Tick");
        }
    }

    void EndPlay() override
    {
        if (m_counters)
        {
            ++m_counters->endedPlay;
            m_counters->events.push_back("EndPlay");
        }
    }

    void OnUnregister() override
    {
        if (m_counters)
        {
            ++m_counters->unregistered;
        }
    }

private:
    RuntimeLifecycleCounters* m_counters = nullptr;
};

class SceneManagedLegacyTickComponent : public RVX::Component
{
public:
    const char* GetTypeName() const override { return "SceneManagedLegacyTickComponent"; }

    void Tick(float deltaTime) override
    {
        ++tickCount;
        lastDeltaTime = deltaTime;
    }

    int tickCount = 0;
    float lastDeltaTime = 0.0f;
};

class SelfDestroyingActorComponent : public RVX::ActorComponent
{
public:
    SelfDestroyingActorComponent(RVX::SceneManager* sceneManager,
                                 RVX::SceneEntity::Handle ownerHandle,
                                 int* tickCount)
        : m_sceneManager(sceneManager)
        , m_ownerHandle(ownerHandle)
        , m_tickCount(tickCount)
    {
        SetCanEverTick(true);
        SetTickEnabled(true);
    }

    const char* GetClassName() const override { return "SelfDestroyingActorComponent"; }

    void TickComponent(float) override
    {
        if (m_tickCount)
        {
            ++(*m_tickCount);
        }
        if (m_sceneManager)
        {
            m_sceneManager->DestroyEntity(m_ownerHandle);
        }
    }

private:
    RVX::SceneManager* m_sceneManager = nullptr;
    RVX::SceneEntity::Handle m_ownerHandle = RVX::SceneEntity::InvalidHandle;
    int* m_tickCount = nullptr;
};
```

- [ ] **Step 2: Add scene-managed actor lifecycle test**

Add this test:

```cpp
bool Test_SceneManagerUpdateDispatchesActorComponentLifecycle()
{
    RVX::SceneManager sceneManager;
    sceneManager.Initialize();

    const auto handle = sceneManager.CreateEntity("RuntimeLifecycleEntity");
    auto* entity = sceneManager.GetEntity(handle);
    TEST_ASSERT_NOT_NULL(entity);

    RuntimeLifecycleCounters counters;
    auto component = std::make_unique<SceneManagedActorComponent>(&counters);
    auto* raw = component.get();
    TEST_ASSERT_EQ(raw, static_cast<RVX::Actor*>(entity)->AddOwnedComponent(std::move(component)));

    sceneManager.Update(0.5f);
    sceneManager.Update(0.25f);

    TEST_ASSERT_TRUE(raw->HasBegunPlay());
    TEST_ASSERT_EQ(1, counters.beganPlay);
    TEST_ASSERT_EQ(2, counters.ticked);
    TEST_ASSERT_TRUE(IsNear(0.25f, counters.lastDeltaTime));
    TEST_ASSERT_EQ(static_cast<size_t>(3), counters.events.size());
    TEST_ASSERT_EQ(std::string("BeginPlay"), counters.events[0]);
    TEST_ASSERT_EQ(std::string("Tick"), counters.events[1]);
    TEST_ASSERT_EQ(std::string("Tick"), counters.events[2]);

    sceneManager.Shutdown();
    return true;
}
```

- [ ] **Step 3: Add late-added component lifecycle test**

Add this test:

```cpp
bool Test_SceneManagerBeginsComponentsAddedAfterActorBeginsPlay()
{
    RVX::SceneManager sceneManager;
    sceneManager.Initialize();

    const auto handle = sceneManager.CreateEntity("LateComponentEntity");
    auto* entity = sceneManager.GetEntity(handle);
    TEST_ASSERT_NOT_NULL(entity);

    sceneManager.Update(0.016f);
    TEST_ASSERT_TRUE(static_cast<RVX::Actor*>(entity)->HasBegunPlay());

    RuntimeLifecycleCounters counters;
    auto component = std::make_unique<SceneManagedActorComponent>(&counters);
    auto* raw = component.get();
    TEST_ASSERT_EQ(raw, static_cast<RVX::Actor*>(entity)->AddOwnedComponent(std::move(component)));
    TEST_ASSERT_FALSE(raw->HasBegunPlay());

    sceneManager.Update(0.033f);

    TEST_ASSERT_TRUE(raw->HasBegunPlay());
    TEST_ASSERT_EQ(1, counters.beganPlay);
    TEST_ASSERT_EQ(1, counters.ticked);
    TEST_ASSERT_TRUE(IsNear(0.033f, counters.lastDeltaTime));
    TEST_ASSERT_EQ(static_cast<size_t>(2), counters.events.size());
    TEST_ASSERT_EQ(std::string("BeginPlay"), counters.events[0]);
    TEST_ASSERT_EQ(std::string("Tick"), counters.events[1]);

    sceneManager.Shutdown();
    return true;
}
```

- [ ] **Step 4: Add legacy tick compatibility test**

Add this test:

```cpp
bool Test_SceneManagerUpdateKeepsLegacyComponentTicking()
{
    RVX::SceneManager sceneManager;
    sceneManager.Initialize();

    const auto handle = sceneManager.CreateEntity("LegacyTickEntity");
    auto* entity = sceneManager.GetEntity(handle);
    TEST_ASSERT_NOT_NULL(entity);

    auto* component = entity->AddComponent<SceneManagedLegacyTickComponent>();
    TEST_ASSERT_NOT_NULL(component);

    sceneManager.Update(0.125f);
    TEST_ASSERT_EQ(1, component->tickCount);
    TEST_ASSERT_TRUE(IsNear(0.125f, component->lastDeltaTime));

    entity->SetActive(false);
    sceneManager.Update(0.5f);
    TEST_ASSERT_EQ(1, component->tickCount);

    entity->SetActive(true);
    sceneManager.Update(0.25f);
    TEST_ASSERT_EQ(2, component->tickCount);
    TEST_ASSERT_TRUE(IsNear(0.25f, component->lastDeltaTime));

    sceneManager.Shutdown();
    return true;
}
```

- [ ] **Step 5: Add active-state and destroy-time tests**

Add these tests:

```cpp
bool Test_SceneEntityActiveStateControlsActorTickThroughActorPointer()
{
    RVX::SceneEntity entity("DirectActorTickEntity");
    RVX::Actor* actor = &entity;

    RuntimeLifecycleCounters counters;
    auto component = std::make_unique<SceneManagedActorComponent>(&counters);
    auto* raw = component.get();
    TEST_ASSERT_EQ(raw, actor->AddOwnedComponent(std::move(component)));

    entity.SetActive(false);
    actor->Tick(0.25f);
    TEST_ASSERT_EQ(0, counters.ticked);

    actor->SetActive(true);
    actor->Tick(0.5f);
    TEST_ASSERT_EQ(1, counters.ticked);
    TEST_ASSERT_TRUE(IsNear(0.5f, counters.lastDeltaTime));
    return true;
}

bool Test_SceneManagerDefersDestroyRequestsDuringLifecycleDispatch()
{
    RVX::SceneManager sceneManager;
    sceneManager.Initialize();

    const auto handle = sceneManager.CreateEntity("SelfDestroyingEntity");
    auto* entity = sceneManager.GetEntity(handle);
    TEST_ASSERT_NOT_NULL(entity);

    int tickCount = 0;
    auto component = std::make_unique<SelfDestroyingActorComponent>(&sceneManager, handle, &tickCount);
    TEST_ASSERT_NOT_NULL(static_cast<RVX::Actor*>(entity)->AddOwnedComponent(std::move(component)));

    sceneManager.Update(0.016f);

    TEST_ASSERT_EQ(1, tickCount);
    TEST_ASSERT_EQ(nullptr, sceneManager.GetEntity(handle));

    sceneManager.Shutdown();
    return true;
}

bool Test_SceneManagerDestroyEntityDispatchesEndPlayBeforeUnregister()
{
    RVX::SceneManager sceneManager;
    sceneManager.Initialize();

    const auto handle = sceneManager.CreateEntity("DestroyLifecycleEntity");
    auto* entity = sceneManager.GetEntity(handle);
    TEST_ASSERT_NOT_NULL(entity);

    RuntimeLifecycleCounters counters;
    auto component = std::make_unique<SceneManagedActorComponent>(&counters);
    TEST_ASSERT_NOT_NULL(static_cast<RVX::Actor*>(entity)->AddOwnedComponent(std::move(component)));

    sceneManager.Update(0.016f);
    TEST_ASSERT_EQ(1, counters.beganPlay);

    sceneManager.DestroyEntity(handle);

    TEST_ASSERT_EQ(1, counters.endedPlay);
    TEST_ASSERT_EQ(1, counters.unregistered);
    sceneManager.Shutdown();
    return true;
}

bool Test_SceneManagerDestroyEntityBeforeBeginPlayDoesNotDispatchEndPlay()
{
    RVX::SceneManager sceneManager;
    sceneManager.Initialize();

    const auto handle = sceneManager.CreateEntity("NeverBegunLifecycleEntity");
    auto* entity = sceneManager.GetEntity(handle);
    TEST_ASSERT_NOT_NULL(entity);

    RuntimeLifecycleCounters counters;
    auto component = std::make_unique<SceneManagedActorComponent>(&counters);
    TEST_ASSERT_NOT_NULL(static_cast<RVX::Actor*>(entity)->AddOwnedComponent(std::move(component)));

    entity->SetActive(false);
    sceneManager.DestroyEntity(handle);

    TEST_ASSERT_EQ(0, counters.beganPlay);
    TEST_ASSERT_EQ(0, counters.ticked);
    TEST_ASSERT_EQ(0, counters.endedPlay);
    TEST_ASSERT_EQ(1, counters.unregistered);
    sceneManager.Shutdown();
    return true;
}
```

- [ ] **Step 6: Register the new tests**

Add these entries before `suite.Run()`:

```cpp
suite.AddTest("SceneManagerUpdateDispatchesActorComponentLifecycle",
              Test_SceneManagerUpdateDispatchesActorComponentLifecycle);
suite.AddTest("SceneManagerBeginsComponentsAddedAfterActorBeginsPlay",
              Test_SceneManagerBeginsComponentsAddedAfterActorBeginsPlay);
suite.AddTest("SceneManagerUpdateKeepsLegacyComponentTicking",
              Test_SceneManagerUpdateKeepsLegacyComponentTicking);
suite.AddTest("SceneEntityActiveStateControlsActorTickThroughActorPointer",
              Test_SceneEntityActiveStateControlsActorTickThroughActorPointer);
suite.AddTest("SceneManagerDefersDestroyRequestsDuringLifecycleDispatch",
              Test_SceneManagerDefersDestroyRequestsDuringLifecycleDispatch);
suite.AddTest("SceneManagerDestroyEntityDispatchesEndPlayBeforeUnregister",
              Test_SceneManagerDestroyEntityDispatchesEndPlayBeforeUnregister);
suite.AddTest("SceneManagerDestroyEntityBeforeBeginPlayDoesNotDispatchEndPlay",
              Test_SceneManagerDestroyEntityBeforeBeginPlayDoesNotDispatchEndPlay);
```

- [ ] **Step 7: Run RED**

Run:

```powershell
cmake --build build\Debug --config Debug --target ActorComponentValidation
.\build\Debug\Tests\Debug\ActorComponentValidation.exe
```

Expected before implementation:

- Build fails because `Actor::HasBegunPlay()` is missing, or tests fail because `SceneManager::Update()` does not drive lifecycle and `Actor::Tick()` ignores `SceneEntity` active state.

---

## Task 2: Tighten Actor and SceneEntity Lifecycle Semantics

**Files:**
- Modify: `Scene/Include/Scene/Actor.h`
- Modify: `Scene/Private/Actor.cpp`
- Modify: `Scene/Include/Scene/SceneEntity.h`

- [ ] **Step 1: Add actor lifecycle state getter**

In `Scene/Include/Scene/Actor.h`, add this in the lifecycle section:

```cpp
bool HasBegunPlay() const { return m_hasBegunPlay; }
```

- [ ] **Step 2: Begin late-added components idempotently**

Replace `Actor::BeginPlay()` in `Scene/Private/Actor.cpp` with:

```cpp
void Actor::BeginPlay()
{
    if (!m_hasBegunPlay)
    {
        m_hasBegunPlay = true;
    }

    for (auto& component : m_components)
    {
        if (component && component->IsEnabled() && !component->HasBegunPlay())
        {
            component->SetHasBegunPlay(true);
            component->BeginPlay();
        }
    }
}
```

- [ ] **Step 3: Respect derived active state when ticking**

Replace the first guard in `Actor::Tick(float deltaTime)`:

```cpp
if (!IsActive())
    return;
```

Keep the component iteration unchanged:

```cpp
for (auto& component : m_components)
{
    if (component && component->IsEnabled() && component->CanEverTick() && component->IsTickEnabled())
    {
        component->TickComponent(deltaTime);
    }
}
```

- [ ] **Step 4: Mark SceneEntity overrides and sync base active state**

In `Scene/Include/Scene/SceneEntity.h`, replace these declarations:

```cpp
const std::string& GetName() const { return m_name; }
void SetName(const std::string& name) { m_name = name; }
bool IsActive() const { return m_active; }
void SetActive(bool active) { m_active = active; }
```

with:

```cpp
const std::string& GetName() const override { return m_name; }
void SetName(const std::string& name) override
{
    Actor::SetName(name);
    m_name = name;
}

bool IsActive() const override { return m_active; }
void SetActive(bool active) override
{
    Actor::SetActive(active);
    m_active = active;
}
```

This keeps the compatibility fields coherent while allowing code that stores only an `Actor*` to observe and update `SceneEntity` active state correctly.

- [ ] **Step 5: Run focused GREEN**

Run:

```powershell
cmake --build build\Debug --config Debug --target ActorComponentValidation
.\build\Debug\Tests\Debug\ActorComponentValidation.exe
```

Expected after Task 2:

- `SceneEntityActiveStateControlsActorTickThroughActorPointer` passes.
- Lifecycle tests that require `SceneManager::Update()` may still fail until Task 3.

---

## Task 3: Drive Actor Runtime Lifecycle from SceneManager

**Files:**
- Modify: `Scene/Include/Scene/SceneManager.h`
- Modify: `Scene/Private/SceneManager.cpp`

- [ ] **Step 1: Declare lifecycle update and deferred-destroy helpers**

In the private helper section of `Scene/Include/Scene/SceneManager.h`, add:

```cpp
void UpdateEntityLifecycles(float deltaTime);
void QueuePendingDestroy(SceneEntity::Handle handle);
void FlushPendingDestroyEntities();
void DestroyEntityImmediate(SceneEntity::Handle handle);
```

Add these private members near `m_indexNeedsRebuild`:

```cpp
bool m_isDispatchingLifecycles = false;
std::vector<SceneEntity::Handle> m_pendingDestroyEntities;
```

- [ ] **Step 2: Call lifecycle update before spatial dirty collection**

Replace `SceneManager::Update(float deltaTime)` with:

```cpp
void SceneManager::Update(float deltaTime)
{
    if (!m_initialized) return;

    UpdateEntityLifecycles(deltaTime);

    // Collect dirty entities after gameplay/component ticks so transform changes
    // made during the frame are visible to the spatial index update.
    CollectDirtyEntities();

    // Update spatial index if needed
    if (m_config.autoRebuildIndex)
    {
        UpdateDirtyEntities();
    }
}
```

- [ ] **Step 3: Implement active entity lifecycle dispatch**

Add this implementation near `SceneManager::Update()`:

```cpp
void SceneManager::UpdateEntityLifecycles(float deltaTime)
{
    std::vector<SceneEntity::Handle> entityHandles;
    entityHandles.reserve(m_entities.size());
    for (const auto& [handle, entity] : m_entities)
    {
        if (entity)
        {
            entityHandles.push_back(handle);
        }
    }

    m_isDispatchingLifecycles = true;
    for (SceneEntity::Handle handle : entityHandles)
    {
        auto it = m_entities.find(handle);
        if (it == m_entities.end() || !it->second || !it->second->IsActive())
            continue;

        SceneEntity* entity = it->second.get();
        entity->BeginPlay();

        it = m_entities.find(handle);
        if (it == m_entities.end() || !it->second || !it->second->IsActive())
            continue;

        entity = it->second.get();
        entity->Tick(deltaTime);

        it = m_entities.find(handle);
        if (it == m_entities.end() || !it->second || !it->second->IsActive())
            continue;

        entity = it->second.get();
        entity->TickComponents(deltaTime);
    }
    m_isDispatchingLifecycles = false;

    FlushPendingDestroyEntities();
}
```

This intentionally calls both paths:

- `entity->Tick(deltaTime)` handles pure `ActorComponent` ownership.
- `entity->TickComponents(deltaTime)` preserves old `SceneEntity` legacy component behavior.
- The snapshot means entities created during a tick start lifecycle on the next frame.
- Deferred destroy means components may destroy their owner or another entity during lifecycle callbacks without invalidating the current dispatch pass.

- [ ] **Step 4: Implement deferred destroy**

Replace `SceneManager::DestroyEntity(...)` with:

```cpp
void SceneManager::DestroyEntity(SceneEntity::Handle handle)
{
    if (m_isDispatchingLifecycles)
    {
        QueuePendingDestroy(handle);
        return;
    }

    DestroyEntityImmediate(handle);
}
```

Add:

```cpp
void SceneManager::QueuePendingDestroy(SceneEntity::Handle handle)
{
    if (m_entities.find(handle) == m_entities.end())
        return;

    if (std::find(m_pendingDestroyEntities.begin(), m_pendingDestroyEntities.end(), handle) !=
        m_pendingDestroyEntities.end())
    {
        return;
    }

    m_pendingDestroyEntities.push_back(handle);
}

void SceneManager::FlushPendingDestroyEntities()
{
    if (m_pendingDestroyEntities.empty())
        return;

    auto pending = std::move(m_pendingDestroyEntities);
    m_pendingDestroyEntities.clear();

    for (SceneEntity::Handle handle : pending)
    {
        DestroyEntityImmediate(handle);
    }
}
```

- [ ] **Step 5: End play before unregistering on immediate destroy and shutdown**

Move the current body of `SceneManager::DestroyEntity(...)` into `DestroyEntityImmediate(...)` and add `EndPlay()` before unregistering:

```cpp
void SceneManager::DestroyEntityImmediate(SceneEntity::Handle handle)
{
    auto it = m_entities.find(handle);
    if (it != m_entities.end())
    {
        it->second->EndPlay();
        it->second->UnregisterAllComponents();
        it->second->SetSceneManager(nullptr);
        m_entities.erase(it);
        m_indexNeedsRebuild = true;

        if (m_initialized && m_spatialIndex)
        {
            RebuildSpatialIndex();
        }
    }
}
```

In `SceneManager::Shutdown()`, clear lifecycle dispatch state before the entity loop:

```cpp
m_isDispatchingLifecycles = false;
m_pendingDestroyEntities.clear();
```

Then replace the per-entity block:

```cpp
entity->UnregisterAllComponents();
entity->SetSceneManager(nullptr);
```

with:

```cpp
entity->EndPlay();
entity->UnregisterAllComponents();
entity->SetSceneManager(nullptr);
```

- [ ] **Step 6: Run lifecycle GREEN**

Run:

```powershell
cmake --build build\Debug --config Debug --target ActorComponentValidation
.\build\Debug\Tests\Debug\ActorComponentValidation.exe
```

Expected:

- All `ActorComponentValidation` tests pass, including the seven Phase 6 tests.

---

## Task 4: Full Validation, ModelViewer Smoke, Review, and Commit

**Files:**
- No source changes unless validation or review uncovers issues.

- [ ] **Step 1: Build validation set**

Run:

```powershell
cmake --build build\Debug --config Debug --target ActorComponentValidation SpatialComponentValidation ResourceInstantiationValidation RenderSceneValidation SystemIntegrationTest MaterialSystemValidation RenderPassBindingValidation ModelViewer
```

- [ ] **Step 2: Run validation executables**

Run:

```powershell
.\build\Debug\Tests\Debug\ActorComponentValidation.exe
.\build\Debug\Tests\Debug\SpatialComponentValidation.exe
.\build\Debug\Tests\Debug\ResourceInstantiationValidation.exe
.\build\Debug\Tests\Debug\RenderSceneValidation.exe
.\build\Debug\Tests\Debug\SystemIntegrationTest.exe
.\build\Debug\Tests\Debug\MaterialSystemValidation.exe
.\build\Debug\Tests\Debug\RenderPassBindingValidation.exe
```

Expected: all return `0`.

- [ ] **Step 3: Run ModelViewer smoke**

Start ModelViewer hidden, allow it to run for eight seconds, then stop it and inspect logs:

```powershell
Start-Process -FilePath ".\build\Debug\Samples\ModelViewer\Debug\ModelViewer.exe" -RedirectStandardOutput "build\Debug\modelviewer_phase6_smoke.log" -RedirectStandardError "build\Debug\modelviewer_phase6_smoke.err.log" -WindowStyle Hidden -PassThru
```

Expected log signals:

- `Model loaded successfully`
- `Model instantiated as SceneEntity`
- `Scene entity ready`
- `Uploaded mesh to GPU`
- `RenderGraph stats`
- no `critical`, `Assertion Failed`, or `[error]`

- [ ] **Step 4: Request one code review**

Use one reviewer:

```text
DESCRIPTION: Implemented UE-style actor component Phase 6 runtime lifecycle integration. SceneManager now drives BeginPlay/Tick/EndPlay for active SceneEntity actors, preserves legacy Component ticking, begins late-added actor components, respects SceneEntity active state through Actor pointers, and ends play before unregister on destroy/shutdown.
PLAN_OR_REQUIREMENTS: Docs/superpowers/plans/2026-05-15-ue-style-actor-component-phase6-runtime-lifecycle.md
BASE_SHA: commit before Task 1 implementation
HEAD_SHA: current HEAD
```

Fix Critical and Important issues before committing.

- [ ] **Step 5: Commit implementation**

Run:

```powershell
git add Docs/superpowers/plans/2026-05-15-ue-style-actor-component-phase6-runtime-lifecycle.md Tests/ActorComponentValidation/main.cpp Scene/Include/Scene/Actor.h Scene/Private/Actor.cpp Scene/Include/Scene/SceneEntity.h Scene/Include/Scene/SceneManager.h Scene/Private/SceneManager.cpp
git commit -m "Integrate actor component runtime lifecycle"
```

---

## Self-Review

- Spec coverage: Phase 6 completes the missing runtime lifecycle step in the UE-style component migration. The plan covers pure actor component BeginPlay/Tick/EndPlay, late-added runtime components, active-state gating, legacy `Component` tick preservation, lifecycle event ordering, deferred destroy during lifecycle dispatch, and destroy/shutdown lifecycle ordering.
- Placeholder scan: No placeholder tasks remain. Every code-changing step includes concrete code and every validation step includes exact commands and expected outcomes.
- Type consistency: `SceneManagedActorComponent` and `SelfDestroyingActorComponent` derive from `ActorComponent`, enter actors through `Actor::AddOwnedComponent`, and are ticked by `Actor::Tick`. `SceneManagedLegacyTickComponent` derives from `Component`, enters through `SceneEntity::AddComponent`, and remains ticked by `SceneEntity::TickComponents`. `SceneManager::UpdateEntityLifecycles` calls both ownership paths intentionally and uses handle snapshots plus pending destroy storage to keep callbacks mutation-safe.
- Compatibility check: Existing legacy component storage, render primitive registration, prefab/model instantiation, and spatial index update semantics remain in place. The lifecycle update runs before dirty collection so component-driven transform changes are included in the same frame's spatial index update.
- Risk check: The only behavior change for inactive entities is that `SceneManager::Update()` skips gameplay/component ticking while still letting spatial queries rebuild from existing dirty state after reactivation. `EndPlay()` is guarded by `Actor::m_hasBegunPlay`, so explicit `DestroyEntity()` plus later destructor cleanup will not double-end actor components. Destroy requests made during lifecycle dispatch become end-of-dispatch operations rather than immediate erases, which avoids iterator and owner self-destruction hazards.
