# UE-Style Actor Component Phase 33 Legacy Component Lifecycle Bridge Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make legacy `Scene::Component` subclasses participate in the same UE-style lifecycle as `ActorComponent` when owned by `SceneEntity`.

**Architecture:** `Actor` lifecycle methods become virtual so `SceneEntity` can extend them polymorphically. `SceneEntity` keeps the legacy container for compatibility, but bridges registration, initialization, begin play, tick, end play, unregister, and removal cleanup through small private helpers on `Component`.

**Tech Stack:** C++20, RenderVerseX Scene module, standalone `ActorComponentValidation` executable.

---

## File Structure

- Modify `Scene/Include/Scene/Actor.h`
  - Make `RegisterAllComponents()`, `UnregisterAllComponents()`, `BeginPlay()`, `Tick(float)`, and `EndPlay()` virtual.
- Modify `Scene/Include/Scene/Component.h`
  - Add private friend-only lifecycle bridge helpers used by `SceneEntity`.
- Modify `Scene/Private/Component.cpp`
  - Implement helper methods that call protected `ActorComponent` state setters and callbacks.
- Modify `Scene/Include/Scene/SceneEntity.h`
  - Declare lifecycle overrides and private legacy lifecycle dispatch helpers.
  - Auto-register legacy components added while the entity is scene-managed.
- Modify `Scene/Private/SceneEntity.cpp`
  - Implement lifecycle overrides.
  - Dispatch legacy components in insertion order for register/begin/tick and reverse order for end/unregister.
  - Remove legacy components with EndPlay, Unregister, OnDetach, OnComponentDestroyed, and owner clear in that order.
- Modify `Scene/Private/SceneManager.cpp`
  - Stop calling `TickComponents()` separately; `SceneEntity::Tick()` becomes the single polymorphic per-frame tick entry point.
- Modify `Tests/ActorComponentValidation/main.cpp`
  - Add regression tests for legacy component lifecycle parity, late-added legacy components, tick removal cleanup, and `Actor*` polymorphic ticking.

---

### Task 1: Add Regression Coverage For Legacy Lifecycle Parity

**Files:**
- Modify: `Tests/ActorComponentValidation/main.cpp`

- [ ] **Step 1: Add lifecycle counters and legacy test component**

Add a `LegacyLifecycleCounters` struct near `RuntimeLifecycleCounters`, then add a `SceneManagedLegacyLifecycleComponent` class near `SceneManagedLegacyTickComponent`:

```cpp
struct LegacyLifecycleCounters
{
    int created = 0;
    int attached = 0;
    int registered = 0;
    int initialized = 0;
    int beganPlay = 0;
    int ticked = 0;
    int endedPlay = 0;
    int unregistered = 0;
    int detached = 0;
    int destroyed = 0;
    float lastDeltaTime = 0.0f;
    std::vector<std::string> events;
};

class SceneManagedLegacyLifecycleComponent : public RVX::Component
{
public:
    explicit SceneManagedLegacyLifecycleComponent(LegacyLifecycleCounters* counters)
        : m_counters(counters)
    {
    }

    const char* GetTypeName() const override { return "SceneManagedLegacyLifecycleComponent"; }

    void OnComponentCreated() override
    {
        if (m_counters) { ++m_counters->created; m_counters->events.push_back("Created"); }
    }

    void OnAttach() override
    {
        if (m_counters) { ++m_counters->attached; m_counters->events.push_back("Attach"); }
    }

    void OnRegister() override
    {
        if (m_counters) { ++m_counters->registered; m_counters->events.push_back("Register"); }
    }

    void InitializeComponent() override
    {
        if (m_counters) { ++m_counters->initialized; m_counters->events.push_back("Initialize"); }
    }

    void BeginPlay() override
    {
        if (m_counters) { ++m_counters->beganPlay; m_counters->events.push_back("BeginPlay"); }
    }

    void Tick(float deltaTime) override
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
        if (m_counters) { ++m_counters->endedPlay; m_counters->events.push_back("EndPlay"); }
    }

    void OnUnregister() override
    {
        if (m_counters) { ++m_counters->unregistered; m_counters->events.push_back("Unregister"); }
    }

    void OnDetach() override
    {
        if (m_counters) { ++m_counters->detached; m_counters->events.push_back("Detach"); }
    }

    void OnComponentDestroyed() override
    {
        if (m_counters) { ++m_counters->destroyed; m_counters->events.push_back("Destroy"); }
    }

private:
    LegacyLifecycleCounters* m_counters = nullptr;
};
```

- [ ] **Step 2: Add scene-managed lifecycle test**

Add test function:

```cpp
bool Test_SceneManagerDispatchesLegacyComponentFullLifecycle()
{
    RVX::SceneManager sceneManager;
    sceneManager.Initialize();

    auto entity = std::make_shared<RVX::SceneEntity>("LegacyLifecycleEntity");
    LegacyLifecycleCounters counters;
    auto* component = entity->AddComponent<SceneManagedLegacyLifecycleComponent>(&counters);
    TEST_ASSERT_NOT_NULL(component);

    sceneManager.AddEntity(entity);
    TEST_ASSERT_TRUE(component->IsRegistered());
    TEST_ASSERT_TRUE(component->IsInitialized());
    TEST_ASSERT_EQ(1, counters.registered);
    TEST_ASSERT_EQ(1, counters.initialized);

    sceneManager.Update(0.125f);
    TEST_ASSERT_TRUE(component->HasBegunPlay());
    TEST_ASSERT_EQ(1, counters.beganPlay);
    TEST_ASSERT_EQ(1, counters.ticked);
    TEST_ASSERT_TRUE(IsNear(0.125f, counters.lastDeltaTime));

    sceneManager.DestroyEntity(entity->GetHandle());
    TEST_ASSERT_EQ(1, counters.endedPlay);
    TEST_ASSERT_EQ(1, counters.unregistered);

    entity.reset();
    TEST_ASSERT_EQ(1, counters.detached);
    TEST_ASSERT_EQ(1, counters.destroyed);

    sceneManager.Shutdown();
    return true;
}
```

- [ ] **Step 3: Add late-add registration/begin-play test**

Add test function:

```cpp
bool Test_SceneManagerBeginsLegacyComponentAddedAfterEntityBeginsPlay()
{
    RVX::SceneManager sceneManager;
    sceneManager.Initialize();

    const auto handle = sceneManager.CreateEntity("LateLegacyLifecycleEntity");
    auto* entity = sceneManager.GetEntity(handle);
    TEST_ASSERT_NOT_NULL(entity);

    sceneManager.Update(0.016f);
    TEST_ASSERT_TRUE(entity->HasBegunPlay());

    LegacyLifecycleCounters counters;
    auto* component = entity->AddComponent<SceneManagedLegacyLifecycleComponent>(&counters);
    TEST_ASSERT_NOT_NULL(component);
    TEST_ASSERT_TRUE(component->IsRegistered());
    TEST_ASSERT_TRUE(component->IsInitialized());
    TEST_ASSERT_FALSE(component->HasBegunPlay());

    sceneManager.Update(0.033f);
    TEST_ASSERT_TRUE(component->HasBegunPlay());
    TEST_ASSERT_EQ(1, counters.beganPlay);
    TEST_ASSERT_EQ(1, counters.ticked);
    TEST_ASSERT_TRUE(IsNear(0.033f, counters.lastDeltaTime));

    sceneManager.Shutdown();
    return true;
}
```

- [ ] **Step 4: Add polymorphic Actor pointer tick test**

Add test function:

```cpp
bool Test_SceneEntityActorPointerTickDispatchesLegacyComponents()
{
    RVX::SceneEntity entity("LegacyActorPointerTickEntity");
    RVX::Actor* actor = &entity;

    LegacyLifecycleCounters counters;
    auto* component = entity.AddComponent<SceneManagedLegacyLifecycleComponent>(&counters);
    TEST_ASSERT_NOT_NULL(component);

    actor->Tick(0.5f);
    TEST_ASSERT_EQ(1, counters.ticked);
    TEST_ASSERT_TRUE(IsNear(0.5f, counters.lastDeltaTime));
    return true;
}
```

- [ ] **Step 5: Add removal cleanup test**

Add test function:

```cpp
bool Test_SceneEntityLegacyRemovalDispatchesLifecycleCleanup()
{
    RVX::SceneManager sceneManager;
    sceneManager.Initialize();

    const auto handle = sceneManager.CreateEntity("LegacyRemovalLifecycleEntity");
    auto* entity = sceneManager.GetEntity(handle);
    TEST_ASSERT_NOT_NULL(entity);

    LegacyLifecycleCounters counters;
    auto* component = entity->AddComponent<SceneManagedLegacyLifecycleComponent>(&counters);
    TEST_ASSERT_NOT_NULL(component);
    sceneManager.Update(0.016f);

    TEST_ASSERT_TRUE(entity->RemoveComponent<SceneManagedLegacyLifecycleComponent>());
    TEST_ASSERT_EQ(nullptr, entity->GetComponent<SceneManagedLegacyLifecycleComponent>());
    TEST_ASSERT_EQ(1, counters.endedPlay);
    TEST_ASSERT_EQ(1, counters.unregistered);
    TEST_ASSERT_EQ(1, counters.detached);
    TEST_ASSERT_EQ(1, counters.destroyed);

    sceneManager.Shutdown();
    return true;
}
```

- [ ] **Step 6: Register the new tests**

Add four `suite.AddTest(...)` entries near existing SceneManager/legacy component tests.

- [ ] **Step 7: Run red check**

Run:

```powershell
.\build\Debug\Tests\ActorComponentValidation\Debug\ActorComponentValidation.exe
```

Expected: at least the new lifecycle tests fail before implementation.

---

### Task 2: Implement Legacy Component Lifecycle Bridge

**Files:**
- Modify: `Scene/Include/Scene/Actor.h`
- Modify: `Scene/Include/Scene/Component.h`
- Modify: `Scene/Private/Component.cpp`
- Modify: `Scene/Include/Scene/SceneEntity.h`
- Modify: `Scene/Private/SceneEntity.cpp`
- Modify: `Scene/Private/SceneManager.cpp`

- [ ] **Step 1: Make Actor lifecycle methods virtual**

Change declarations in `Scene/Include/Scene/Actor.h`:

```cpp
virtual void RegisterAllComponents();
virtual void UnregisterAllComponents();
bool HasBegunPlay() const { return m_hasBegunPlay; }
virtual void BeginPlay();
virtual void Tick(float deltaTime);
virtual void EndPlay();
```

- [ ] **Step 2: Add Component bridge helper declarations**

Add private methods in `Scene/Include/Scene/Component.h`:

```cpp
void RegisterWithOwner();
void UnregisterFromOwner();
void BeginPlayWithOwner();
void EndPlayWithOwner();
```

- [ ] **Step 3: Implement Component bridge helpers**

Add to `Scene/Private/Component.cpp`:

```cpp
void Component::RegisterWithOwner()
{
    if (IsRegistered())
        return;

    SetRegistered(true);
    OnRegister();
    if (!IsInitialized())
    {
        InitializeComponent();
        SetInitialized(true);
    }
}

void Component::UnregisterFromOwner()
{
    if (!IsRegistered())
        return;

    OnUnregister();
    SetRegistered(false);
}

void Component::BeginPlayWithOwner()
{
    if (HasBegunPlay() || !IsEnabled())
        return;

    SetHasBegunPlay(true);
    BeginPlay();
}

void Component::EndPlayWithOwner()
{
    if (!HasBegunPlay())
        return;

    EndPlay();
    SetHasBegunPlay(false);
}
```

- [ ] **Step 4: Declare SceneEntity lifecycle overrides**

Add public overrides in `Scene/Include/Scene/SceneEntity.h`:

```cpp
void RegisterAllComponents() override;
void UnregisterAllComponents() override;
void BeginPlay() override;
void Tick(float deltaTime) override;
void EndPlay() override;
```

Add private helpers:

```cpp
void RegisterLegacyComponent(Component* component);
void UnregisterLegacyComponent(Component* component);
void BeginPlayLegacyComponents();
void EndPlayLegacyComponents();
void RegisterAllLegacyComponents();
void UnregisterAllLegacyComponents();
```

- [ ] **Step 5: Auto-register legacy components added to scene-managed entities**

After storing a legacy component in both `SceneEntity::AddComponent<T>()` and `SceneEntity::AddOwnedComponent()`, call:

```cpp
if (ShouldAutoRegisterComponent(ptr))
{
    RegisterLegacyComponent(ptr);
}
```

- [ ] **Step 6: Implement SceneEntity lifecycle overrides and helpers**

Add definitions in `Scene/Private/SceneEntity.cpp`:

```cpp
void SceneEntity::RegisterLegacyComponent(Component* component)
{
    if (!component)
        return;

    component->RegisterWithOwner();
}

void SceneEntity::UnregisterLegacyComponent(Component* component)
{
    if (!component)
        return;

    component->UnregisterFromOwner();
}

void SceneEntity::RegisterAllLegacyComponents()
{
    auto snapshot = MakeLegacyComponentSnapshot();
    for (Component* component : snapshot)
    {
        if (component && !IsLegacyComponentRemovalPending(std::type_index(typeid(*component))))
        {
            RegisterLegacyComponent(component);
        }
    }
}

void SceneEntity::UnregisterAllLegacyComponents()
{
    auto snapshot = MakeLegacyComponentSnapshot();
    for (auto it = snapshot.rbegin(); it != snapshot.rend(); ++it)
    {
        Component* component = *it;
        if (component && !IsLegacyComponentRemovalPending(std::type_index(typeid(*component))))
        {
            UnregisterLegacyComponent(component);
        }
    }
}

void SceneEntity::BeginPlayLegacyComponents()
{
    auto snapshot = MakeLegacyComponentSnapshot();
    BeginLegacyComponentDispatch();
    for (Component* component : snapshot)
    {
        if (component && !IsLegacyComponentRemovalPending(std::type_index(typeid(*component))))
        {
            component->BeginPlayWithOwner();
        }
    }
    EndLegacyComponentDispatch();
}

void SceneEntity::EndPlayLegacyComponents()
{
    auto snapshot = MakeLegacyComponentSnapshot();
    BeginLegacyComponentDispatch();
    for (auto it = snapshot.rbegin(); it != snapshot.rend(); ++it)
    {
        Component* component = *it;
        if (component && !IsLegacyComponentRemovalPending(std::type_index(typeid(*component))))
        {
            component->EndPlayWithOwner();
        }
    }
    EndLegacyComponentDispatch();
}

void SceneEntity::RegisterAllComponents()
{
    Actor::RegisterAllComponents();
    RegisterAllLegacyComponents();
}

void SceneEntity::UnregisterAllComponents()
{
    UnregisterAllLegacyComponents();
    Actor::UnregisterAllComponents();
}

void SceneEntity::BeginPlay()
{
    Actor::BeginPlay();
    BeginPlayLegacyComponents();
}

void SceneEntity::Tick(float deltaTime)
{
    if (!IsActive())
        return;

    Actor::Tick(deltaTime);
    TickComponents(deltaTime);
}

void SceneEntity::EndPlay()
{
    EndPlayLegacyComponents();
    Actor::EndPlay();
}
```

- [ ] **Step 7: Update legacy removal cleanup**

In `SceneEntity::RemoveLegacyComponentByType()`, before `OnDetach()`:

```cpp
removedComponent->EndPlayWithOwner();
removedComponent->UnregisterFromOwner();
```

- [ ] **Step 8: Update destructor cleanup**

At the beginning of `SceneEntity::~SceneEntity()`, call:

```cpp
EndPlay();
UnregisterAllComponents();
```

Then keep legacy detach/destroy cleanup as the final owner teardown.

- [ ] **Step 9: Make SceneManager tick use the polymorphic SceneEntity tick**

Remove the separate `entity->TickComponents(deltaTime);` block in `SceneManager::Update()` because `SceneEntity::Tick()` now dispatches both actor and legacy components.

- [ ] **Step 10: Run validation**

Run:

```powershell
cmake --build build --config Debug --target ActorComponentValidation
.\build\Debug\Tests\ActorComponentValidation\Debug\ActorComponentValidation.exe
.\build\Debug\Tests\RenderSceneValidation\Debug\RenderSceneValidation.exe
git diff --check
```

Expected: all validation commands exit with code 0, with only known line-ending warnings allowed from `git diff --check`.

---

## Plan Review

**Spec coverage:** This phase covers the concrete remaining lifecycle gap: legacy `Component` subclasses receive registration, initialization, begin play, tick, end play, unregister, detach, destroy, and polymorphic `Actor*` tick behavior while preserving the legacy component container.

**Risk review:** The largest behavioral risk is double ticking legacy components. The plan addresses it by moving legacy tick into `SceneEntity::Tick()` and removing `SceneManager`'s separate `TickComponents()` call. Direct calls to `TickComponents()` remain supported for legacy compatibility tests and tools.

**Placeholder scan:** No placeholder tasks remain. Each code path and validation command is specified.

**Type consistency:** The planned helper methods are private to `Component` and accessible to `SceneEntity` through the existing `friend class SceneEntity`. The lifecycle override declarations require making the corresponding `Actor` methods virtual first.
