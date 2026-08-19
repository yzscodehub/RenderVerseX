# Engine Render Composition Initialization Order Fix

**Status:** Local implementation complete; candidate gates pending
**Branch:** `codex/architecture-implementation`
**Predecessor candidate:** `cf65f9c5`
**Scope:** Core subsystem scheduling and Engine-owned Render composition only

## Goal

Fix the `ModelViewer` startup regression without making `RenderSubsystem`
depend on `Runtime/Window`, without manually initializing subsystems outside
the collection, and without weakening the existing fail-closed Render
composition checks.

The completed design must provide:

- explicit, type-safe composition dependencies;
- deterministic dependency ordering independent of hash iteration order;
- dependency-first initialization and reverse-order shutdown;
- cycle, missing-node, duplicate, and active-lifecycle diagnostics;
- unchanged standalone `RenderSubsystem` configuration;
- a successful bounded DX12 `ModelViewer` run and Fresh Build Truth.

## Observed Failure

Directly starting the current Debug `ModelViewer.exe` resolves the default
model successfully and then exits with code `-1`:

```text
Model path: C:\Users\yinzs\Desktop\DamagedHelmet.glb
Initializing subsystem: RenderSubsystem
Subsystem 'RenderSubsystem' failed to initialize:
WindowSubsystem must be initialized before RenderSubsystem
```

`ModelViewer` registers `WindowSubsystem` before `RenderSubsystem`, but
`SubsystemCollection::BuildOrder()` seeds its zero-indegree queue by iterating
an `unordered_map`. Because Engine-owned Render composition requirements are
not represented in the dependency graph, Render may be initialized first.

## Approved Design

### Intrinsic and composition dependencies remain distinct

Dependencies that are always true for a subsystem remain declared through
`ISubsystem::GetTypedDependencies()`. Dependencies that exist only in a
specific composition are registered by the composition root.

`RenderSubsystem` therefore does **not** declare `WindowSubsystem` or
`ResourceSubsystem` as intrinsic dependencies. It remains usable by focused
tests and non-Engine hosts that configure it from an owned
`NativeSurfaceDesc`.

Engine registers these composition edges:

```text
WindowSubsystem   --> RenderSubsystem
ResourceSubsystem --> RenderSubsystem
```

### Dependency registration is typed and diagnostic

Add a Core-owned result contract:

```cpp
enum class SubsystemDependencyRegistrationCode : uint8
{
    Added = 0,
    AlreadyRegistered,
    MissingDependent,
    MissingPrerequisite,
    SelfDependency,
    LifecycleActive
};

struct SubsystemDependencyRegistrationResult
{
    SubsystemDependencyRegistrationCode code =
        SubsystemDependencyRegistrationCode::MissingDependent;

    [[nodiscard]] bool IsAccepted() const noexcept
    {
        return code == SubsystemDependencyRegistrationCode::Added ||
               code ==
                   SubsystemDependencyRegistrationCode::AlreadyRegistered;
    }
};
```

Expose the type-safe API on `SubsystemCollection<TBase>`:

```cpp
template <typename TDependent, typename TPrerequisite>
[[nodiscard]] SubsystemDependencyRegistrationResult
AddInitializationDependency();
```

The public API must not accept subsystem names or raw pointers.

### One graph owns all ordering decisions

Intrinsic typed dependencies, legacy string dependencies, and composition
dependencies must feed one internal graph representation. Validation, cycle
detection, initialization order, rollback, and shutdown must not rebuild
different interpretations of the dependency graph.

For nodes that are simultaneously ready, registration ordinal is the stable
tie-breaker. Registration order never overrides an explicit dependency.

### Lifecycle behavior is fail-closed

- Missing dependent or prerequisite types reject registration.
- Self-dependencies reject registration.
- Duplicate edges are idempotent.
- Dependency mutation is rejected while any subsystem is initialized.
- Cycles fail before any subsystem is initialized.
- Initialization failure unwinds only nodes initialized by that call.
- Normal shutdown and failure unwind use reverse realized initialization order.
- `Clear()` removes composition edges.

The existing check in
`RenderRuntimeComposition::BeforeRenderSubsystemInitialize()` remains a
runtime invariant. The dependency graph prevents the normal ordering error;
the runtime check catches future composition regressions.

## Non-Goals

- Do not add a `Runtime/Window` include or link to the Render module.
- Do not put window ownership or a live window pointer in `RenderSubsystem`.
- Do not initialize `WindowSubsystem` manually from the Render hook.
- Do not use registration order as the only correctness mechanism.
- Do not add a general plugin scheduler, phase framework, or dynamic subsystem
  unloading design in this repair.
- Do not change RHI, backend, Render Thread, frame packet, or resource gateway
  contracts.
- Do not add another permanent GPU smoke that duplicates the existing
  `ModelViewer` and native lifecycle gates.

## Expected File Scope

- Add:
  `Docs/superpowers/plans/2026-07-26-engine-render-composition-order-fix.md`
- Modify: `Core/Include/Core/Subsystem/SubsystemCollection.h`
- Modify: `Engine/Private/Engine.cpp`
- Modify: `Tests/AppModeBoundaryValidation/main.cpp`
- Modify: `Tests/EngineRenderCompositionValidation/main.cpp`
- Modify: `Tests/RHIContractValidation/main.cpp`
- Modify: `Scripts/run_architecture_baseline.ps1`
- Modify after final evidence only:
  `Docs/superpowers/specs/phase-log.md`

No CMake target registration should be required because all affected test
executables already exist.

## Implementation Steps

### Task 1: Add failing Core lifecycle regressions

- [ ] Add no-dependency recording subsystem fixtures to
  `Tests/AppModeBoundaryValidation/main.cpp`.
- [ ] Add
  `CompositionDependenciesOverrideRegistrationOrderAndReverseShutdown`.
  Register the dependent first, register the prerequisite second, add the
  composition edge, and require:

  ```text
  prerequisite:init
  dependent:init
  dependent:shutdown
  prerequisite:shutdown
  ```

- [ ] Add
  `CompositionDependencyRegistrationIsTypedIdempotentAndFailClosed`.
  Cover missing dependent, missing prerequisite, self-dependency, first add,
  duplicate add, and mutation while active.
- [ ] Add `CompositionDependencyCyclesFailBeforeInitialization`. Construct a
  two-node cycle entirely from composition edges and require zero lifecycle
  callbacks.
- [ ] Add `UnrelatedSubsystemInitializationOrderIsDeterministic`. Repeat the
  same registration sequence enough times to prove identical order without
  dependencies.

Run:

```powershell
cmake --build --preset win_x64_debug `
  --target AppModeBoundaryValidation -- /m:1
```

Expected red result: compilation fails because the typed composition
dependency API and result contract do not exist.

### Task 2: Implement one deterministic dependency graph

- [ ] Add the registration result contract in
  `SubsystemCollection.h`.
- [ ] Store composition edges by `std::type_index`.
- [ ] Reject dependency mutation when the collection or any owned subsystem is
  initialized.
- [ ] Make duplicate registration idempotent.
- [ ] Centralize graph construction so intrinsic typed, legacy string, and
  composition edges are deduplicated into the same adjacency/indegree model.
- [ ] Preserve optional typed dependency behavior.
- [ ] Produce missing dependency and cycle diagnostics before initialization.
- [ ] Replace `unordered_map` traversal as the zero-indegree ordering source.
  Select ready nodes using registration ordinal as the deterministic
  tie-breaker.
- [ ] Keep `m_ordered` as the deterministic planned order used by ticking and
  normal reverse shutdown.
- [ ] Keep a call-local realized initialization prefix for failure unwind, so
  rollback never touches a node that did not finish initialization.
- [ ] Clear composition dependencies in `Clear()`.

Run:

```powershell
cmake --build --preset win_x64_debug `
  --target AppModeBoundaryValidation -- /m:1
ctest --test-dir build\win_x64_debug -C Debug `
  -R "^AppModeBoundaryValidation\.(Composition|UnrelatedSubsystemInitializationOrder)" `
  --output-on-failure
```

Expected green result: every new dependency and determinism regression passes,
and the existing initialization hook, rollback, targeted shutdown, and
dependency tests remain green.

### Task 3: Register Engine-owned Render prerequisites

- [ ] In `Engine::InitializeSubsystems()`, keep the existing required
  `ResourceSubsystem` and `WindowSubsystem` presence check.
- [ ] Before creating or preparing `RenderRuntimeComposition`, register:

  ```cpp
  AddInitializationDependency<RenderSubsystem, WindowSubsystem>();
  AddInitializationDependency<
      RenderSubsystem,
      Resource::ResourceSubsystem>();
  ```

- [ ] Reject Engine initialization with an owned diagnostic if either
  registration result is not accepted.
- [ ] Keep dependency registration idempotent so a fully unwound failed
  initialization can be retried.
- [ ] Do not move surface capture out of the staged pre-Render hook.
- [ ] Do not remove the `IsWindowInitialized()` fail-closed check.

Add or revise structural regressions:

- [ ] `EngineRenderCompositionValidation` requires both typed composition
  edges to appear before `InitializeAll()`.
- [ ] `RHIContractValidation.StandardEngineInjectsWindowBeforeRenderInitialization`
  requires Engine-owned edge registration and continues to reject a
  `SetWindowSubsystem` back-reference.
- [ ] Require `RenderSubsystem.h` to remain free of
  `Runtime/Window/WindowSubsystem.h`, `WindowSubsystem*`, and a typed
  `WindowSubsystem` dependency declaration.

Run:

```powershell
cmake --build --preset win_x64_debug `
  --target EngineRenderCompositionValidation `
           RHIContractValidation `
           AppModeBoundaryValidation -- /m:1

ctest --test-dir build\win_x64_debug -C Debug `
  -R "^(EngineRenderCompositionValidation|RHIContractValidation|AppModeBoundaryValidation)\." `
  --output-on-failure
```

Expected: all focused lifecycle, composition, and boundary tests pass.

### Task 4: Promote composition regressions into the architecture baseline

- [ ] Add `EngineRenderCompositionValidation\.` to
  `Scripts/run_architecture_baseline.ps1`.
- [ ] Keep `AppModeBoundaryValidation\.`,
  `RHIContractValidation\.`, and `Architecture.M1ArchitectureCut`.
- [ ] Do not add a second invocation of the M1 checker.

Run:

```powershell
pwsh -NoProfile -ExecutionPolicy Bypass `
  -File Scripts\run_architecture_baseline.ps1 `
  -BuildDir build\win_x64_debug `
  -Configuration Debug `
  -ReportPath build\win_x64_debug\BuildTruth\CompositionOrderRepair\ArchitectureBaseline.json
```

Expected: the expanded architecture baseline passes and its report includes
the Engine composition suite.

### Task 5: Validate the real regression through ModelViewer

Build:

```powershell
cmake --build --preset win_x64_debug --target ModelViewer -- /m:1
```

Run the bounded DX12 example:

```powershell
build\win_x64_debug\Samples\Showcase\ModelViewer\Debug\ModelViewer.exe `
  --smoke `
  --model Tests\Fixtures\ModelViewer\R7Triangle.gltf `
  --backend dx12 `
  --width 320 `
  --height 180 `
  --frames 8 `
  --screenshot build\win_x64_debug\ModelViewer_CompositionOrder_DX12.ppm `
  --no-ibl `
  --validation
```

Expected:

- exit code `0`;
- Engine logs Window and Resource initialization before Render;
- at least eight frames reach Present;
- the screenshot exists and is non-empty;
- Render shuts down before Window and Resource.

Run the existing DX12 sample gate when available:

```powershell
ctest --test-dir build\win_x64_debug -C Debug `
  -R "^ModelViewerGPUDrivenSmoke$" --output-on-failure
```

Finally launch `ModelViewer.exe` without arguments. It must resolve the local
Damaged Helmet, remain in the run loop instead of exiting with `-1`, and close
normally through its window. This direct launch is a local acceptance check,
not a portable CI fixture.

Execution note (2026-07-26):

- The bounded base DX12 smoke completed eight frames, wrote a non-empty
  screenshot, and exited with code `0`.
- The no-argument Damaged Helmet launch remained in the run loop for the
  bounded observation window and closed normally through its window event.
- `ModelViewerGPUDrivenSmoke` reached rendering but failed its independent
  GPU-driven readiness contract (`opaqueIndirectEligible=false`) alongside
  DX12 pipeline/resource-state validation diagnostics. Per the stop
  conditions, that RHI/Render-pass repair is not folded into this composition
  ordering patch and remains a separate follow-up gate.

### Task 6: Review and create a replacement candidate

- [ ] Inspect the diff for the approved file scope.
- [ ] Confirm no Render-to-Runtime include or link was introduced.
- [ ] Confirm the dependency API is typed and composition edges are not
  strings.
- [ ] Confirm all graph consumers share one edge model.
- [ ] Confirm reverse shutdown and initialization unwind use the realized
  order.
- [ ] Run:

  ```powershell
  git diff --check
  ```

- [ ] Commit:

  ```powershell
  git add `
    Docs\superpowers\plans\2026-07-26-engine-render-composition-order-fix.md `
    Core\Include\Core\Subsystem\SubsystemCollection.h `
    Engine\Private\Engine.cpp `
    Tests\AppModeBoundaryValidation\main.cpp `
    Tests\EngineRenderCompositionValidation\main.cpp `
    Tests\RHIContractValidation\main.cpp `
    Scripts\run_architecture_baseline.ps1

  git commit -m "fix: order engine render composition dependencies"
  ```

This repair commit supersedes `cf65f9c5` as the Task 19 candidate. Evidence
from `cf65f9c5` must not be used as final M1 evidence.

### Task 7: Re-run commit-tied Windows gates

On the exact replacement candidate:

```powershell
$env:RVX_NATIVE_ALLOW_SOFTWARE_ADAPTER = "1"

pwsh -NoProfile -ExecutionPolicy Bypass `
  -File Scripts\run_build_truth.ps1 `
  -ConfigurePreset win_x64_debug `
  -BuildPreset win_x64_debug `
  -TestPreset win_x64_debug_unit_lint `
  -BuildDir build\win_x64_debug `
  -Configuration Debug `
  -BaseRef origin/master `
  -NativeTestRegex "^NativeRenderLifecycleValidation\.RequiredBackendPresentsResizesAndStops$" `
  -RequiredNativeBackend DX12 `
  -Fresh

cmake --preset win_x64_debug_editor_compile
cmake --build --preset win_x64_debug_editor_compile `
  --target RVXEditor -- /m:1
```

Expected:

- Fresh Build Truth status is `passed`;
- report `sourceCommit` equals the replacement candidate;
- unit/lint and expanded architecture baseline pass;
- DX12 native lifecycle evidence passes;
- Editor compile-only remains green;
- branch, working, and staged diff hygiene pass.

If any code or configuration change is required, create a new candidate and
restart this task.

### Task 8: Re-run cross-platform candidate evidence

Only after explicit push/PR authorization:

- [ ] Push the replacement candidate or open a PR targeting `master`.
- [ ] Run/observe the existing CI workflow.
- [ ] Require Windows DX12 Build Truth and Editor compile.
- [ ] Require Linux Vulkan native lifecycle and TSAN.
- [ ] Require macOS Metal native lifecycle.
- [ ] Confirm every artifact records the same replacement source commit.
- [ ] Record the repaired candidate and three-platform evidence in
  `Docs/superpowers/specs/phase-log.md`.
- [ ] Commit the evidence record separately.

Do not mark M1 complete while any required platform artifact is missing.

## Stop Conditions

Stop implementation and reassess rather than broadening the patch if:

- the fix requires a Render dependency on Runtime, HAL window objects, or
  Resource implementation types;
- the collection must manually initialize a prerequisite from a callback;
- deterministic ordering cannot be expressed without replacing the existing
  lifecycle collection;
- a cycle is detected only after one or more subsystems initialize;
- ModelViewer requires backend or Render Thread changes to start;
- the repair changes Task 19 native/TSAN semantics beyond rerunning evidence.

## Acceptance Checklist

- [ ] ModelViewer no longer exits because Render precedes Window.
- [ ] Engine expresses Window/Resource prerequisites explicitly.
- [ ] Render remains independently configurable.
- [ ] Ordering is deterministic across repeated runs.
- [ ] Composition and intrinsic dependencies share one DAG.
- [ ] Duplicate, missing, self, cycle, and active-lifecycle cases are covered.
- [ ] Initialization rollback and reverse shutdown remain symmetric.
- [ ] Architecture baseline includes Engine Render composition.
- [ ] Bounded DX12 ModelViewer smoke passes.
- [ ] Fresh Windows Build Truth and Editor compile pass on the candidate.
- [ ] Linux Vulkan/TSAN and macOS Metal pass on the same pushed candidate.
