# UE-Style Actor Component Phase 2 Transform Migration Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make root `SceneComponent` the transform and attachment source of truth for `Actor` and compatibility `SceneEntity`.

**Architecture:** Keep `SceneEntity` as the compatibility object used by current samples, but give every `SceneEntity` a default root `SceneComponent`. Old transform and hierarchy APIs forward to that root component, while render extraction and spatial indexing continue to consume `SceneEntity` during this phase.

**Tech Stack:** C++20, CMake, Scene module, standalone validation executable, existing `ActorComponentValidation`, `SystemIntegrationTest`, ModelViewer smoke verification.

---

## Scope Review

Phase 1 introduced the new core types. Phase 2 moves transform ownership without changing rendering, spatial query result types, model instantiation return types, or prefab format.

This phase implements:

- Actor-level transform forwarding to `RootComponent`.
- `SceneComponent` world rotation and world scale accessors.
- A default root `SceneComponent` for every `SceneEntity`.
- `SceneEntity` transform getters/setters forwarded to the root component.
- `SceneEntity` hierarchy APIs mirrored into root component attachment.
- Tests proving root attachment and compatibility transform forwarding.

This phase explicitly does not implement:

- `StaticMeshComponent`.
- Primitive registration lists.
- RenderSceneCollector primitive iteration.
- Spatial query results that return primitive components.
- Prefab serialization format changes.
- Removal of old `SceneEntity` compatibility members.

## File Structure

### Modified Files

- `Scene/Include/Scene/SceneComponent.h`
  - Add `GetRelativeScale3D()`, `SetRelativeScale3D()`, `GetWorldRotation()`, and `GetWorldScale()`.
- `Scene/Private/SceneComponent.cpp`
  - Implement world rotation/scale through attachment parents.
- `Scene/Include/Scene/Actor.h`
  - Add transform forwarding API that uses `RootComponent`.
- `Scene/Private/Actor.cpp`
  - Implement actor transform forwarding.
- `Scene/Include/Scene/SceneEntity.h`
  - Include `SceneComponent.h`, expose old transform API as root forwarding, and document compatibility.
- `Scene/Private/SceneEntity.cpp`
  - Create default root component, forward transform/hierarchy operations, keep spatial dirty behavior intact.
- `Tests/ActorComponentValidation/main.cpp`
  - Add RED/GREEN tests for actor root transform and SceneEntity compatibility transform/hierarchy.

---

## Task 1: Add Actor Root Transform Forwarding

**Files:**
- Modify: `Tests/ActorComponentValidation/main.cpp`
- Modify: `Scene/Include/Scene/SceneComponent.h`
- Modify: `Scene/Private/SceneComponent.cpp`
- Modify: `Scene/Include/Scene/Actor.h`
- Modify: `Scene/Private/Actor.cpp`

- [ ] **Step 1: Write failing validation for Actor root transform forwarding**

Add this test to `Tests/ActorComponentValidation/main.cpp`:

```cpp
bool Test_ActorTransformForwardsToRootComponent()
{
    RVX::Actor actor("RootTransformActor");
    auto* root = actor.AddComponent<RVX::SceneComponent>();
    actor.SetRootComponent(root);

    actor.SetPosition(RVX::Vec3(3.0f, 4.0f, 5.0f));
    actor.SetScale(RVX::Vec3(2.0f, 3.0f, 4.0f));

    TEST_ASSERT_EQ(RVX::Vec3(3.0f, 4.0f, 5.0f), root->GetRelativeLocation());
    TEST_ASSERT_EQ(RVX::Vec3(3.0f, 4.0f, 5.0f), actor.GetWorldPosition());
    TEST_ASSERT_EQ(RVX::Vec3(2.0f, 3.0f, 4.0f), root->GetRelativeScale3D());
    TEST_ASSERT_EQ(RVX::Vec3(2.0f, 3.0f, 4.0f), actor.GetWorldScale());
    TEST_ASSERT_EQ(root->GetWorldTransform(), actor.GetWorldMatrix());
    return true;
}
```

Register it in `main()`:

```cpp
suite.AddTest("ActorTransformForwardsToRootComponent",
              Test_ActorTransformForwardsToRootComponent);
```

- [ ] **Step 2: Run RED**

Run:

```powershell
cmake --build build\Debug --config Debug --target ActorComponentValidation
```

Expected: build fails because `Actor::SetPosition`, `Actor::GetWorldPosition`, `Actor::SetScale`, `Actor::GetWorldScale`, `Actor::GetWorldMatrix`, and `SceneComponent::GetRelativeScale3D` do not exist.

- [ ] **Step 3: Extend SceneComponent transform API**

Add declarations to `Scene/Include/Scene/SceneComponent.h`:

```cpp
const Vec3& GetRelativeScale3D() const { return m_relativeScale; }
void SetRelativeScale3D(const Vec3& scale) { SetRelativeScale(scale); }

Quat GetWorldRotation() const;
Vec3 GetWorldScale() const;
```

Add implementations to `Scene/Private/SceneComponent.cpp`:

```cpp
Quat SceneComponent::GetWorldRotation() const
{
    if (m_attachParent)
    {
        return m_attachParent->GetWorldRotation() * m_relativeRotation;
    }
    return m_relativeRotation;
}

Vec3 SceneComponent::GetWorldScale() const
{
    if (m_attachParent)
    {
        return m_attachParent->GetWorldScale() * m_relativeScale;
    }
    return m_relativeScale;
}
```

- [ ] **Step 4: Add Actor transform forwarding API**

Add declarations to `Scene/Include/Scene/Actor.h`:

```cpp
Vec3 GetWorldPosition() const;
Quat GetWorldRotation() const;
Vec3 GetWorldScale() const;
Mat4 GetWorldMatrix() const;

void SetPosition(const Vec3& position);
void SetRotation(const Quat& rotation);
void SetScale(const Vec3& scale);
void Translate(const Vec3& delta);
```

Add implementations to `Scene/Private/Actor.cpp`:

```cpp
Vec3 Actor::GetWorldPosition() const
{
    return m_rootComponent ? m_rootComponent->GetWorldLocation() : Vec3(0.0f);
}

Quat Actor::GetWorldRotation() const
{
    return m_rootComponent ? m_rootComponent->GetWorldRotation() : Quat(1.0f, 0.0f, 0.0f, 0.0f);
}

Vec3 Actor::GetWorldScale() const
{
    return m_rootComponent ? m_rootComponent->GetWorldScale() : Vec3(1.0f);
}

Mat4 Actor::GetWorldMatrix() const
{
    return m_rootComponent ? m_rootComponent->GetWorldTransform() : Mat4(1.0f);
}

void Actor::SetPosition(const Vec3& position)
{
    if (m_rootComponent)
    {
        m_rootComponent->SetRelativeLocation(position);
    }
}

void Actor::SetRotation(const Quat& rotation)
{
    if (m_rootComponent)
    {
        m_rootComponent->SetRelativeRotation(rotation);
    }
}

void Actor::SetScale(const Vec3& scale)
{
    if (m_rootComponent)
    {
        m_rootComponent->SetRelativeScale(scale);
    }
}

void Actor::Translate(const Vec3& delta)
{
    if (m_rootComponent)
    {
        m_rootComponent->SetRelativeLocation(m_rootComponent->GetRelativeLocation() + delta);
    }
}
```

- [ ] **Step 5: Run GREEN**

Run:

```powershell
cmake --build build\Debug --config Debug --target ActorComponentValidation
build\Debug\Tests\Debug\ActorComponentValidation.exe
```

Expected: all ActorComponentValidation tests pass.

---

## Task 2: Make SceneEntity Transform Compatibility Use Root SceneComponent

**Files:**
- Modify: `Tests/ActorComponentValidation/main.cpp`
- Modify: `Scene/Include/Scene/SceneEntity.h`
- Modify: `Scene/Private/SceneEntity.cpp`

- [ ] **Step 1: Write failing validation for SceneEntity root transform forwarding**

Add this test:

```cpp
bool Test_SceneEntityCreatesRootAndForwardsTransform()
{
    RVX::SceneEntity entity("CompatTransformEntity");
    auto* root = entity.GetRootComponent();
    TEST_ASSERT_NOT_NULL(root);

    entity.SetPosition(RVX::Vec3(7.0f, 8.0f, 9.0f));
    entity.SetScale(RVX::Vec3(2.0f, 2.0f, 2.0f));

    TEST_ASSERT_EQ(RVX::Vec3(7.0f, 8.0f, 9.0f), entity.GetPosition());
    TEST_ASSERT_EQ(RVX::Vec3(7.0f, 8.0f, 9.0f), root->GetRelativeLocation());
    TEST_ASSERT_EQ(RVX::Vec3(7.0f, 8.0f, 9.0f), entity.GetWorldPosition());
    TEST_ASSERT_EQ(RVX::Vec3(2.0f, 2.0f, 2.0f), entity.GetScale());
    TEST_ASSERT_EQ(root->GetWorldTransform(), entity.GetWorldMatrix());
    return true;
}
```

Register it:

```cpp
suite.AddTest("SceneEntityCreatesRootAndForwardsTransform",
              Test_SceneEntityCreatesRootAndForwardsTransform);
```

- [ ] **Step 2: Run RED**

Run:

```powershell
cmake --build build\Debug --config Debug --target ActorComponentValidation
build\Debug\Tests\Debug\ActorComponentValidation.exe
```

Expected: test fails because `SceneEntity` does not create a root component and old transform getters still read legacy storage directly.

- [ ] **Step 3: Add compatibility root member**

Update `Scene/Include/Scene/SceneEntity.h`:

```cpp
#include "Scene/SceneComponent.h"
```

Add private member:

```cpp
SceneComponent* m_compatRootComponent = nullptr;
```

Change move operations to deleted because `Actor` ownership is non-movable:

```cpp
SceneEntity(SceneEntity&&) = delete;
SceneEntity& operator=(SceneEntity&&) = delete;
```

- [ ] **Step 4: Create default root in constructor**

Update `Scene/Private/SceneEntity.cpp` constructor:

```cpp
SceneEntity::SceneEntity(const std::string& name)
    : Actor(name)
    , m_handle(GenerateHandle())
    , m_name(name)
{
    m_compatRootComponent = Actor::AddComponent<SceneComponent>();
    SetRootComponent(m_compatRootComponent);
}
```

- [ ] **Step 5: Forward transform API to root component**

Change declarations in `Scene/Include/Scene/SceneEntity.h` so `GetPosition`, `GetRotation`, and `GetScale` are declared instead of inline:

```cpp
const Vec3& GetPosition() const;
void SetPosition(const Vec3& position);

const Quat& GetRotation() const;
void SetRotation(const Quat& rotation);

const Vec3& GetScale() const;
void SetScale(const Vec3& scale);
```

Update implementations in `Scene/Private/SceneEntity.cpp`:

```cpp
const Vec3& SceneEntity::GetPosition() const
{
    return m_compatRootComponent ? m_compatRootComponent->GetRelativeLocation() : m_position;
}

const Quat& SceneEntity::GetRotation() const
{
    return m_compatRootComponent ? m_compatRootComponent->GetRelativeRotation() : m_rotation;
}

const Vec3& SceneEntity::GetScale() const
{
    return m_compatRootComponent ? m_compatRootComponent->GetRelativeScale() : m_scale;
}

void SceneEntity::SetPosition(const Vec3& position)
{
    m_position = position;
    if (m_compatRootComponent)
    {
        m_compatRootComponent->SetRelativeLocation(position);
    }
    MarkTransformDirty();
}

void SceneEntity::SetRotation(const Quat& rotation)
{
    m_rotation = rotation;
    if (m_compatRootComponent)
    {
        m_compatRootComponent->SetRelativeRotation(rotation);
    }
    MarkTransformDirty();
}

void SceneEntity::SetScale(const Vec3& scale)
{
    m_scale = scale;
    if (m_compatRootComponent)
    {
        m_compatRootComponent->SetRelativeScale(scale);
    }
    MarkTransformDirty();
}

Mat4 SceneEntity::GetLocalMatrix() const
{
    if (m_compatRootComponent)
    {
        return m_compatRootComponent->GetRelativeTransform();
    }

    Mat4 localMatrix(1.0f);
    localMatrix = glm::translate(localMatrix, m_position);
    localMatrix *= glm::mat4_cast(m_rotation);
    localMatrix = glm::scale(localMatrix, m_scale);
    return localMatrix;
}

Mat4 SceneEntity::GetWorldMatrix() const
{
    if (m_compatRootComponent)
    {
        return m_compatRootComponent->GetWorldTransform();
    }

    Mat4 localMatrix = GetLocalMatrix();
    return m_parent ? m_parent->GetWorldMatrix() * localMatrix : localMatrix;
}

Vec3 SceneEntity::GetWorldPosition() const
{
    return m_compatRootComponent ? m_compatRootComponent->GetWorldLocation() : m_position;
}

Quat SceneEntity::GetWorldRotation() const
{
    return m_compatRootComponent ? m_compatRootComponent->GetWorldRotation() : m_rotation;
}

Vec3 SceneEntity::GetWorldScale() const
{
    return m_compatRootComponent ? m_compatRootComponent->GetWorldScale() : m_scale;
}
```

Keep `m_position`, `m_rotation`, `m_scale`, `m_worldMatrix`, and dirty flags as compatibility mirrors for this phase.

- [ ] **Step 6: Run GREEN**

Run:

```powershell
cmake --build build\Debug --config Debug --target ActorComponentValidation
build\Debug\Tests\Debug\ActorComponentValidation.exe
```

Expected: all ActorComponentValidation tests pass.

---

## Task 3: Mirror SceneEntity Hierarchy Into SceneComponent Attachment

**Files:**
- Modify: `Tests/ActorComponentValidation/main.cpp`
- Modify: `Scene/Private/SceneEntity.cpp`

- [ ] **Step 1: Write failing validation for hierarchy attachment**

Add this test:

```cpp
bool Test_SceneEntityHierarchyAttachesRootComponents()
{
    RVX::SceneEntity parent("ParentEntity");
    RVX::SceneEntity child("ChildEntity");

    parent.SetPosition(RVX::Vec3(10.0f, 0.0f, 0.0f));
    child.SetPosition(RVX::Vec3(2.0f, 0.0f, 0.0f));

    parent.AddChild(&child);

    TEST_ASSERT_EQ(parent.GetRootComponent(), child.GetRootComponent()->GetAttachParent());
    TEST_ASSERT_EQ(RVX::Vec3(12.0f, 0.0f, 0.0f), child.GetWorldPosition());

    parent.SetPosition(RVX::Vec3(20.0f, 0.0f, 0.0f));
    TEST_ASSERT_EQ(RVX::Vec3(22.0f, 0.0f, 0.0f), child.GetWorldPosition());

    TEST_ASSERT_TRUE(parent.RemoveChild(&child));
    TEST_ASSERT_EQ(nullptr, child.GetRootComponent()->GetAttachParent());
    TEST_ASSERT_EQ(RVX::Vec3(2.0f, 0.0f, 0.0f), child.GetWorldPosition());
    return true;
}
```

Register it:

```cpp
suite.AddTest("SceneEntityHierarchyAttachesRootComponents",
              Test_SceneEntityHierarchyAttachesRootComponents);
```

- [ ] **Step 2: Run RED**

Run:

```powershell
cmake --build build\Debug --config Debug --target ActorComponentValidation
build\Debug\Tests\Debug\ActorComponentValidation.exe
```

Expected: test fails because existing `SceneEntity::AddChild()` does not attach root components, or because `RemoveChild()` does not detach them.

- [ ] **Step 3: Update AddChild**

In `Scene/Private/SceneEntity.cpp`, after setting `child->m_parent = this`, attach roots:

```cpp
if (m_compatRootComponent && child->m_compatRootComponent)
{
    child->m_compatRootComponent->AttachToComponent(m_compatRootComponent);
}
```

- [ ] **Step 4: Update RemoveChild**

Before clearing `child->m_parent`, detach the child root only if it is attached to this parent root:

```cpp
if (m_compatRootComponent && child->m_compatRootComponent &&
    child->m_compatRootComponent->GetAttachParent() == m_compatRootComponent)
{
    child->m_compatRootComponent->DetachFromComponent();
}
```

- [ ] **Step 5: Run GREEN**

Run:

```powershell
cmake --build build\Debug --config Debug --target ActorComponentValidation
build\Debug\Tests\Debug\ActorComponentValidation.exe
```

Expected: all ActorComponentValidation tests pass.

---

## Task 4: Full Phase 2 Regression and Review

**Files:**
- Validation and review only.

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

Run sequentially:

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

Launch `build\Debug\Samples\ModelViewer\Debug\ModelViewer.exe`, capture a screenshot, and verify DamagedHelmet is visible and the latest log tail has no `error`, `critical`, or assertion.

- [ ] **Step 4: Code review gate**

Run:

```powershell
git diff --check
git diff -- Scene Tests
```

Review focus:

- Actor transform methods are no-ops without a root component and do not crash.
- Every `SceneEntity` owns exactly one compatibility root component.
- Old `SceneEntity` transform getters and setters forward to root component state.
- Parent/child compatibility APIs attach/detach root components without creating cycles.
- Spatial dirty and bounds dirty behavior remains intact.
- Render extraction still uses local matrices and preserves ModelViewer output.

If review finds issues, fix them and rerun validation before committing.

- [ ] **Step 5: Commit Phase 2**

Stage only:

```text
Scene/
Tests/
Docs/superpowers/plans/2026-05-14-ue-style-actor-component-phase2-transform.md
```

Commit:

```powershell
git commit -m "Move SceneEntity transforms to root scene components"
```
