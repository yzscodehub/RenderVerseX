# UE-Style Actor Component Phase 7 Component Mutation Safety Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make `Actor` component lifecycle dispatch safe when components add or remove actor components during `BeginPlay()`, `TickComponent()`, or `EndPlay()`.

**Architecture:** `Actor` keeps immediate add semantics so callers still receive a valid component pointer and runtime-created components can auto-register. Lifecycle dispatch switches from direct vector iteration to a raw-pointer snapshot, while removals requested during dispatch are queued and flushed when the outermost dispatch completes. Pending removals are skipped by later stages in the same dispatch, so a component removed by an earlier component does not tick later in that frame.

**Tech Stack:** C++20, RenderVerseX Scene module, custom validation executables, CMake.

## Compatibility Decisions

- Do not change `Actor::AddComponent<T>()` return semantics. It still constructs, stores, calls `OnComponentCreated()`, and auto-registers immediately when the owner is scene-managed.
- Components added during a dispatch are not part of the current dispatch snapshot and begin/tick on the next matching lifecycle call.
- Components removed during a dispatch are marked pending, skipped by later snapshot entries, and actually receive `EndPlay()`, `OnUnregister()`, `OnComponentDestroyed()`, and owner clearing after the outermost dispatch returns.
- Components already pending removal are skipped by `BeginPlay()`, `Tick()`, and `EndPlay()` snapshot dispatch so pending removals cannot receive stale duplicate lifecycle callbacks while the dispatch unwinds.
- Actor destruction clears pending removals before destroying the remaining owned components, so queued components are destroyed once and do not receive duplicate cleanup callbacks.
- Removing the root component remains rejected.
- Legacy `SceneEntity::m_components` removal remains unchanged in this phase. This phase only hardens pure actor component ownership in `Actor::m_components`.

---

## File Structure

- Modify `Tests/ActorComponentValidation/main.cpp`
  - Add mutation-safety tests for add-during-tick, self-remove-during-tick, removing another component before its turn, add-during-BeginPlay, self/sibling removal during EndPlay, and actor destruction while removals are pending.

- Modify `Scene/Include/Scene/Actor.h`
  - Add pending-removal state and private helpers for snapshot dispatch.

- Modify `Scene/Private/Actor.cpp`
  - Add snapshot-based `BeginPlay()`, `Tick()`, and `EndPlay()` traversal.
  - Queue removals requested during dispatch and flush them once dispatch depth returns to zero.
  - Refactor immediate removal into a helper so queued and non-queued removals share the same cleanup path.

---

## Task 1: Add Failing Component Mutation Tests

**Files:**
- Modify: `Tests/ActorComponentValidation/main.cpp`

- [ ] **Step 1: Add mutation helper components**

Add these helper components inside the anonymous namespace after `SelfDestroyingActorComponent`:

```cpp
class TickCounterComponent : public RVX::ActorComponent
{
public:
    explicit TickCounterComponent(int* tickCount = nullptr)
        : m_tickCount(tickCount)
    {
        SetCanEverTick(true);
        SetTickEnabled(true);
    }

    const char* GetClassName() const override { return "TickCounterComponent"; }

    void BeginPlay() override
    {
        if (beginCount)
        {
            ++(*beginCount);
        }
    }

    void TickComponent(float) override
    {
        if (m_tickCount)
        {
            ++(*m_tickCount);
        }
    }

    int* beginCount = nullptr;

private:
    int* m_tickCount = nullptr;
};

class AddComponentDuringTickComponent : public RVX::ActorComponent
{
public:
    explicit AddComponentDuringTickComponent(int* addedTickCount)
        : m_addedTickCount(addedTickCount)
    {
        SetCanEverTick(true);
        SetTickEnabled(true);
    }

    const char* GetClassName() const override { return "AddComponentDuringTickComponent"; }

    void TickComponent(float) override
    {
        if (!m_added && GetOwner())
        {
            GetOwner()->AddComponent<TickCounterComponent>(m_addedTickCount);
            m_added = true;
        }
    }

private:
    int* m_addedTickCount = nullptr;
    bool m_added = false;
};

class AddComponentDuringBeginPlayComponent : public RVX::ActorComponent
{
public:
    explicit AddComponentDuringBeginPlayComponent(int* addedBeginCount)
        : m_addedBeginCount(addedBeginCount)
    {
    }

    const char* GetClassName() const override { return "AddComponentDuringBeginPlayComponent"; }

    void BeginPlay() override
    {
        if (!m_added && GetOwner())
        {
            auto* added = GetOwner()->AddComponent<TickCounterComponent>();
            added->beginCount = m_addedBeginCount;
            m_added = true;
        }
    }

private:
    int* m_addedBeginCount = nullptr;
    bool m_added = false;
};

class SelfRemovingTickComponent : public RVX::ActorComponent
{
public:
    explicit SelfRemovingTickComponent(int* tickCount)
        : m_tickCount(tickCount)
    {
        SetCanEverTick(true);
        SetTickEnabled(true);
    }

    const char* GetClassName() const override { return "SelfRemovingTickComponent"; }

    void TickComponent(float) override
    {
        if (m_tickCount)
        {
            ++(*m_tickCount);
        }
        if (GetOwner())
        {
            GetOwner()->RemoveComponent<SelfRemovingTickComponent>();
        }
    }

private:
    int* m_tickCount = nullptr;
};

class VictimTickComponent : public RVX::ActorComponent
{
public:
    explicit VictimTickComponent(int* tickCount)
        : m_tickCount(tickCount)
    {
        SetCanEverTick(true);
        SetTickEnabled(true);
    }

    const char* GetClassName() const override { return "VictimTickComponent"; }

    void TickComponent(float) override
    {
        if (m_tickCount)
        {
            ++(*m_tickCount);
        }
    }

private:
    int* m_tickCount = nullptr;
};

class RemoveVictimDuringTickComponent : public RVX::ActorComponent
{
public:
    RemoveVictimDuringTickComponent()
    {
        SetCanEverTick(true);
        SetTickEnabled(true);
    }

    const char* GetClassName() const override { return "RemoveVictimDuringTickComponent"; }

    void TickComponent(float) override
    {
        if (GetOwner())
        {
            GetOwner()->RemoveComponent<VictimTickComponent>();
        }
    }
};

struct EndPlayMutationCounters
{
    int removerEndPlay = 0;
    int victimEndPlay = 0;
    int victimDestroyed = 0;
};

class EndPlayVictimComponent : public RVX::ActorComponent
{
public:
    explicit EndPlayVictimComponent(EndPlayMutationCounters* counters)
        : m_counters(counters)
    {
    }

    const char* GetClassName() const override { return "EndPlayVictimComponent"; }

    void EndPlay() override
    {
        if (m_counters)
        {
            ++m_counters->victimEndPlay;
        }
    }

    void OnComponentDestroyed() override
    {
        if (m_counters)
        {
            ++m_counters->victimDestroyed;
        }
    }

private:
    EndPlayMutationCounters* m_counters = nullptr;
};

class SelfRemovingEndPlayComponent : public RVX::ActorComponent
{
public:
    explicit SelfRemovingEndPlayComponent(int* endPlayCount)
        : m_endPlayCount(endPlayCount)
    {
    }

    const char* GetClassName() const override { return "SelfRemovingEndPlayComponent"; }

    void EndPlay() override
    {
        if (m_endPlayCount)
        {
            ++(*m_endPlayCount);
        }
        if (GetOwner())
        {
            GetOwner()->RemoveComponent<SelfRemovingEndPlayComponent>();
        }
    }

private:
    int* m_endPlayCount = nullptr;
};

class RemoveVictimDuringEndPlayComponent : public RVX::ActorComponent
{
public:
    explicit RemoveVictimDuringEndPlayComponent(EndPlayMutationCounters* counters)
        : m_counters(counters)
    {
    }

    const char* GetClassName() const override { return "RemoveVictimDuringEndPlayComponent"; }

    void EndPlay() override
    {
        if (m_counters)
        {
            ++m_counters->removerEndPlay;
        }
        if (GetOwner())
        {
            GetOwner()->RemoveComponent<EndPlayVictimComponent>();
        }
    }

private:
    EndPlayMutationCounters* m_counters = nullptr;
};

class DestructionPendingRemovalComponent : public RVX::ActorComponent
{
public:
    explicit DestructionPendingRemovalComponent(int* destroyedCount)
        : m_destroyedCount(destroyedCount)
    {
        SetCanEverTick(true);
        SetTickEnabled(true);
    }

    const char* GetClassName() const override { return "DestructionPendingRemovalComponent"; }

    void TickComponent(float) override
    {
        if (GetOwner())
        {
            GetOwner()->RemoveComponent<DestructionPendingRemovalComponent>();
        }
    }

    void OnComponentDestroyed() override
    {
        if (m_destroyedCount)
        {
            ++(*m_destroyedCount);
        }
    }

private:
    int* m_destroyedCount = nullptr;
};
```

- [ ] **Step 2: Add add-during-tick test**

Add:

```cpp
bool Test_ActorTickSnapshotsComponentsAddedDuringTick()
{
    RVX::Actor actor("AddDuringTickActor");
    int addedTickCount = 0;
    actor.AddComponent<AddComponentDuringTickComponent>(&addedTickCount);

    actor.Tick(0.016f);
    TEST_ASSERT_EQ(0, addedTickCount);
    TEST_ASSERT_EQ(static_cast<size_t>(2), actor.GetActorComponentCount());

    actor.Tick(0.016f);
    TEST_ASSERT_EQ(1, addedTickCount);
    return true;
}
```

- [ ] **Step 3: Add self-remove-during-tick test**

Add:

```cpp
bool Test_ActorDefersSelfRemovalDuringTick()
{
    RVX::Actor actor("SelfRemoveActor");
    int tickCount = 0;
    actor.AddComponent<SelfRemovingTickComponent>(&tickCount);

    actor.Tick(0.016f);

    TEST_ASSERT_EQ(1, tickCount);
    TEST_ASSERT_EQ(static_cast<size_t>(0), actor.GetActorComponentCount());

    actor.Tick(0.016f);
    TEST_ASSERT_EQ(1, tickCount);
    return true;
}
```

- [ ] **Step 4: Add remove-other-before-turn test**

Add:

```cpp
bool Test_ActorSkipsComponentRemovedEarlierInTick()
{
    RVX::Actor actor("RemoveOtherActor");
    int victimTickCount = 0;
    actor.AddComponent<RemoveVictimDuringTickComponent>();
    actor.AddComponent<VictimTickComponent>(&victimTickCount);

    actor.Tick(0.016f);

    TEST_ASSERT_EQ(0, victimTickCount);
    TEST_ASSERT_EQ(nullptr, actor.GetComponent<VictimTickComponent>());
    return true;
}
```

- [ ] **Step 5: Add add-during-BeginPlay test**

Add:

```cpp
bool Test_ActorBeginPlaySnapshotsComponentsAddedDuringBeginPlay()
{
    RVX::Actor actor("AddDuringBeginPlayActor");
    int addedBeginCount = 0;
    actor.AddComponent<AddComponentDuringBeginPlayComponent>(&addedBeginCount);

    actor.BeginPlay();
    TEST_ASSERT_EQ(0, addedBeginCount);
    TEST_ASSERT_EQ(static_cast<size_t>(2), actor.GetActorComponentCount());

    actor.BeginPlay();
    TEST_ASSERT_EQ(1, addedBeginCount);
    return true;
}
```

- [ ] **Step 6: Register the tests**

- [ ] **Step 6: Add EndPlay mutation and teardown tests**

Add:

```cpp
bool Test_ActorDefersSelfRemovalDuringEndPlay()
{
    RVX::Actor actor("SelfRemoveEndPlayActor");
    int endPlayCount = 0;
    actor.AddComponent<SelfRemovingEndPlayComponent>(&endPlayCount);

    actor.BeginPlay();
    actor.EndPlay();

    TEST_ASSERT_EQ(1, endPlayCount);
    TEST_ASSERT_EQ(static_cast<size_t>(0), actor.GetActorComponentCount());
    return true;
}

bool Test_ActorSkipsComponentRemovedEarlierInEndPlay()
{
    RVX::Actor actor("RemoveOtherEndPlayActor");
    EndPlayMutationCounters counters;
    actor.AddComponent<EndPlayVictimComponent>(&counters);
    actor.AddComponent<RemoveVictimDuringEndPlayComponent>(&counters);

    actor.BeginPlay();
    actor.EndPlay();

    TEST_ASSERT_EQ(1, counters.removerEndPlay);
    TEST_ASSERT_EQ(0, counters.victimEndPlay);
    TEST_ASSERT_EQ(1, counters.victimDestroyed);
    TEST_ASSERT_EQ(nullptr, actor.GetComponent<EndPlayVictimComponent>());
    return true;
}

bool Test_ActorDestructionClearsPendingRemovalWithoutDoubleDestroy()
{
    int destroyedCount = 0;
    {
        RVX::Actor actor("DestroyPendingRemovalActor");
        actor.AddComponent<DestructionPendingRemovalComponent>(&destroyedCount);
        actor.Tick(0.016f);
    }

    TEST_ASSERT_EQ(1, destroyedCount);
    return true;
}
```

- [ ] **Step 7: Register the tests**

Add these before `SceneComponentAttachmentComputesWorldTransform`:

```cpp
suite.AddTest("ActorTickSnapshotsComponentsAddedDuringTick",
              Test_ActorTickSnapshotsComponentsAddedDuringTick);
suite.AddTest("ActorDefersSelfRemovalDuringTick",
              Test_ActorDefersSelfRemovalDuringTick);
suite.AddTest("ActorSkipsComponentRemovedEarlierInTick",
              Test_ActorSkipsComponentRemovedEarlierInTick);
suite.AddTest("ActorBeginPlaySnapshotsComponentsAddedDuringBeginPlay",
              Test_ActorBeginPlaySnapshotsComponentsAddedDuringBeginPlay);
suite.AddTest("ActorDefersSelfRemovalDuringEndPlay",
              Test_ActorDefersSelfRemovalDuringEndPlay);
suite.AddTest("ActorSkipsComponentRemovedEarlierInEndPlay",
              Test_ActorSkipsComponentRemovedEarlierInEndPlay);
suite.AddTest("ActorDestructionClearsPendingRemovalWithoutDoubleDestroy",
              Test_ActorDestructionClearsPendingRemovalWithoutDoubleDestroy);
```

- [ ] **Step 8: Run RED**

Run:

```powershell
cmake --build build\Debug --config Debug --target ActorComponentValidation
.\build\Debug\Tests\Debug\ActorComponentValidation.exe
```

Expected before implementation:

- At least one mutation-safety test fails. The most important expected failures are:
  - added components may tick or begin during the same dispatch, because `Actor` iterates `m_components` directly;
  - removing a component during dispatch may erase from the vector during iteration.
  - components removed during `EndPlay()` may receive stale or duplicate `EndPlay()`/destroy callbacks.

---

## Task 2: Add Snapshot Dispatch and Deferred Removal to Actor

**Files:**
- Modify: `Scene/Include/Scene/Actor.h`
- Modify: `Scene/Private/Actor.cpp`

- [ ] **Step 1: Add private dispatch helpers and state**

In `Scene/Include/Scene/Actor.h`, replace private helper declarations:

```cpp
bool RemoveComponentInstance(ActorComponent* component);
void DestroyAllComponents();
```

with:

```cpp
bool RemoveComponentInstance(ActorComponent* component);
bool RemoveComponentInstanceNow(ActorComponent* component);
bool IsComponentRemovalPending(const ActorComponent* component) const;
void QueuePendingComponentRemoval(ActorComponent* component);
void FlushPendingComponentRemovals();
std::vector<ActorComponent*> MakeComponentSnapshot() const;
void BeginComponentDispatch();
void EndComponentDispatch();
void DestroyAllComponents();
```

Add private members near `m_components`:

```cpp
int32 m_componentDispatchDepth = 0;
std::vector<ActorComponent*> m_pendingRemoveComponents;
```

- [ ] **Step 2: Implement snapshot and dispatch helpers**

In `Scene/Private/Actor.cpp`, add:

```cpp
std::vector<ActorComponent*> Actor::MakeComponentSnapshot() const
{
    std::vector<ActorComponent*> snapshot;
    snapshot.reserve(m_components.size());
    for (const auto& component : m_components)
    {
        if (component)
        {
            snapshot.push_back(component.get());
        }
    }
    return snapshot;
}

void Actor::BeginComponentDispatch()
{
    ++m_componentDispatchDepth;
}

void Actor::EndComponentDispatch()
{
    if (m_componentDispatchDepth <= 0)
        return;

    --m_componentDispatchDepth;
    if (m_componentDispatchDepth == 0)
    {
        FlushPendingComponentRemovals();
    }
}

bool Actor::IsComponentRemovalPending(const ActorComponent* component) const
{
    return std::find(m_pendingRemoveComponents.begin(),
                     m_pendingRemoveComponents.end(),
                     component) != m_pendingRemoveComponents.end();
}

void Actor::QueuePendingComponentRemoval(ActorComponent* component)
{
    if (!component)
        return;

    if (component == static_cast<ActorComponent*>(m_rootComponent))
        return;

    if (IsComponentRemovalPending(component))
        return;

    m_pendingRemoveComponents.push_back(component);
}

void Actor::FlushPendingComponentRemovals()
{
    if (m_pendingRemoveComponents.empty())
        return;

    auto pending = std::move(m_pendingRemoveComponents);
    m_pendingRemoveComponents.clear();

    for (ActorComponent* component : pending)
    {
        RemoveComponentInstanceNow(component);
    }
}
```

- [ ] **Step 3: Refactor removal into immediate and queued paths**

Replace `Actor::RemoveComponentInstance(...)` with:

```cpp
bool Actor::RemoveComponentInstance(ActorComponent* component)
{
    if (!component)
        return false;

    if (component == static_cast<ActorComponent*>(m_rootComponent))
        return false;

    auto it = std::find_if(m_components.begin(), m_components.end(), [component](const auto& ownedComponent) {
        return ownedComponent.get() == component;
    });

    if (it == m_components.end())
        return false;

    if (m_componentDispatchDepth > 0)
    {
        QueuePendingComponentRemoval(component);
        return IsComponentRemovalPending(component);
    }

    return RemoveComponentInstanceNow(component);
}
```

Add:

```cpp
bool Actor::RemoveComponentInstanceNow(ActorComponent* component)
{
    auto it = std::find_if(m_components.begin(), m_components.end(), [component](const auto& ownedComponent) {
        return ownedComponent.get() == component;
    });

    if (it == m_components.end())
        return false;

    if (component == static_cast<ActorComponent*>(m_rootComponent))
        return false;

    if (component->HasBegunPlay())
    {
        component->EndPlay();
        component->SetHasBegunPlay(false);
    }
    if (component->IsRegistered())
    {
        component->OnUnregister();
        component->SetRegistered(false);
    }
    component->OnComponentDestroyed();
    component->SetOwnerActor(nullptr);
    m_components.erase(it);
    return true;
}
```

- [ ] **Step 4: Update BeginPlay to use snapshot dispatch**

Replace `Actor::BeginPlay()` with:

```cpp
void Actor::BeginPlay()
{
    if (!m_hasBegunPlay)
    {
        m_hasBegunPlay = true;
    }

    auto snapshot = MakeComponentSnapshot();
    BeginComponentDispatch();
    for (ActorComponent* component : snapshot)
    {
        if (component && !IsComponentRemovalPending(component) &&
            component->IsEnabled() && !component->HasBegunPlay())
        {
            component->SetHasBegunPlay(true);
            component->BeginPlay();
        }
    }
    EndComponentDispatch();
}
```

- [ ] **Step 5: Update Tick to use snapshot dispatch**

Replace `Actor::Tick(float deltaTime)` component iteration with:

```cpp
auto snapshot = MakeComponentSnapshot();
BeginComponentDispatch();
for (ActorComponent* component : snapshot)
{
    if (component && !IsComponentRemovalPending(component) &&
        component->IsEnabled() && component->CanEverTick() && component->IsTickEnabled())
    {
        component->TickComponent(deltaTime);
    }
}
EndComponentDispatch();
```

Keep the `if (!IsActive()) return;` guard before taking the snapshot.

- [ ] **Step 6: Update EndPlay to use snapshot dispatch**

Replace `Actor::EndPlay()` component iteration with:

```cpp
auto snapshot = MakeComponentSnapshot();
BeginComponentDispatch();
for (auto it = snapshot.rbegin(); it != snapshot.rend(); ++it)
{
    ActorComponent* component = *it;
    if (component && !IsComponentRemovalPending(component) && component->HasBegunPlay())
    {
        component->EndPlay();
        component->SetHasBegunPlay(false);
    }
}
EndComponentDispatch();
m_hasBegunPlay = false;
```

- [ ] **Step 7: Clear pending removals during destroy**

In `Actor::DestroyAllComponents()`, after `UnregisterAllComponents()` and before component destruction loop, add:

```cpp
m_pendingRemoveComponents.clear();
m_componentDispatchDepth = 0;
```

- [ ] **Step 8: Make queued removal cleanup suppress duplicate EndPlay**

In `Actor::FlushPendingComponentRemovals()`, add this before `RemoveComponentInstanceNow(component)`:

```cpp
if (component && component->HasBegunPlay())
{
    component->SetHasBegunPlay(false);
}
```

The component's own callback already ran if it removed itself during `EndPlay()`, or it was deliberately removed before its `EndPlay()` turn. This keeps the shared immediate cleanup path from issuing a second `EndPlay()` while still preserving unregister/destroy/owner clearing.

- [ ] **Step 9: Run GREEN**

Run:

```powershell
cmake --build build\Debug --config Debug --target ActorComponentValidation
.\build\Debug\Tests\Debug\ActorComponentValidation.exe
```

Expected:

- All `ActorComponentValidation` tests pass, including the four Phase 7 mutation tests.
- All `ActorComponentValidation` tests pass, including the seven Phase 7 mutation tests.

---

## Task 3: Full Validation, ModelViewer Smoke, Review, and Commit

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

Run hidden for eight seconds and capture logs:

```powershell
Start-Process -FilePath ".\build\Debug\Samples\ModelViewer\Debug\ModelViewer.exe" -RedirectStandardOutput "build\Debug\modelviewer_phase7_smoke.log" -RedirectStandardError "build\Debug\modelviewer_phase7_smoke.err.log" -WindowStyle Hidden -PassThru
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
DESCRIPTION: Implemented UE-style actor component Phase 7 component mutation safety. Actor lifecycle dispatch now uses component snapshots, queued removals during dispatch, pending-removal skips, and shared immediate cleanup for queued/non-queued removals.
PLAN_OR_REQUIREMENTS: Docs/superpowers/plans/2026-05-15-ue-style-actor-component-phase7-component-mutation-safety.md
BASE_SHA: commit before Task 1 implementation
HEAD_SHA: current HEAD
```

Fix Critical and Important issues before committing.

- [ ] **Step 5: Commit implementation**

Run:

```powershell
git add Docs/superpowers/plans/2026-05-15-ue-style-actor-component-phase7-component-mutation-safety.md Tests/ActorComponentValidation/main.cpp Scene/Include/Scene/Actor.h Scene/Private/Actor.cpp
git commit -m "Harden actor component mutation during lifecycle dispatch"
```

---

## Self-Review

- Spec coverage: Phase 7 targets runtime mutation safety inside actor component dispatch. Tests cover component addition during tick, addition during BeginPlay, self-removal during tick, removing another component before its tick turn, self-removal during EndPlay, removing another component before its EndPlay turn, and actor teardown after a queued removal.
- Placeholder scan: No placeholder tasks remain. Every code-changing step contains concrete code and every validation step includes exact commands.
- Type consistency: Pending-removal helpers operate on `ActorComponent*` because `Actor::m_components` owns all pure actor components. Existing template APIs remain unchanged.
- Compatibility check: Immediate add behavior is preserved. Immediate removal outside dispatch still uses the same cleanup path as before. Root removal remains rejected. Legacy `SceneEntity::m_components` removal is intentionally unchanged.
- Risk check: Snapshot dispatch means newly added components wait until the next lifecycle call, which is consistent with common engine behavior and avoids vector iteration surprises. Queued removal means callbacks can request removal without erasing the component currently executing or invalidating later snapshot entries. Pending removals suppress duplicate `EndPlay()` while still running unregister/destroy cleanup once.
