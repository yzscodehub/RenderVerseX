# UE-Style Actor Component Phase 20 Legacy Component Mutation Safety Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make legacy `SceneEntity::m_components` tick dispatch safe when legacy `Component` instances add or remove legacy components during `Tick()`.

**Architecture:** Mirror the already-hardened `Actor` component dispatch pattern for the legacy compatibility container: maintain insertion order for deterministic ticking, take a raw-pointer snapshot before ticking, defer removals requested during legacy component dispatch, skip entries already queued for removal, and flush removals after the outermost dispatch returns. Keep legacy components out of `Actor::m_components` so they do not double tick or enter UE-style `BeginPlay()`/registration lifecycle by accident.

**Tech Stack:** C++20, RenderVerseX Scene module, standalone `ActorComponentValidation`.

---

## Scope

- Harden `SceneEntity::TickComponents()` against legacy component add/remove mutation during tick.
- Preserve deterministic insertion-order ticking for legacy components instead of relying on `unordered_map` traversal order.
- Preserve immediate `SceneEntity::AddComponent<T>()` behavior for legacy components.
- Preserve immediate `SceneEntity::RemoveComponent<T>()` behavior outside legacy component dispatch.
- Keep legacy `Component` ownership in `SceneEntity::m_components`; do not move legacy components into `Actor::m_components`.
- Do not add legacy `BeginPlay()`, registration, reflection, serialization, or prefab override behavior in this phase.

## Files

- Modify: `E:\WorkSpace\RenderVerseX\Tests\ActorComponentValidation\main.cpp`
  - Add failing tests for add-during-tick, self-remove-during-tick, and remove-other-before-turn on legacy `Component`.
- Modify: `E:\WorkSpace\RenderVerseX\Scene\Include\Scene\SceneEntity.h`
  - Add private insertion-order and dispatch helpers/state for legacy component snapshots and deferred removals.
  - Route legacy `RemoveComponent<T>()` through the deferred path while dispatching.
- Modify: `E:\WorkSpace\RenderVerseX\Scene\Private\SceneEntity.cpp`
  - Implement snapshot dispatch helpers and update `TickComponents()`.
- Modify: `E:\WorkSpace\RenderVerseX\Docs\superpowers\plans\2026-05-15-ue-style-actor-component-phase20-legacy-component-mutation-safety.md`
  - Track completion status.

---

## Task 1: Add Failing Legacy Mutation Tests

**Files:**
- Modify: `E:\WorkSpace\RenderVerseX\Tests\ActorComponentValidation\main.cpp`

- [x] **Step 1: Add legacy mutation helper components**

Add these helper classes inside the anonymous namespace after `SceneManagedLegacyTickComponent`:

```cpp
    class AddLegacyDuringTickComponent : public RVX::Component
    {
    public:
        explicit AddLegacyDuringTickComponent(int* addedTickCount)
            : m_addedTickCount(addedTickCount)
        {
        }

        const char* GetTypeName() const override { return "AddLegacyDuringTickComponent"; }

        void Tick(float) override
        {
            if (!m_added && GetOwner())
            {
                GetOwner()->AddComponent<SceneManagedLegacyTickComponent>(m_addedTickCount);
                m_added = true;
            }
        }

    private:
        int* m_addedTickCount = nullptr;
        bool m_added = false;
    };

    class SelfRemovingLegacyTickComponent : public RVX::Component
    {
    public:
        explicit SelfRemovingLegacyTickComponent(int* tickCount)
            : m_tickCount(tickCount)
        {
        }

        const char* GetTypeName() const override { return "SelfRemovingLegacyTickComponent"; }

        void Tick(float) override
        {
            if (m_tickCount)
            {
                ++(*m_tickCount);
            }
            if (GetOwner())
            {
                GetOwner()->RemoveComponent<SelfRemovingLegacyTickComponent>();
            }
        }

    private:
        int* m_tickCount = nullptr;
    };

    class VictimLegacyTickComponent : public RVX::Component
    {
    public:
        explicit VictimLegacyTickComponent(int* tickCount)
            : m_tickCount(tickCount)
        {
        }

        const char* GetTypeName() const override { return "VictimLegacyTickComponent"; }

        void Tick(float) override
        {
            if (m_tickCount)
            {
                ++(*m_tickCount);
            }
        }

    private:
        int* m_tickCount = nullptr;
    };

    class RemoveVictimLegacyDuringTickComponent : public RVX::Component
    {
    public:
        const char* GetTypeName() const override { return "RemoveVictimLegacyDuringTickComponent"; }

        void Tick(float) override
        {
            if (GetOwner())
            {
                GetOwner()->RemoveComponent<VictimLegacyTickComponent>();
            }
        }
    };
```

- [x] **Step 2: Add add-during-tick test**

Add this test after `Test_SceneManagerUpdateKeepsLegacyComponentTicking`:

```cpp
    bool Test_SceneEntityLegacyTickSnapshotsComponentsAddedDuringTick()
    {
        RVX::SceneEntity entity("LegacyAddDuringTickEntity");
        int addedTickCount = 0;

        auto* spawner = entity.AddComponent<AddLegacyDuringTickComponent>(&addedTickCount);
        TEST_ASSERT_NOT_NULL(spawner);

        entity.TickComponents(0.016f);
        TEST_ASSERT_EQ(0, addedTickCount);
        TEST_ASSERT_EQ(static_cast<size_t>(2), entity.GetComponentCount());

        entity.TickComponents(0.016f);
        TEST_ASSERT_EQ(1, addedTickCount);
        return true;
    }
```

- [x] **Step 3: Add self-remove-during-tick test**

Add this test after the add-during-tick test:

```cpp
    bool Test_SceneEntityLegacyDefersSelfRemovalDuringTick()
    {
        RVX::SceneEntity entity("LegacySelfRemoveEntity");
        int tickCount = 0;

        auto* component = entity.AddComponent<SelfRemovingLegacyTickComponent>(&tickCount);
        TEST_ASSERT_NOT_NULL(component);

        entity.TickComponents(0.016f);

        TEST_ASSERT_EQ(1, tickCount);
        TEST_ASSERT_EQ(static_cast<size_t>(0), entity.GetComponentCount());

        entity.TickComponents(0.016f);
        TEST_ASSERT_EQ(1, tickCount);
        return true;
    }
```

- [x] **Step 4: Add remove-other-before-turn test**

Add this test after the self-remove test:

```cpp
    bool Test_SceneEntityLegacySkipsComponentRemovedEarlierInTick()
    {
        RVX::SceneEntity entity("LegacyRemoveOtherEntity");
        int victimTickCount = 0;

        auto* remover = entity.AddComponent<RemoveVictimLegacyDuringTickComponent>();
        TEST_ASSERT_NOT_NULL(remover);
        auto* victim = entity.AddComponent<VictimLegacyTickComponent>(&victimTickCount);
        TEST_ASSERT_NOT_NULL(victim);

        entity.TickComponents(0.016f);

        TEST_ASSERT_EQ(0, victimTickCount);
        TEST_ASSERT_EQ(nullptr, entity.GetComponent<VictimLegacyTickComponent>());
        return true;
    }
```

- [x] **Step 5: Register the three tests**

Register these tests in `main()` immediately after `SceneManagerUpdateKeepsLegacyComponentTicking`:

```cpp
    suite.AddTest("SceneEntityLegacyTickSnapshotsComponentsAddedDuringTick",
                  Test_SceneEntityLegacyTickSnapshotsComponentsAddedDuringTick);
    suite.AddTest("SceneEntityLegacyDefersSelfRemovalDuringTick",
                  Test_SceneEntityLegacyDefersSelfRemovalDuringTick);
    suite.AddTest("SceneEntityLegacySkipsComponentRemovedEarlierInTick",
                  Test_SceneEntityLegacySkipsComponentRemovedEarlierInTick);
```

- [x] **Step 6: Build and confirm RED**

Run:

```powershell
cmake --build build\Debug --config Debug --target ActorComponentValidation
.\build\Debug\Tests\Debug\ActorComponentValidation.exe
```

Expected before implementation: at least one of the three new legacy mutation tests fails or crashes because `SceneEntity::TickComponents()` currently iterates `m_components` directly while `RemoveComponent<T>()` erases from the same `unordered_map`.

---

## Task 2: Add Legacy Component Snapshot Dispatch

**Files:**
- Modify: `E:\WorkSpace\RenderVerseX\Scene\Include\Scene\SceneEntity.h`
- Modify: `E:\WorkSpace\RenderVerseX\Scene\Private\SceneEntity.cpp`

- [x] **Step 1: Add private helper declarations and state**

In `SceneEntity.h`, add these private helper declarations near `MarkChildrenTransformDirty()`:

```cpp
        bool RemoveLegacyComponentByType(std::type_index typeIndex);
        bool IsLegacyComponentRemovalPending(std::type_index typeIndex) const;
        void QueuePendingLegacyComponentRemoval(std::type_index typeIndex);
        void FlushPendingLegacyComponentRemovals();
        std::vector<Component*> MakeLegacyComponentSnapshot() const;
        void BeginLegacyComponentDispatch();
        void EndLegacyComponentDispatch();
```

Add these private members near `m_components`:

```cpp
        std::vector<std::type_index> m_legacyComponentOrder;
        int32 m_legacyComponentDispatchDepth = 0;
        std::vector<std::type_index> m_pendingRemoveLegacyComponents;
```

- [x] **Step 2: Update legacy `AddComponent<T>()` and `RemoveComponent<T>()`**

In `SceneEntity::AddComponent<T>()`, record insertion order before storing a newly-created legacy component:

```cpp
            m_legacyComponentOrder.push_back(typeIndex);
            m_components[typeIndex] = std::move(component);
```

Replace the legacy branch in `SceneEntity::RemoveComponent<T>()` with this code:

```cpp
        if constexpr (std::is_base_of_v<Component, T>)
        {
            std::type_index typeIndex(typeid(T));
            if (m_components.find(typeIndex) == m_components.end())
            {
                return false;
            }

            if (m_legacyComponentDispatchDepth > 0)
            {
                QueuePendingLegacyComponentRemoval(typeIndex);
                return IsLegacyComponentRemovalPending(typeIndex);
            }

            return RemoveLegacyComponentByType(typeIndex);
        }
```

- [x] **Step 3: Clear pending legacy removals during destruction**

In `SceneEntity::~SceneEntity()`, before iterating `m_components`, add:

```cpp
    m_pendingRemoveLegacyComponents.clear();
    m_legacyComponentDispatchDepth = 0;
```

- [x] **Step 4: Implement legacy dispatch helpers**

Add these implementations in `SceneEntity.cpp` near the `Components` section before `TickComponents(...)`:

```cpp
bool SceneEntity::RemoveLegacyComponentByType(std::type_index typeIndex)
{
    auto it = m_components.find(typeIndex);
    if (it == m_components.end())
        return false;

    std::unique_ptr<Component> removedComponent = std::move(it->second);
    m_components.erase(it);

    if (removedComponent)
    {
        removedComponent->OnDetach();
        removedComponent->OnComponentDestroyed();
        removedComponent->SetOwner(nullptr);
    }

    MarkBoundsDirty();
    m_legacyComponentOrder.erase(
        std::remove(m_legacyComponentOrder.begin(), m_legacyComponentOrder.end(), typeIndex),
        m_legacyComponentOrder.end());
    return true;
}

bool SceneEntity::IsLegacyComponentRemovalPending(std::type_index typeIndex) const
{
    return std::find(m_pendingRemoveLegacyComponents.begin(),
                     m_pendingRemoveLegacyComponents.end(),
                     typeIndex) != m_pendingRemoveLegacyComponents.end();
}

void SceneEntity::QueuePendingLegacyComponentRemoval(std::type_index typeIndex)
{
    if (m_components.find(typeIndex) == m_components.end())
        return;

    if (IsLegacyComponentRemovalPending(typeIndex))
        return;

    m_pendingRemoveLegacyComponents.push_back(typeIndex);
}

void SceneEntity::FlushPendingLegacyComponentRemovals()
{
    if (m_pendingRemoveLegacyComponents.empty())
        return;

    auto pending = std::move(m_pendingRemoveLegacyComponents);
    m_pendingRemoveLegacyComponents.clear();

    for (std::type_index typeIndex : pending)
    {
        RemoveLegacyComponentByType(typeIndex);
    }
}

std::vector<Component*> SceneEntity::MakeLegacyComponentSnapshot() const
{
    std::vector<Component*> snapshot;
    snapshot.reserve(m_components.size());
    for (std::type_index typeIndex : m_legacyComponentOrder)
    {
        auto it = m_components.find(typeIndex);
        if (it != m_components.end() && it->second)
        {
            snapshot.push_back(it->second.get());
        }
    }
    return snapshot;
}

void SceneEntity::BeginLegacyComponentDispatch()
{
    ++m_legacyComponentDispatchDepth;
}

void SceneEntity::EndLegacyComponentDispatch()
{
    if (m_legacyComponentDispatchDepth <= 0)
        return;

    --m_legacyComponentDispatchDepth;
    if (m_legacyComponentDispatchDepth == 0)
    {
        FlushPendingLegacyComponentRemovals();
    }
}
```

- [x] **Step 5: Update `TickComponents(...)` to use snapshot dispatch**

Replace `SceneEntity::TickComponents(float deltaTime)` with:

```cpp
void SceneEntity::TickComponents(float deltaTime)
{
    auto snapshot = MakeLegacyComponentSnapshot();
    BeginLegacyComponentDispatch();
    for (Component* component : snapshot)
    {
        if (!component || IsLegacyComponentRemovalPending(std::type_index(typeid(*component))))
            continue;

        if (component->IsEnabled())
        {
            component->Tick(deltaTime);
        }
    }
    EndLegacyComponentDispatch();
}
```

- [x] **Step 6: Build and run focused validation**

Run:

```powershell
cmake --build build\Debug --config Debug --target ActorComponentValidation
.\build\Debug\Tests\Debug\ActorComponentValidation.exe
```

Expected result: all `ActorComponentValidation` tests pass, including the three new legacy mutation tests.

---

## Task 3: Validate, Review, And Commit

**Files:**
- No additional source files.

- [x] **Step 1: Run whitespace check**

```powershell
git diff --check
```

Expected result: exit code 0. Line-ending warnings are acceptable if no whitespace-error lines are reported.

- [x] **Step 2: Build all validation targets and ModelViewer sequentially**

```powershell
foreach ($target in @('ActorComponentValidation','SpatialComponentValidation','ResourceInstantiationValidation','RenderSceneValidation','SystemIntegrationTest','MaterialSystemValidation','RenderPassBindingValidation','ModelViewer')) {
    Write-Host "=== Building $target ==="
    cmake --build build\Debug --config Debug --target $target
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}
```

Expected result: every target builds.

- [x] **Step 3: Run validation executables**

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

- [x] **Step 4: Run ModelViewer smoke**

```powershell
$stdout = "build_codex\phase20_modelviewer_stdout.log"
$stderr = "build_codex\phase20_modelviewer_stderr.log"
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

- [x] **Step 5: Request exactly one code review using Dalton**

Use this review context:

```text
Review Phase 20 of the UE-style Actor Component framework.

Requirements:
- SceneEntity legacy Component Tick dispatch uses a snapshot.
- Removing a legacy Component during Tick is deferred until legacy dispatch completes.
- Legacy components queued for removal are skipped later in the same Tick dispatch.
- Adding a legacy Component during Tick does not tick it until the next TickComponents call.
- Immediate add/remove outside TickComponents remains unchanged.
- Legacy components remain outside Actor::m_components and do not enter Actor BeginPlay/Register lifecycle.

Please report only Critical and Important issues, with file/line references.
```

- [x] **Step 6: Fix any Critical or Important review findings**

If Dalton reports no Critical or Important findings, record that no fix was required and continue.

- [x] **Step 7: Update every completed checkbox in this plan**

Mark every completed `- [ ]` as `- [x]` before committing.

- [x] **Step 8: Commit the implementation**

```powershell
git add Docs\superpowers\plans\2026-05-15-ue-style-actor-component-phase20-legacy-component-mutation-safety.md Scene\Include\Scene\SceneEntity.h Scene\Private\SceneEntity.cpp Tests\ActorComponentValidation\main.cpp
git commit -m "Harden legacy component mutation during tick"
```

---

## Self-Review

- Spec coverage: This plan closes the explicit Phase 7 compatibility gap where legacy `SceneEntity::m_components` mutation remained unsafe while `Actor::m_components` was already snapshot-based.
- Placeholder scan: All code-changing steps name exact files and include concrete code blocks; all validation steps include exact commands and expected results.
- Type consistency: `std::type_index`, `Component*`, `SceneEntity::m_components`, `SceneEntity::m_legacyComponentOrder`, `SceneEntity::TickComponents`, and `SceneEntity::RemoveComponent<T>()` match current codebase types and ownership model.
- Risk check: The plan intentionally keeps legacy components out of `Actor::m_components`, avoiding double ticking and accidental UE-style lifecycle callbacks while still making the compatibility tick path safe.
