# RenderVerseX Module Boundaries

Date: 2026-07-02

This document records the P1 dependency scaffold for the architecture
implementation branch. The goal is to make module edges explicit before moving
large systems such as render extraction, component ownership, and runtime asset
packaging.

## Layers

The intended dependency direction is:

1. Application: `Editor`, `Samples`, `Tests`
2. Composition: `Engine`
3. Runtime and feature modules: `Runtime`, `World`, `Render`, `Resource`,
   `Scene`, `Animation`, `Audio`, `Physics`, `Particle`, `Terrain`, `Water`
4. Contracts and extraction: `RenderContracts`, `RenderExtraction`
5. HAL and backend abstraction: `HAL`, `RHI`, `RHI_*`
6. Foundation: `Core`, `Geometry`, `Spatial`, `ShaderCompiler`

Higher layers may depend downward. Lower layers should not reach upward into
composition or application layers.

## Gate

The module boundary gate is:

```powershell
python Scripts\check_module_boundaries.py --root . --config Docs\module-boundaries.json
```

When configured with tests enabled, CTest also exposes:

```powershell
ctest --test-dir build\win_x64_debug -C Debug -R Architecture.ModuleBoundaries --output-on-failure
```

The branch baseline runner includes this test once the build tree has been
reconfigured after P1.

## Legacy Edges

`Docs/module-boundaries.json` separates normal allowed edges from legacy edges.
Legacy edges are allowed by default so the current branch stays buildable, but
they are reported by the gate and capped by `legacyBudgets`. Each later phase
should lower the matching budget, then remove the legacy edge from the JSON when
the code no longer needs it.

Current legacy groups:

- `Render -> Resource`: closed. GPU upload, material binding, skybox, and IBL
  paths now consume render-facing contracts from `RenderContracts`; concrete
  Resource types implement those contracts outside the Render module.
- `Render -> Engine`: closed. Composition roots now inject `WindowSubsystem`
  before render initialization.
- `Scene -> Render`: closed. Scene renderables now expose `RenderContracts`
  proxies instead of `RenderScene` collection hooks.
- `Scene -> Resource`: closed. Scene components now hold Core/Scene asset
  handles and query render-facing interfaces instead of including concrete
  Resource handles.
- `Scene -> Animation`: closed. Animator and skeleton component implementations
  are compiled by the Animation module as Scene adapters. Render extraction
  consumes the Scene-owned `ISkinningPaletteProvider` contract and never names
  the concrete Animation adapter.
- `Scene -> Audio`: closed. `AudioComponent` exposes only forward-declared
  audio handles/clips from Scene; playback implementation is compiled by the
  Audio module.
- `Scene -> Physics`: closed. Collider and rigid body component implementations
  are compiled by the Physics module as Scene adapters; Scene headers retain
  only forward declarations and configuration state.
- `Resource -> Scene`: closed. Model asset data now lives in `Geometry/Asset`,
  and Scene actor instantiation is compiled by `ResourceSceneAdapters`.
- `Particle -> Engine`: closed. Particle services now receive `RenderSubsystem`
  through explicit composition-root injection, and particle components discover
  the active particle subsystem without including Engine.

`RenderContracts` is intentionally header-only and depends only on `Core`. It
owns render extraction data such as `RenderPrimitiveProxy`,
`RenderLightProxy`, and `RenderProxySnapshot`.

`RenderExtraction` owns Scene/World-to-render snapshot extraction. Render can
consume it as an adapter, but Scene/World details should stay inside the
extraction module.

`ResourceSceneAdapters` is the explicit composition adapter for model resource
data that needs to create Scene actors/components. Resource owns asset lifetime
and loading; the adapter owns Scene construction.

Run with `--fail-on-legacy` when a phase is ready to prove one or more legacy
edges have been burned down.
