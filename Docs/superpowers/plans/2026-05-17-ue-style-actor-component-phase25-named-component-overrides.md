# UE-Style Actor Component Phase 25 Named Component Overrides Implementation Plan

**Goal:** Let prefab single-property component payload overrides target a specific actor component instance, so same-class duplicate components no longer fall back to class-order behavior for `RevertProperty(...)`, `IsOverridden(...)`, and `RemoveOverride(...)`.

**Architecture:** Append an optional `componentName` field to `PropertyOverride` after `value` to preserve existing `{componentType, propertyPath, value}` aggregate initializers. Existing class-only calls keep their behavior when `componentName` is empty. New overload/default-parameter APIs accept `componentName` and resolve actor component payload targets by live runtime component name, using `PrefabInstance` name bindings when the prefab-requested name was uniquified during instantiation.

**Testing Policy:** The user prefers not to add tests unless necessary. This phase changes duplicate-component targeting semantics, so add only focused tests covering the previously unsafe paths.

---

## Scope

- Append `PropertyOverride::componentName` after `value` for backward source compatibility.
- Make `PrefabInstance::AddOverride(...)` treat `(componentType, propertyPath, componentName)` as the override identity.
- Add optional `componentName` parameters to `RemoveOverride(...)`, `IsOverridden(...)`, and `RevertProperty(...)`.
- Keep class-only behavior unchanged when `componentName` is empty.
- For actor component `"serializedData"` named targets:
  - Interpret `componentName` as the live runtime component name.
  - Use `PrefabActorComponentNameBinding` to map runtime names back to prefab component data when `AddOwnedComponent(...)` had to uniquify names.
  - Restore only the targeted component payload.
  - Remove only the exact named override.
- Keep legacy component payload overrides class-only.
- Keep child entity targeting out of scope.

## Files

- Modify: `E:\WorkSpace\RenderVerseX\Scene\Include\Scene\Prefab.h`
- Modify: `E:\WorkSpace\RenderVerseX\Scene\Private\Prefab.cpp`
- Modify: `E:\WorkSpace\RenderVerseX\Tests\ResourceInstantiationValidation\main.cpp`
- Modify: `E:\WorkSpace\RenderVerseX\Docs\superpowers\plans\2026-05-17-ue-style-actor-component-phase25-named-component-overrides.md`

---

## Task 1: Add Minimal Failing Named Override Coverage

- [ ] **Step 1: Add named duplicate `RevertProperty` test**

In `Tests\ResourceInstantiationValidation\main.cpp`, add one focused test near the existing named duplicate `RevertAll` tests:

- Create prefab data with two `PrefabPayloadComponent` entries named `PrimaryPayload` and `SecondaryPayload`.
- Instantiate.
- Dirty both live payloads.
- Add two named overrides with `componentName` set to each live name.
- Call `RevertProperty("PrefabPayloadComponent", "serializedData", "SecondaryPayload")`.
- Assert only `SecondaryPayload` is restored.
- Assert `IsOverridden(..., "PrimaryPayload")` remains true and `IsOverridden(..., "SecondaryPayload")` is false.

- [ ] **Step 2: Add named duplicate `RemoveOverride` test**

Add one tiny API-level test:

- Create a `PrefabInstance`.
- Add two overrides with the same component class/property but different `componentName` values.
- Call `RemoveOverride("PrefabPayloadComponent", "serializedData", "SecondaryPayload")`.
- Assert only the secondary override is removed and the primary override remains.

- [ ] **Step 3: Add named `ApplyToPrefab` cleanup test**

Add one tiny persistence cleanup test:

- Create prefab data with two `PrefabPayloadComponent` entries named `PrimaryPayload` and `SecondaryPayload`.
- Instantiate.
- Add two named payload overrides.
- Keep only one live component payload dirty enough to be captured by `ApplyToPrefab()`.
- Call `ApplyToPrefab()`.
- Assert the override matching a captured root component class/name is cleared and an unrelated named override is preserved.

- [ ] **Step 4: Add uniquified runtime name `RevertProperty` test**

Reuse the existing `PrefabNameCollisionActor` fixture:

- Constructor component keeps `PrimaryPayload`.
- Prefab-created component is uniquified to `PrimaryPayload_1`.
- Dirty both payloads.
- Add a named override for `PrimaryPayload_1`.
- Call `RevertProperty("PrefabPayloadComponent", "serializedData", "PrimaryPayload_1")`.
- Assert only the prefab-created component is restored through the binding.

- [ ] **Step 5: Register the tests**

Register the tests close to the existing prefab revert property tests.

- [ ] **Step 6: Confirm RED**

Run:

```powershell
cmake --build build\Debug --config Debug --target ResourceInstantiationValidation
```

Expected before implementation: compile fails because `PropertyOverride` has no fourth `componentName` field and `RevertProperty`/`IsOverridden` do not accept a component name.

---

## Task 2: Implement Named Override Targeting

- [ ] **Step 1: Extend `PropertyOverride`**

Append:

```cpp
std::string componentName;
```

after `value`.

- [ ] **Step 2: Extend public prefab instance methods**

Update declarations and definitions:

```cpp
void RemoveOverride(const std::string& componentType,
                    const std::string& propertyPath,
                    const std::string& componentName = "");

bool IsOverridden(const std::string& componentType,
                  const std::string& propertyPath,
                  const std::string& componentName = "") const;

void RevertProperty(const std::string& componentType,
                    const std::string& propertyPath,
                    const std::string& componentName = "");
```

- [ ] **Step 3: Include component name in override identity**

`AddOverride(...)`, `RemoveOverride(...)`, and `IsOverridden(...)` must compare all three identity fields.

- [ ] **Step 4: Add named actor component payload helper**

Extend or add an internal helper for `"serializedData"` actor component payloads that:

- Accepts `componentType`, `componentName`, and `m_componentNameBindings`.
- If `componentName` is empty, preserves current class-only behavior.
- If `componentName` is non-empty, finds the matching prefab actor component record by runtime binding first, then by exact prefab name.
- Applies the prefab payload only to the live component with the requested runtime name.
- Returns false if no exact named target exists.

- [ ] **Step 5: Clear named component overrides on apply**

Update `RemoveSupportedRootComponentPayloadOverrides(...)` so `ApplyToPrefab()` clears named component payload overrides only when the captured root data contains the matching component class/name. Existing unnamed class-only clearing remains unchanged.

- [ ] **Step 6: Focused validation**

Run:

```powershell
cmake --build build\Debug --config Debug --target ResourceInstantiationValidation
.\build\Debug\Tests\Debug\ResourceInstantiationValidation.exe
```

Expected result: all tests pass.

---

## Task 3: Validate, Review, And Commit

- [ ] **Step 1: Run whitespace check**

```powershell
git diff --check
```

- [ ] **Step 2: Build required targets**

At minimum build:

```powershell
foreach ($target in @('ActorComponentValidation','ResourceInstantiationValidation','RenderSceneValidation','ModelViewer')) {
    cmake --build build\Debug --config Debug --target $target
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}
```

- [ ] **Step 3: Run validation executables**

Run:

```powershell
.\build\Debug\Tests\Debug\ActorComponentValidation.exe
.\build\Debug\Tests\Debug\ResourceInstantiationValidation.exe
.\build\Debug\Tests\Debug\RenderSceneValidation.exe
```

- [ ] **Step 4: Run ModelViewer smoke**

Run `ModelViewer.exe` with `C:\Users\yinzs\Desktop\DamagedHelmet.glb` and confirm the usual load/upload/render graph log lines with empty stderr.

- [ ] **Step 5: Request exactly one code review agent**

Ask Dalton to report only Critical and Important issues.

- [ ] **Step 6: Fix any Critical or Important review findings**

If there are none, continue.

- [ ] **Step 7: Update this plan checkboxes**

Mark completed items.

- [ ] **Step 8: Commit**

```powershell
git add Docs\superpowers\plans\2026-05-17-ue-style-actor-component-phase25-named-component-overrides.md Scene\Include\Scene\Prefab.h Scene\Private\Prefab.cpp Tests\ResourceInstantiationValidation\main.cpp
git commit -m "Target prefab component overrides by name"
```

---

## Self-Review

- Compatibility: Existing three-field `PropertyOverride` initializers still compile because `componentName` is appended.
- Scope: This phase only targets root actor component payload overrides; child entity addressing remains a later phase.
- Risk: The risky duplicate same-class behavior gets minimal direct coverage; broader validation relies on existing prefab and actor component tests.
