# UE-Style Actor Component Phase 24 Prefab Component Instance Names Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Use exactly one Dalton review for the plan and exactly one Dalton review for the implementation.

**Goal:** Persist actor component instance names in prefab data so duplicate same-class actor components can be reconstructed and payloads can be matched by stable component identity.

**Architecture:** Extend `PrefabActorComponentData` with an optional `name` field appended after `serializedData` to preserve existing two-field aggregate initializers. Captured prefab actor components store `ActorComponent::GetName()`. Instantiation sets the requested component name before `Actor::AddOwnedComponent(...)`, then records a transient binding from `(className, prefabName, ordinal)` to the final owner-unique runtime component name returned by the actor. Payload restoration uses those bindings first, exact `(className, name)` matching second, and current class-order matching for old prefab data with empty names.

**Tech Stack:** C++20, RenderVerseX Scene module, standalone `ResourceInstantiationValidation`.

---

## Scope

- Add optional actor component instance name storage to `PrefabActorComponentData`.
- Keep existing source compatibility for `{className, serializedData}` aggregate initializers.
- `Prefab::CreateFromEntity(...)` and `Prefab::UpdateRootEntityStateFrom(...)` capture actor component names.
- `Prefab::Instantiate(...)` restores actor component names before ownership/lifecycle registration.
- `PrefabInstance` records transient prefab component name bindings when instantiated components are uniquified by the owning actor.
- `Prefab::GetMemoryUsage()` accounts for actor component names.
- `PrefabInstance::RevertAll()` restores actor component payloads by the final runtime name when a binding exists, or by `(className, name)` when names are available.
- `PrefabInstance::RevertProperty(componentType, "serializedData")` continues to support class-only targets and does not introduce a new override target format in this phase.
- Existing unnamed prefab data keeps current class-order behavior.

## Files

- Modify: `E:\WorkSpace\RenderVerseX\Scene\Include\Scene\Prefab.h`
  - Append actor component `name` field to `PrefabActorComponentData`.
  - Add a transient component name binding container to `PrefabInstance`.
- Modify: `E:\WorkSpace\RenderVerseX\Scene\Private\Prefab.cpp`
  - Capture, instantiate, memory-count, and match named actor component data.
- Modify: `E:\WorkSpace\RenderVerseX\Tests\ResourceInstantiationValidation\main.cpp`
  - Add coverage for serialization, instantiation, and named payload matching.
- Modify: `E:\WorkSpace\RenderVerseX\Docs\superpowers\plans\2026-05-15-ue-style-actor-component-phase24-prefab-component-instance-names.md`
  - Track completion status.

---

## Task 1: Add Failing Prefab Component Name Tests

**Files:**
- Modify: `E:\WorkSpace\RenderVerseX\Tests\ResourceInstantiationValidation\main.cpp`

- [x] **Step 1: Add prefab serialization test for actor component names**

Add a test near `Test_PrefabSerializesActorComponentPayloads` that creates a `SceneEntity`, adds a `PrefabPayloadComponent`, sets its name to `PrimaryPayload`, creates a prefab, and asserts:

```cpp
TEST_ASSERT_EQ(std::string("PrimaryPayload"), data->actorComponentData[0].name);
```

- [x] **Step 2: Add prefab instantiation test for actor component names**

Add a test near `Test_PrefabInstantiatesActorComponentPayloads` using:

```cpp
PrefabEntityData data;
data.name = "NamedComponentPrefabRoot";
data.actorComponentData.push_back({"PrefabPayloadComponent", "ammo=12", "PrimaryPayload"});
```

Register `PrefabPayloadComponent`, instantiate, and assert the live component name is `PrimaryPayload`.

- [x] **Step 3: Add named duplicate payload restore test**

Add a test near `Test_PrefabInstanceRevertAllRestoresActorComponentPayload` with two same-class actor component records:

```cpp
data.actorComponentData.push_back({"PrefabPayloadComponent", "slot=primary", "PrimaryPayload"});
data.actorComponentData.push_back({"PrefabPayloadComponent", "slot=secondary", "SecondaryPayload"});
```

After instantiation, temporarily swap the live component names and payloads, call `RevertAll()`, and assert the payloads were restored to the components matching the prefab names, not merely by class order.

- [x] **Step 4: Add uniquified runtime name binding test**

Add a test with a registered actor class that creates an existing `PrefabPayloadComponent` named `PrimaryPayload` in its constructor. Instantiate prefab data whose actor class is that class and whose prefab component record also requests `PrimaryPayload`.

Assert:

- The actor-owned constructor component keeps `PrimaryPayload`.
- The prefab-created component is uniquified to `PrimaryPayload_1`.
- `PrefabInstance::RevertAll()` restores the prefab-created component payload through the binding instead of looking only for the original prefab name.

- [x] **Step 5: Document class-only `RevertProperty` limitation in tests**

Add a focused assertion or test comment near the named duplicate tests that this phase intentionally keeps `RevertProperty(componentType, "serializedData")` class-only. Named single-property override targets are a later phase.

- [x] **Step 6: Register the new tests**

Register the new tests in `main()` close to the related prefab serialization/instantiation/revert tests.

- [x] **Step 7: Build and confirm RED**

Run:

```powershell
cmake --build build\Debug --config Debug --target ResourceInstantiationValidation
```

Expected before implementation: the target fails to compile because `PrefabActorComponentData` has no `name` field, or the new tests fail because names are not captured/restored.

---

## Task 2: Persist Actor Component Names In Prefabs

**Files:**
- Modify: `E:\WorkSpace\RenderVerseX\Scene\Include\Scene\Prefab.h`
- Modify: `E:\WorkSpace\RenderVerseX\Scene\Private\Prefab.cpp`

- [x] **Step 1: Extend `PrefabActorComponentData`**

Append this field after `serializedData`:

```cpp
std::string name;
```

Do not insert it before `serializedData`; existing two-field aggregate initializers must keep working.

- [x] **Step 2: Capture actor component names**

In `CapturePrefabEntityComponents(...)`, change the actor component push to store:

```cpp
{className, actorComponent->SerializePrefabData(), actorComponent->GetName()}
```

- [x] **Step 3: Restore actor component names on instantiate**

In `Prefab::CreateComponents(...)`, before `component->DeserializePrefabData(...)`, set the requested name when non-empty:

```cpp
if (!componentData.name.empty())
{
    component->SetName(componentData.name);
}
```

Keep `Actor::AddOwnedComponent(...)` responsible for final owner-unique collision handling.

- [x] **Step 4: Record final runtime component name bindings**

Add a small transient binding structure to `PrefabInstance` and an internal setter used by `Prefab::InstantiateInternal(...)` after the root `PrefabInstance` component is created.

Each binding should include:

- Actor component class name.
- Prefab requested name.
- Actor component ordinal within that class in prefab data.
- Final runtime name after `Actor::AddOwnedComponent(...)`.

Update `Prefab::CreateComponents(...)` to optionally output bindings for actor components it creates.

- [x] **Step 5: Account for name memory**

In `Prefab::GetMemoryUsage()`, include `component.name.capacity()` for every actor component data entry.

- [x] **Step 6: Match actor component payloads by binding or name when available**

Add helpers in `Prefab.cpp` to find pure actor components by `(className, name)`. Update `ApplyPrefabEntityComponentPayloads(...)` to:

- Use the `PrefabInstance` binding final runtime name when one exists for the prefab component record.
- Use exact name matching when `componentData.name` is non-empty and no binding exists.
- Use existing class-order matching when `componentData.name` is empty.
- Skip missing named components instead of applying named payloads to the wrong component.

Keep `ApplyPrefabEntityComponentPayload(...)` class-only for this phase.

- [x] **Step 7: Build and run focused validation**

Run:

```powershell
cmake --build build\Debug --config Debug --target ResourceInstantiationValidation
.\build\Debug\Tests\Debug\ResourceInstantiationValidation.exe
```

Expected result: all `ResourceInstantiationValidation` tests pass, including the three new component-name prefab tests.

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
$stdout = "build_codex\phase24_modelviewer_stdout.log"
$stderr = "build_codex\phase24_modelviewer_stderr.log"
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

- [x] **Step 5: Request exactly one Dalton code review**

Ask Dalton to review only Critical and Important issues for this phase.

- [x] **Step 6: Fix any Critical or Important review findings**

If Dalton reports no Critical or Important findings, record that no fix was required and continue.

- [x] **Step 7: Update every completed checkbox in this plan**

Mark every completed `- [ ]` as `- [x]` before committing.

- [x] **Step 8: Commit the implementation**

```powershell
git add Docs\superpowers\plans\2026-05-15-ue-style-actor-component-phase24-prefab-component-instance-names.md Scene\Include\Scene\Prefab.h Scene\Private\Prefab.cpp Tests\ResourceInstantiationValidation\main.cpp
git commit -m "Persist prefab actor component names"
```

---

## Self-Review

- Spec coverage: The plan persists the stable component names introduced in Phase 23 and applies them to prefab capture, instantiation, and payload restoration.
- Compatibility: Appending `name` after `serializedData` preserves current two-field aggregate initializers and keeps old unnamed prefab data on the class-order fallback path.
- Scope check: This phase does not change the public `PropertyOverride` target format; named single-property override targeting is left for a later, focused phase and documented by tests.
- Review response: Dalton's plan review flagged name uniquification as an Important risk. The revised plan records transient prefab-to-runtime component name bindings, so `RevertAll()` can find the final owner-unique component name even when `AddOwnedComponent(...)` had to add a suffix.
- Risk control: Existing unnamed prefabs keep class-order matching. Named prefabs avoid applying payloads to the wrong same-class component when names or instantiation bindings are available.
