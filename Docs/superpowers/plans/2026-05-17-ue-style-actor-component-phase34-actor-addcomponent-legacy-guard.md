# UE-Style Actor Component Phase 34 Actor AddComponent Legacy Guard Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Prevent `Actor::AddComponent<T>()` from inserting legacy `Component` subclasses into the pure Actor component container.

**Architecture:** Keep the deliberate ownership split: pure `ActorComponent` subclasses are owned by `Actor`, while legacy `Component` subclasses must enter through `SceneEntity::AddComponent<T>()` so both the legacy map and UE-style lifecycle bridge stay coherent. The template guard mirrors the existing `Actor::AddOwnedComponent()` runtime rejection.

**Tech Stack:** C++20, RenderVerseX Scene module, standalone `ActorComponentValidation` executable.

---

## File Structure

- Modify `Tests/ActorComponentValidation/main.cpp`
  - Add a regression test proving `Actor*` cannot add a legacy `Component` through `Actor::AddComponent<T>()`.
- Modify `Scene/Include/Scene/Actor.h`
  - Include `Scene/Component.h`.
  - Return `nullptr` immediately for `T` deriving from `Component`.

---

### Task 1: Add Regression Test

**Files:**
- Modify: `Tests/ActorComponentValidation/main.cpp`

- [ ] **Step 1: Add test near `Test_SceneEntityAndComponentAreActorCompatible`**

```cpp
bool Test_ActorAddComponentRejectsLegacyComponentToAvoidContainerSplit()
{
    RVX::SceneEntity entity("ActorLegacyAddGuardEntity");
    RVX::Actor* actor = &entity;
    const size_t initialActorComponents = actor->GetActorComponentCount();

    auto* component = actor->AddComponent<TestLegacyComponent>();

    TEST_ASSERT_EQ(nullptr, component);
    TEST_ASSERT_EQ(initialActorComponents, actor->GetActorComponentCount());
    TEST_ASSERT_EQ(static_cast<size_t>(0), entity.GetComponentCount());
    TEST_ASSERT_EQ(nullptr, entity.GetComponent<TestLegacyComponent>());
    return true;
}
```

- [ ] **Step 2: Register the test**

Add:

```cpp
suite.AddTest("ActorAddComponentRejectsLegacyComponentToAvoidContainerSplit",
              Test_ActorAddComponentRejectsLegacyComponentToAvoidContainerSplit);
```

near the existing actor/component compatibility tests.

- [ ] **Step 3: Run red check**

Run:

```powershell
cmake --build build\Debug --config Debug --target ActorComponentValidation
.\build\Debug\Tests\Debug\ActorComponentValidation.exe
```

Expected before implementation: the new guard test fails because `Actor::AddComponent<T>()` currently creates the legacy component in the wrong container.

---

### Task 2: Guard Actor::AddComponent<T>()

**Files:**
- Modify: `Scene/Include/Scene/Actor.h`

- [ ] **Step 1: Include legacy component type**

Add:

```cpp
#include "Scene/Component.h"
```

- [ ] **Step 2: Add early guard in template**

At the top of `Actor::AddComponent<T>()`, after the static assertion:

```cpp
if constexpr (std::is_base_of_v<Component, T>)
{
    return nullptr;
}
```

- [ ] **Step 3: Run validation**

Run:

```powershell
cmake --build build\Debug --config Debug --target ActorComponentValidation
.\build\Debug\Tests\Debug\ActorComponentValidation.exe
git diff --check
```

Expected: all ActorComponentValidation tests pass; `git diff --check` has no whitespace errors except known line-ending warnings.

---

## Plan Review

**Spec coverage:** This closes the remaining known container-split entry point: `Actor::AddOwnedComponent()` already rejects legacy components, and this phase makes `Actor::AddComponent<T>()` do the same.

**Risk review:** Including `Scene/Component.h` in `Actor.h` is acceptable because `Component.h` depends only on `ActorComponent.h` and a `SceneEntity` forward declaration; it does not include `Actor.h`, so it does not create an include cycle. The guard is `if constexpr`, so pure actor components keep the existing path.

**Placeholder scan:** No placeholders or deferred implementation steps remain.

**Type consistency:** `Component` is in namespace `RVX`, matching the template definition scope in `Actor.h`.
