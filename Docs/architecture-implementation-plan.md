# RenderVerseX Architecture Implementation Plan

Date: 2026-07-02
Branch: `codex/architecture-implementation`
Base: `engine-remediation` at `6b09c4d`
Worktree: `.claude/worktrees/architecture-implementation`

## Objective

Move RenderVerseX from a feature-rich migration state toward a production-grade
engine framework with explicit module boundaries, verified runtime contracts,
and a staged migration path for render extraction, scene/component ownership,
resource lifecycle, and quality gates.

This plan is deliberately incremental. It does not rewrite the engine in one
step. Each phase must leave the branch buildable and must add or preserve an
automated gate before the next phase starts.

## Current Baseline

The implementation branch starts from the committed `engine-remediation`
history, not from uncommitted local workspace edits. The current code already
has several important foundation pieces:

- Engine and World subsystem lifetimes through `EngineSubsystem`,
  `WorldSubsystem`, and `SubsystemCollection`.
- Modern RHI contracts for explicit barriers, descriptors, uploads, heaps,
  queries, backend capabilities, and multi-backend validation.
- RenderGraph validation and diagnostics for pass/resource hazards.
- Render proxy data and a scene bridge, with legacy collector fallback still
  present.
- Actor/ActorComponent migration scaffolding while legacy Component support
  remains.
- Resource manager, cook tooling, GPU upload/resource managers, and render
  honesty tests.

The main architecture risks to burn down are:

- Render private includes currently reach up into Engine, World, Resource, and
  Scene implementation headers.
- Render extraction is not yet a hard contract; `SceneRenderer` can still fall
  back to legacy scene collection.
- Actor/ActorComponent and legacy Component paths coexist.
- Multiple global service access patterns coexist with subsystem lifetime
  ownership.
- Resource loading has async/cook hooks but is not yet a full shipping runtime
  package/VFS/residency model.

## Phase Order

### P0: Baseline and Gates

Goal: make the migration measurable before changing architecture.

Tasks:

- Keep the worktree clean and isolated from the main workspace.
- Record this implementation plan.
- Add a minimal architecture baseline runner for representative unit tests.
- Use the baseline runner before and after each later phase.

Gate:

```powershell
.\Scripts\run_architecture_baseline.ps1 -BuildDir build\win_x64_debug -Configuration Debug
```

### P1: Module Boundary Scaffolding

Goal: make the intended dependency direction visible to the build and tests.

Tasks:

- Add `Docs/module-boundaries.json` as the source of truth for allowed and
  tolerated legacy include edges.
- Add `Scripts/check_module_boundaries.py` and CTest coverage through
  `Architecture.ModuleBoundaries`.
- Introduce `RenderContracts` as a header-only render contract boundary for
  render-only snapshot data.
- Move pure data types used across Scene/World and Render behind that boundary.
- Start removing Render private include dependence on World/Scene headers.
- Add a boundary validation check that can fail if Render reaches across layers.

Gate:

- Baseline runner passes.
- Boundary check reports no new upward Render includes.
- Legacy edges are reported explicitly and can be tightened with
  `--fail-on-legacy`.

### P2: Extract/Proxy Default Path

Goal: make Render consume immutable scene snapshots instead of gameplay objects.

Tasks:

- Make legacy render collection fallback opt-in on `SceneRenderer` so the
  default path cannot silently leave proxy extraction.
- Move scene-to-render proxy building to the dedicated `RenderExtraction`
  module.
- Keep `RenderScene` and `SceneRenderer` consuming render-only data.
- Make legacy fallback observable and keep it outside the default path.

Gate:

- Render proxy bridge tests pass.
- Normal sample/editor paths report proxy path usage and zero legacy fallback
  frames unless explicitly requested by a compatibility test.

### P3: Actor/Component Model Convergence

Goal: prevent long-term dual ownership between `ActorComponent` and legacy
`Component` containers.

Tasks:

- Add `Architecture.PhaseGates` coverage to reject new production
  `MeshRendererComponent` instantiation.
- Define `Actor` as the authoring facade and component lifetime owner.
- Keep legacy Component as compatibility only.
- Route new model/prefab instantiation through ActorComponent paths.
- Add migration tests for legacy rejection, serialization, prefab restore, and
  scene hierarchy compatibility.

Gate:

- ActorComponent and ResourceInstantiation validation pass.
- New code paths do not add legacy components unless a compatibility test
  explicitly requests it.

### P4: Service and Lifetime Unification

Goal: reduce unowned global state.

Tasks:

- Add `Architecture.PhaseGates` coverage so `Services::Get` and
  `SystemManager` do not spread outside compatibility/foundation surfaces.
- Prefer EngineSubsystem and WorldSubsystem ownership for runtime services.
- Restrict `Services`, `SystemManager`, and singleton access to documented
  compatibility or foundation use.
- Add shutdown/multi-world isolation tests.

Gate:

- Subsystem dependency validation catches missing dependencies and cycles.
- Multi-world service access remains isolated.

### P5: Resource, Cook, and Runtime Closure

Goal: make asset loading suitable for shipping builds.

Tasks:

- Replace busy-wait load waiting with condition/future-based completion.
- Add `Architecture.PhaseGates` coverage to reject `ResourceHandle` busy-wait
  regressions and keep GPU resource budget controls visible.
- Finish hot reload/file watcher behavior or make unsupported states explicit.
- Separate source assets, cooked artifacts, runtime package/VFS access, and GPU
  residency.
- Track memory and upload budgets through Resource and GPUResourceManager.

Gate:

- Cooked assets load without source asset reads in shipping-style tests.
- Missing/invalid assets produce explicit fallback diagnostics.

### P6: Quality and Capability Gates

Goal: make production quality enforceable.

Tasks:

- Add `Architecture.PhaseGates` coverage for representative capability,
  unsupported, fallback, and proxy-default honesty tests.
- Map each capability bit to a contract test.
- Keep render honesty tests for unsupported/fallback paths.
- Maintain unit, GPU, visual golden, and cross-backend lanes.
- Add performance/memory counters as non-visual regression artifacts.

Gate:

- Capability tests distinguish supported, unsupported, fallback, and skipped
  with explicit reasons.
- Visual and GPU lanes are documented with required hardware assumptions.

## Minimal Architecture Baseline

The baseline runner intentionally avoids broad GPU/visual smoke by default. It
selects representative tests for:

- Core debug configuration.
- RenderGraph lifecycle, barriers, aliasing, and async fallback honesty.
- RenderScene proxy extraction and legacy fallback observability.
- Actor/Component lifecycle and legacy container rejection.
- Resource instantiation and World load behavior.
- Render honesty checks for placeholder/stub visibility.
- System integration for Scene and Resource basics.

This set should stay fast enough to run after every architecture edit. Broader
unit, GPU, and visual lanes remain necessary before merging large phases.

## Change Discipline

- Do not fold unrelated feature work into architecture phases.
- Do not remove compatibility behavior without a test proving the replacement.
- Do not report unsupported features as success.
- Do not hide dependencies by adding include directories without a migration
  note and a follow-up gate.
- Keep phase commits small enough to review independently.
