# UE-Style Actor Component Phase 23 Component Instance Names Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give `ActorComponent` instances stable owner-unique names that can be used by later prefab override and duplicate component addressing phases.

**Architecture:** Add lightweight name storage to `ActorComponent` and make `Actor` assign a unique name when a component is added. Empty names default to the component class name; duplicate names get numeric suffixes such as `TestComponent_1`. This phase does not change prefab serialization, does not add component rename validation after attachment, and does not change legacy `SceneEntity` component uniqueness rules.

**Tech Stack:** C++20, RenderVerseX Scene module, standalone `ActorComponentValidation`.

---

## Scope

- `ActorComponent` exposes `GetName()` and `SetName(std::string)`.
- A newly constructed `ActorComponent` has an empty name before being added to an actor.
- `Actor::AddComponent<T>()` assigns a unique name before `OnComponentCreated()`.
- `Actor::AddOwnedComponent(...)` preserves explicit names when unique and uniquifies collisions.
- Empty component names default to `GetClassName()`.
- Duplicate component names get `_1`, `_2`, etc.
- The compatibility root `SceneComponent` created by `SceneEntity` receives the default name `SceneComponent`.
- Do not add owner-aware runtime rename validation in this phase.
- Do not change prefab component data or override format in this phase.

## Files

- Modify: `E:\WorkSpace\RenderVerseX\Tests\ActorComponentValidation\main.cpp`
  - Add tests for default names, duplicate names, explicit names, and SceneEntity compatibility root naming.
- Modify: `E:\WorkSpace\RenderVerseX\Scene\Include\Scene\ActorComponent.h`
  - Add component name API and storage.
- Modify: `E:\WorkSpace\RenderVerseX\Scene\Include\Scene\Actor.h`
  - Add protected component naming helpers and call them from `AddComponent<T>()`.
- Modify: `E:\WorkSpace\RenderVerseX\Scene\Private\Actor.cpp`
  - Implement component naming helpers and call them from `AddOwnedComponent(...)`.
- Modify: `E:\WorkSpace\RenderVerseX\Docs\superpowers\plans\2026-05-15-ue-style-actor-component-phase23-component-instance-names.md`
  - Track completion status.

---

## Task 1: Add Failing Component Name Tests

**Files:**
- Modify: `E:\WorkSpace\RenderVerseX\Tests\ActorComponentValidation\main.cpp`

- [ ] **Step 1: Add default and duplicate component name test**

Add this test after `Test_ActorComponentLifecycleDefaults`:

```cpp
    bool Test_ActorAssignsUniqueDefaultComponentNames()
    {
        RVX::Actor actor("NamedActor");

        auto* first = actor.AddComponent<TestComponent>();
        auto* second = actor.AddComponent<TestComponent>();

        TEST_ASSERT_NOT_NULL(first);
        TEST_ASSERT_NOT_NULL(second);
        TEST_ASSERT_EQ(std::string("TestComponent"), first->GetName());
        TEST_ASSERT_EQ(std::string("TestComponent_1"), second->GetName());
        return true;
    }
```

- [ ] **Step 2: Add explicit component name collision test**

Add this test after the default name test:

```cpp
    bool Test_ActorAddOwnedComponentUniquifiesExplicitComponentNames()
    {
        RVX::Actor actor("ExplicitNameActor");

        auto firstComponent = std::make_unique<TestComponent>();
        firstComponent->SetName("SharedName");
        auto* first = static_cast<TestComponent*>(
            actor.AddOwnedComponent(std::move(firstComponent)));

        auto secondComponent = std::make_unique<TestComponent>();
        secondComponent->SetName("SharedName");
        auto* second = static_cast<TestComponent*>(
            actor.AddOwnedComponent(std::move(secondComponent)));

        TEST_ASSERT_NOT_NULL(first);
        TEST_ASSERT_NOT_NULL(second);
        TEST_ASSERT_EQ(std::string("SharedName"), first->GetName());
        TEST_ASSERT_EQ(std::string("SharedName_1"), second->GetName());
        return true;
    }
```

- [ ] **Step 3: Add SceneEntity compatibility root name test**

Add this test after the explicit name test:

```cpp
    bool Test_SceneEntityCompatibilityRootGetsDefaultComponentName()
    {
        RVX::SceneEntity entity("NamedSceneEntity");

        TEST_ASSERT_NOT_NULL(entity.GetRootComponent());
        TEST_ASSERT_EQ(std::string("SceneComponent"), entity.GetRootComponent()->GetName());
        return true;
    }
```

- [ ] **Step 4: Register the three tests**

Register them in `main()` immediately after `ActorComponentLifecycleDefaults`:

```cpp
    suite.AddTest("ActorAssignsUniqueDefaultComponentNames",
                  Test_ActorAssignsUniqueDefaultComponentNames);
    suite.AddTest("ActorAddOwnedComponentUniquifiesExplicitComponentNames",
                  Test_ActorAddOwnedComponentUniquifiesExplicitComponentNames);
    suite.AddTest("SceneEntityCompatibilityRootGetsDefaultComponentName",
                  Test_SceneEntityCompatibilityRootGetsDefaultComponentName);
```

- [ ] **Step 5: Build and confirm RED**

Run:

```powershell
cmake --build build\Debug --config Debug --target ActorComponentValidation
```

Expected before implementation: the target fails to compile because `ActorComponent` does not yet expose `GetName()` or `SetName(...)`.

---

## Task 2: Add Component Name Storage And Assignment

**Files:**
- Modify: `E:\WorkSpace\RenderVerseX\Scene\Include\Scene\ActorComponent.h`
- Modify: `E:\WorkSpace\RenderVerseX\Scene\Include\Scene\Actor.h`
- Modify: `E:\WorkSpace\RenderVerseX\Scene\Private\Actor.cpp`

- [ ] **Step 1: Add `ActorComponent` name API**

In `ActorComponent.h`, add `<utility>` to the standard includes:

```cpp
#include <string>
#include <utility>
```

Add these methods in the `Owner and State` section before `GetOwner()`:

```cpp
        const std::string& GetName() const { return m_name; }
        void SetName(std::string name) { m_name = std::move(name); }
```

Add this private member before `m_owner`:

```cpp
        std::string m_name;
```

- [ ] **Step 2: Declare Actor naming helpers**

In `Actor.h`, add these protected methods before the private section:

```cpp
    protected:
        void AssignComponentName(ActorComponent* component);
        std::string MakeUniqueComponentName(const std::string& baseName,
                                            const ActorComponent* ignoredComponent = nullptr) const;
        bool IsComponentNameInUse(const std::string& name,
                                  const ActorComponent* ignoredComponent = nullptr) const;
```

- [ ] **Step 3: Call naming helper from `Actor::AddComponent<T>()`**

In `Actor.h`, inside `Actor::AddComponent<T>()`, after `ptr` is assigned and before `ptr->SetOwnerActor(this);`, add:

```cpp
        AssignComponentName(ptr);
```

- [ ] **Step 4: Implement naming helpers in Actor.cpp**

In `Actor.cpp`, add this implementation after `Actor::~Actor()`:

```cpp
bool Actor::IsComponentNameInUse(const std::string& name,
                                 const ActorComponent* ignoredComponent) const
{
    for (const auto& component : m_components)
    {
        if (!component || component.get() == ignoredComponent)
        {
            continue;
        }

        if (component->GetName() == name)
        {
            return true;
        }
    }

    return false;
}

std::string Actor::MakeUniqueComponentName(const std::string& baseName,
                                           const ActorComponent* ignoredComponent) const
{
    const std::string resolvedBaseName = baseName.empty() ? "ActorComponent" : baseName;
    std::string candidate = resolvedBaseName;
    size_t suffix = 1;

    while (IsComponentNameInUse(candidate, ignoredComponent))
    {
        candidate = resolvedBaseName + "_" + std::to_string(suffix);
        ++suffix;
    }

    return candidate;
}

void Actor::AssignComponentName(ActorComponent* component)
{
    if (!component)
    {
        return;
    }

    const std::string baseName =
        component->GetName().empty() ? component->GetClassName() : component->GetName();
    component->SetName(MakeUniqueComponentName(baseName, component));
}
```

- [ ] **Step 5: Call naming helper from `Actor::AddOwnedComponent(...)`**

In `Actor.cpp`, inside `Actor::AddOwnedComponent(...)`, after `ptr` is assigned and before `ptr->SetOwnerActor(this);`, add:

```cpp
    AssignComponentName(ptr);
```

- [ ] **Step 6: Build and run focused validation**

Run:

```powershell
cmake --build build\Debug --config Debug --target ActorComponentValidation
.\build\Debug\Tests\Debug\ActorComponentValidation.exe
```

Expected result: all `ActorComponentValidation` tests pass, including the three new component name tests.

---

## Task 3: Validate, Review, And Commit

**Files:**
- No additional source files.

- [ ] **Step 1: Run whitespace check**

```powershell
git diff --check
```

Expected result: exit code 0. Line-ending warnings are acceptable if no whitespace-error lines are reported.

- [ ] **Step 2: Build all validation targets and ModelViewer sequentially**

```powershell
foreach ($target in @('ActorComponentValidation','SpatialComponentValidation','ResourceInstantiationValidation','RenderSceneValidation','SystemIntegrationTest','MaterialSystemValidation','RenderPassBindingValidation','ModelViewer')) {
    Write-Host "=== Building $target ==="
    cmake --build build\Debug --config Debug --target $target
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}
```

Expected result: every target builds.

- [ ] **Step 3: Run validation executables**

```powershell
$tests = @(
    '.\build\Debug\Tests\Debug\ActorComponentValidation.exe',
    '.\build\Debug\Tests\Debug\SpatialComponentValidation.exe',
    '.\build\Debug\Tests\Debug\ResourceInstantiationValidation.exe',
    '.\build\Debug\Tests\Debug\RenderSceneValidation.exe',
    '.\build\Debug\Tests\Debug\SystemIntegrationTest.exe',
    '.\build\Debug\Tests\Debug\MaterialSystemValidation.exe',
    '.\build\Debug\Tests\Debug\RenderPassBindingValidation.exe'
)
foreach ($test in $tests) {
    Write-Host "=== Running $test ==="
    & $test
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}
```

Expected result: every executable returns exit code 0 and reports all tests passing.

- [ ] **Step 4: Run ModelViewer smoke**

```powershell
$stdout = "build_codex\phase23_modelviewer_stdout.log"
$stderr = "build_codex\phase23_modelviewer_stderr.log"
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

- [ ] **Step 5: Request exactly one code review using Dalton**

Use this review context:

```text
Review Phase 23 of the UE-style Actor Component framework.

Requirements:
- ActorComponent exposes stable instance name storage through GetName() and SetName(std::string).
- Actor::AddComponent<T>() assigns an owner-unique default name before OnComponentCreated().
- Actor::AddOwnedComponent(...) preserves explicit names when unique and uniquifies collisions.
- Empty names default to GetClassName().
- Duplicate names receive _1, _2 suffixes.
- SceneEntity compatibility root SceneComponent receives the default component name.
- Prefab serialization and override formats remain unchanged in this phase.

Please report only Critical and Important issues, with file/line references.
```

- [ ] **Step 6: Fix any Critical or Important review findings**

If Dalton reports no Critical or Important findings, record that no fix was required and continue.

- [ ] **Step 7: Update every completed checkbox in this plan**

Mark every completed `- [ ]` as `- [x]` before committing.

- [ ] **Step 8: Commit the implementation**

```powershell
git add Docs\superpowers\plans\2026-05-15-ue-style-actor-component-phase23-component-instance-names.md Scene\Include\Scene\ActorComponent.h Scene\Include\Scene\Actor.h Scene\Private\Actor.cpp Tests\ActorComponentValidation\main.cpp
git commit -m "Name actor component instances"
```

---

## Self-Review

- Spec coverage: The plan adds component instance names and owner-unique assignment without changing prefab formats, setting up a later phase for stable duplicate component targeting.
- Placeholder scan: All code-changing steps include exact files and concrete code blocks; validation and commit commands are explicit.
- Type consistency: `ActorComponent`, `Actor::AddComponent`, `Actor::AddOwnedComponent`, `SceneEntity::GetRootComponent`, and `SceneComponent` match current code.
- Scope check: Runtime rename validation and prefab data migration are explicitly deferred to keep this phase small and safe.
