# UE-Style Actor Component Phase 1 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Introduce the UE-style Actor / ActorComponent / SceneComponent / PrimitiveComponent core without breaking existing SceneEntity-based samples.

**Architecture:** Add the new UE-style runtime types inside the Scene module, keep `SceneEntity` and `Component` as compatibility APIs, and do not migrate render extraction yet. New code can create Actors and scene/primitive components; old code continues to use SceneEntity and legacy components.

**Tech Stack:** C++20, CMake, Scene module, standalone validation executable, existing TestFramework, ModelViewer smoke verification.

---

## Scope Review

The design target is a full UE-style Actor Component System, but this phase only builds the safe foundation:

- New `Actor`, `ActorComponent`, `SceneComponent`, and `PrimitiveComponent` types.
- Root component support on `Actor`.
- Component lifecycle hooks and opt-in tick state.
- Lightweight class metadata via `GetClassName()`.
- Minimal `ActorFactory` and generic ActorComponent creation hooks in existing `ComponentFactory`.
- Compatibility relationship: `SceneEntity : Actor` and `Component : ActorComponent`.

This phase explicitly does not migrate:

- `RenderSceneCollector`
- `SpatialSubsystem`
- `ModelResource::Instantiate`
- prefab serialization format
- old `SceneEntity` transform storage

## File Structure

### New Files

- `Scene/Include/Scene/Actor.h`
  - Actor identity, root component pointer, ActorComponent ownership, lifecycle dispatch.
- `Scene/Private/Actor.cpp`
  - Actor handle generation, lifecycle implementation, component registration helpers.
- `Scene/Include/Scene/ActorComponent.h`
  - Base component lifecycle, owner pointer, registration/tick state, metadata hooks.
- `Scene/Private/ActorComponent.cpp`
  - Default lifecycle and state implementation.
- `Scene/Include/Scene/SceneComponent.h`
  - Transform and attachment component.
- `Scene/Private/SceneComponent.cpp`
  - Transform caching, dirty propagation, attach/detach.
- `Scene/Include/Scene/PrimitiveComponent.h`
  - Visibility, layer mask, bounds, render extraction hook.
- `Scene/Private/PrimitiveComponent.cpp`
  - World bounds and dirty state implementation.
- `Scene/Include/Scene/ActorFactory.h`
  - String-to-Actor factory registration.
- `Scene/Private/ActorFactory.cpp`
  - Actor factory storage and creation.
- `Scene/Include/Scene/Scene.h`
  - Module umbrella header exports for the new actor/component types.
- `Tests/ActorComponentValidation/main.cpp`
  - Phase 1 behavior validation.

### Modified Files

- `Scene/Include/Scene/Component.h`
  - Make legacy `Component` inherit `ActorComponent`.
  - Keep `SceneEntity* GetOwner()` compatibility API.
- `Scene/Private/Component.cpp`
  - Bridge legacy owner assignment into ActorComponent owner state.
- `Scene/Include/Scene/SceneEntity.h`
  - Make `SceneEntity` inherit `Actor`.
- `Scene/Private/SceneEntity.cpp`
  - Initialize Actor base with entity name.
- `Scene/Include/Scene/ComponentFactory.h`
  - Add generic ActorComponent factory methods without breaking model-node creators.
- `Scene/Private/ComponentFactory.cpp`
  - Add generic ActorComponent factory storage.
- `Scene/Include/Scene/Scene.h`
  - Export the new actor/component headers from the module umbrella header.
- `Scene/CMakeLists.txt`
  - Add new Scene sources.
- `Tests/CMakeLists.txt`
  - Add `ActorComponentValidation` executable.

---

## Task 1: Add ActorComponent Base

**Files:**
- Create: `Scene/Include/Scene/ActorComponent.h`
- Create: `Scene/Private/ActorComponent.cpp`
- Test: `Tests/ActorComponentValidation/main.cpp`
- Modify: `Tests/CMakeLists.txt`

- [ ] **Step 1: Write failing validation for ActorComponent lifecycle state**

Create `Tests/ActorComponentValidation/main.cpp` with a test that includes `Scene/ActorComponent.h`, and add the `ActorComponentValidation` target to `Tests/CMakeLists.txt`.

```cpp
class TestComponent : public RVX::ActorComponent
{
public:
    const char* GetClassName() const override { return "TestComponent"; }
    void OnRegister() override { registered = true; }
    void InitializeComponent() override { initialized = true; }
    void BeginPlay() override { beganPlay = true; }
    void TickComponent(float deltaTime) override
    {
        ticked = true;
        lastDeltaTime = deltaTime;
    }
    void EndPlay() override { endedPlay = true; }
    void OnUnregister() override { unregistered = true; }

    bool registered = false;
    bool initialized = false;
    bool beganPlay = false;
    bool ticked = false;
    bool endedPlay = false;
    bool unregistered = false;
    float lastDeltaTime = 0.0f;
};

bool Test_ActorComponentLifecycleDefaults()
{
    TestComponent component;
    TEST_ASSERT_TRUE(component.IsEnabled());
    TEST_ASSERT_FALSE(component.IsRegistered());
    TEST_ASSERT_FALSE(component.CanEverTick());
    TEST_ASSERT_FALSE(component.IsTickEnabled());

    component.SetCanEverTick(true);
    component.SetTickEnabled(true);
    TEST_ASSERT_TRUE(component.CanEverTick());
    TEST_ASSERT_TRUE(component.IsTickEnabled());
    TEST_ASSERT_EQ(std::string("TestComponent"), std::string(component.GetClassName()));
    return true;
}
```

- [ ] **Step 2: Run RED**

Run:

```powershell
cmake --build build\Debug --config Debug --target ActorComponentValidation
```

Expected: build fails because `Scene/ActorComponent.h` does not exist.

- [ ] **Step 3: Add ActorComponent**

Create `ActorComponent.h/.cpp` with:

```cpp
class ActorComponent
{
public:
    ActorComponent() = default;
    virtual ~ActorComponent() = default;

    ActorComponent(const ActorComponent&) = delete;
    ActorComponent& operator=(const ActorComponent&) = delete;

    virtual const char* GetClassName() const { return "ActorComponent"; }

    Actor* GetOwner() const { return m_owner; }
    bool IsRegistered() const { return m_registered; }
    bool IsEnabled() const { return m_enabled; }
    void SetEnabled(bool enabled) { m_enabled = enabled; }

    bool CanEverTick() const { return m_canEverTick; }
    void SetCanEverTick(bool canEverTick);
    bool IsTickEnabled() const { return m_tickEnabled; }
    void SetTickEnabled(bool enabled);

    virtual void OnComponentCreated() {}
    virtual void OnRegister() {}
    virtual void InitializeComponent() {}
    virtual void BeginPlay() {}
    virtual void TickComponent(float deltaTime) { (void)deltaTime; }
    virtual void EndPlay() {}
    virtual void OnUnregister() {}
    virtual void OnComponentDestroyed() {}

protected:
    void SetOwnerActor(Actor* owner) { m_owner = owner; }
    void SetRegistered(bool registered) { m_registered = registered; }

private:
    friend class Actor;
    Actor* m_owner = nullptr;
    bool m_enabled = true;
    bool m_registered = false;
    bool m_canEverTick = false;
    bool m_tickEnabled = false;
};
```

- [ ] **Step 4: Register Scene source**

Add `Private/ActorComponent.cpp` to `Scene/CMakeLists.txt`.

- [ ] **Step 5: Run GREEN**

Run:

```powershell
cmake --build build\Debug --config Debug --target ActorComponentValidation
build\Debug\Tests\Debug\ActorComponentValidation.exe
```

Expected: lifecycle default test passes.

---

## Task 2: Add Actor and Root Component Ownership

**Files:**
- Create: `Scene/Include/Scene/Actor.h`
- Create: `Scene/Private/Actor.cpp`
- Modify: `Tests/ActorComponentValidation/main.cpp`
- Modify: `Scene/CMakeLists.txt`

- [ ] **Step 1: Write failing Actor ownership test**

Add:

```cpp
class CountingComponent : public RVX::ActorComponent
{
public:
    const char* GetClassName() const override { return "CountingComponent"; }
    void OnComponentCreated() override { ++created; }
    void OnRegister() override { ++registered; }
    void InitializeComponent() override { ++initialized; }
    void BeginPlay() override { ++beganPlay; }
    void TickComponent(float) override { ++ticked; }
    void EndPlay() override { ++endedPlay; }
    void OnUnregister() override { ++unregistered; }
    void OnComponentDestroyed() override { ++destroyed; }

    int created = 0;
    int registered = 0;
    int initialized = 0;
    int beganPlay = 0;
    int ticked = 0;
    int endedPlay = 0;
    int unregistered = 0;
    int destroyed = 0;
};

bool Test_ActorOwnsComponentsAndDispatchesLifecycle()
{
    RVX::Actor actor("LifecycleActor");
    auto* component = actor.AddComponent<CountingComponent>();
    component->SetCanEverTick(true);
    component->SetTickEnabled(true);

    TEST_ASSERT_EQ(&actor, component->GetOwner());
    TEST_ASSERT_EQ(1, component->created);

    actor.RegisterAllComponents();
    TEST_ASSERT_TRUE(component->IsRegistered());
    TEST_ASSERT_EQ(1, component->registered);
    TEST_ASSERT_EQ(1, component->initialized);

    actor.BeginPlay();
    actor.Tick(0.25f);
    actor.EndPlay();
    actor.UnregisterAllComponents();

    TEST_ASSERT_EQ(1, component->beganPlay);
    TEST_ASSERT_EQ(1, component->ticked);
    TEST_ASSERT_EQ(1, component->endedPlay);
    TEST_ASSERT_EQ(1, component->unregistered);
    return true;
}
```

- [ ] **Step 2: Run RED**

Run `ActorComponentValidation.exe`.

Expected: build fails because `Actor` does not exist.

- [ ] **Step 3: Implement Actor**

Implement `Actor` with:

- stable handle generated from `std::atomic<Handle>`
- name and active state
- insertion-ordered component ownership using `std::vector<std::unique_ptr<ActorComponent>>`
- `AddComponent<T>()` allowing multiple components of the same concrete type
- `GetComponent<T>()` returning the first matching component
- `RemoveComponent<T>()` removing the first matching component
- `RegisterAllComponents()`
- `UnregisterAllComponents()`
- `BeginPlay()`, `Tick()`, `EndPlay()`
- `SceneComponent* m_rootComponent`

- [ ] **Step 4: Run GREEN**

Run:

```powershell
cmake --build build\Debug --config Debug --target ActorComponentValidation
build\Debug\Tests\Debug\ActorComponentValidation.exe
```

Expected: Actor ownership/lifecycle test passes.

---

## Task 3: Add SceneComponent and PrimitiveComponent

**Files:**
- Create: `Scene/Include/Scene/SceneComponent.h`
- Create: `Scene/Private/SceneComponent.cpp`
- Create: `Scene/Include/Scene/PrimitiveComponent.h`
- Create: `Scene/Private/PrimitiveComponent.cpp`
- Modify: `Tests/ActorComponentValidation/main.cpp`
- Modify: `Scene/CMakeLists.txt`

- [ ] **Step 1: Write failing transform and primitive tests**

Add tests requiring:

```cpp
bool Test_SceneComponentAttachmentComputesWorldTransform()
{
    RVX::Actor actor("TransformActor");
    auto* root = actor.AddComponent<RVX::SceneComponent>();
    auto* child = actor.AddComponent<RVX::SceneComponent>();

    actor.SetRootComponent(root);
    root->SetRelativeLocation(RVX::Vec3(10.0f, 0.0f, 0.0f));
    child->SetRelativeLocation(RVX::Vec3(2.0f, 0.0f, 0.0f));
    TEST_ASSERT_TRUE(child->AttachToComponent(root));

    TEST_ASSERT_EQ(RVX::Vec3(12.0f, 0.0f, 0.0f), child->GetWorldLocation());
    TEST_ASSERT_EQ(root, child->GetAttachParent());
    TEST_ASSERT_EQ(static_cast<size_t>(1), root->GetAttachChildren().size());
    return true;
}
```

This also verifies that new `Actor` ownership supports multiple components of the same concrete type.

Add primitive test:

```cpp
bool Test_PrimitiveComponentTracksVisibilityLayerAndBounds()
{
    RVX::PrimitiveComponent primitive;
    primitive.SetVisible(false);
    primitive.SetLayerMask(0x04);
    primitive.SetLocalBounds(RVX::AABB(RVX::Vec3(-1.0f), RVX::Vec3(1.0f)));

    TEST_ASSERT_FALSE(primitive.IsVisible());
    TEST_ASSERT_EQ(static_cast<RVX::uint32>(0x04), primitive.GetLayerMask());
    TEST_ASSERT_TRUE(primitive.GetLocalBounds().IsValid());
    TEST_ASSERT_TRUE(primitive.GetWorldBounds().IsValid());
    return true;
}
```

- [ ] **Step 2: Run RED**

Run `ActorComponentValidation.exe`.

Expected: build fails because `SceneComponent` and `PrimitiveComponent` do not exist.

- [ ] **Step 3: Implement SceneComponent**

Implement relative transform, cached world transform, attach/detach, and dirty propagation.

- [ ] **Step 4: Implement PrimitiveComponent**

Implement visibility, layer mask, local bounds, world bounds, and render extraction stubs:

```cpp
virtual bool HasRenderData() const { return false; }
virtual void CollectRenderData(RenderScene& scene) const { (void)scene; }
```

- [ ] **Step 5: Run GREEN**

Run:

```powershell
cmake --build build\Debug --config Debug --target ActorComponentValidation
build\Debug\Tests\Debug\ActorComponentValidation.exe
```

Expected: all ActorComponentValidation tests pass.

---

## Task 4: Add Factories and Compatibility Inheritance

**Files:**
- Create: `Scene/Include/Scene/ActorFactory.h`
- Create: `Scene/Private/ActorFactory.cpp`
- Modify: `Scene/Include/Scene/Component.h`
- Modify: `Scene/Private/Component.cpp`
- Modify: `Scene/Include/Scene/SceneEntity.h`
- Modify: `Scene/Private/SceneEntity.cpp`
- Modify: `Scene/Include/Scene/ComponentFactory.h`
- Modify: `Scene/Private/ComponentFactory.cpp`
- Modify: `Tests/ActorComponentValidation/main.cpp`
- Modify: `Scene/CMakeLists.txt`

- [ ] **Step 1: Write failing compatibility and factory tests**

Add:

```cpp
bool Test_SceneEntityAndComponentAreActorCompatible()
{
    TEST_ASSERT_TRUE((std::is_base_of_v<RVX::Actor, RVX::SceneEntity>));
    TEST_ASSERT_TRUE((std::is_base_of_v<RVX::ActorComponent, RVX::Component>));

    RVX::SceneEntity entity("CompatEntity");
    auto* component = entity.AddComponent<TestLegacyComponent>();
    auto* actorComponent = static_cast<RVX::ActorComponent*>(component);

    TEST_ASSERT_EQ(&entity, component->GetOwner());
    TEST_ASSERT_EQ(static_cast<RVX::Actor*>(&entity), actorComponent->GetOwner());
    return true;
}
```

With:

```cpp
class TestLegacyComponent : public RVX::Component
{
public:
    const char* GetTypeName() const override { return "TestLegacyComponent"; }
};
```

Add:

```cpp
bool Test_ActorAndComponentFactoriesCreateRegisteredTypes()
{
    RVX::ActorFactory::ClearAll();
    RVX::ActorFactory::Register<RVX::Actor>("Actor");
    auto actor = RVX::ActorFactory::Create("Actor");
    TEST_ASSERT_NOT_NULL(actor.get());
    TEST_ASSERT_EQ(std::string("Actor"), std::string(actor->GetClassName()));

    RVX::ComponentFactory::ClearComponentClasses();
    RVX::ComponentFactory::RegisterComponentClass<RVX::SceneComponent>("SceneComponent");
    auto component = RVX::ComponentFactory::CreateComponentByClassName("SceneComponent");
    TEST_ASSERT_NOT_NULL(component.get());
    TEST_ASSERT_EQ(std::string("SceneComponent"), std::string(component->GetClassName()));
    return true;
}
```

- [ ] **Step 2: Run RED**

Run `ActorComponentValidation.exe`.

Expected: build fails because factories and compatibility inheritance are missing.

- [ ] **Step 3: Implement ActorFactory**

Add template registration and string creation returning `std::unique_ptr<Actor>`.

- [ ] **Step 4: Make Component inherit ActorComponent**

Update legacy `Component` to inherit `ActorComponent`, keep `SceneEntity* GetOwner()`, bridge `GetClassName()` to `GetTypeName()`, forward `TickComponent()` to the legacy `Tick()` hook, and define `SetOwner(SceneEntity*)` in `Component.cpp` so it also calls `SetOwnerActor(owner)`.

- [ ] **Step 5: Make SceneEntity inherit Actor**

Update `SceneEntity` declaration to:

```cpp
class SceneEntity : public Actor, public Spatial::ISpatialEntity
```

Initialize the base in `SceneEntity::SceneEntity`:

```cpp
SceneEntity::SceneEntity(const std::string& name)
    : Actor(name)
    , m_handle(GenerateHandle())
    , m_name(name)
{
}
```

Keep the legacy `SceneEntity` name/handle/spatial APIs authoritative during Phase 1 while the base `Actor` handle/name API is used by new actor-only paths.

- [ ] **Step 6: Extend ComponentFactory**

Add generic component class registration without changing existing model-node creator APIs:

```cpp
template<typename T>
static void RegisterComponentClass(const std::string& typeName);
static std::unique_ptr<ActorComponent> CreateComponentByClassName(const std::string& typeName);
static void ClearComponentClasses();
```

- [ ] **Step 7: Update Scene umbrella header**

Update `Scene/Include/Scene/Scene.h` so users of the module header can see `Actor`, `ActorComponent`, `SceneComponent`, `PrimitiveComponent`, and `ActorFactory`.

- [ ] **Step 8: Run GREEN**

Run:

```powershell
cmake --build build\Debug --config Debug --target ActorComponentValidation
build\Debug\Tests\Debug\ActorComponentValidation.exe
```

Expected: all ActorComponentValidation tests pass.

---

## Task 5: Full Phase 1 Regression

**Files:**
- Validation only.

- [ ] **Step 1: Build and run focused tests**

Run:

```powershell
cmake --build build\Debug --config Debug --target ActorComponentValidation
build\Debug\Tests\Debug\ActorComponentValidation.exe
cmake --build build\Debug --config Debug --target SystemIntegrationTest
build\Debug\Tests\Debug\SystemIntegrationTest.exe
```

Expected: all tests pass.

- [ ] **Step 2: Run render/material regression tests**

Run:

```powershell
cmake --build build\Debug --config Debug --target MaterialSystemValidation
build\Debug\Tests\Debug\MaterialSystemValidation.exe
cmake --build build\Debug --config Debug --target RenderPassBindingValidation
build\Debug\Tests\Debug\RenderPassBindingValidation.exe
```

Expected: all tests pass.

- [ ] **Step 3: Run ModelViewer smoke**

Run:

```powershell
cmake --build build\Debug --config Debug --target ModelViewer
```

Then launch `build\Debug\Samples\ModelViewer\Debug\ModelViewer.exe`, capture a screenshot, and verify DamagedHelmet is visible and latest log tail has no `error`, `critical`, or assertion.

- [ ] **Step 4: Review Phase 1 implementation diff**

Run:

```powershell
git diff --check
git diff -- Scene Tests
```

Review focus:

- Lifecycle callbacks are not double-fired.
- Component ownership cannot dangle after removal or actor destruction.
- Scene transform dirty propagation is recursive.
- Compatibility changes do not alter `SceneEntity` behavior for current samples.
- Public headers preserve include order and member initialization style from `AGENTS.md`.

If review finds issues, fix them and rerun validation before committing.

- [ ] **Step 5: Commit Phase 1**

Stage only:

```text
Scene/
Tests/
Docs/superpowers/plans/2026-05-14-ue-style-actor-component-phase1.md
```

Commit:

```powershell
git commit -m "Add UE-style actor component foundation"
```
