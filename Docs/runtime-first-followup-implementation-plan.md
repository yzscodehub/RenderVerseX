# RenderVerseX Runtime-First Follow-up Implementation Plan

## Goal

继续把 `codex/architecture-implementation` 收敛成 Runtime 优先、边界清晰、能力诚实、可展示、可回归验证的生产底座。

本计划从已经完成的架构收敛、诊断 JSON、Resource package mount table、JobSystem async 门禁之后继续推进。

## Driver Rules

- Driver 负责提交边界、baseline、集成冲突和最终验收。
- Subagent 只处理明确写集，不回退他人改动。
- 每个可提交单元先跑相关专项验证，再跑 `Scripts/run_architecture_baseline.ps1`。
- Editor 继续暂缓，只保留 boundary/smoke/fallback，不作为 Runtime 正确性的唯一入口。

## Remaining Phases

### Phase 8A - Serialization Honesty

Status: implemented in driver.

- `JsonArchive` read path only validates JSON syntax.
- Read deserialization is explicit unsupported and records reason.
- `RenderHonestyValidation` verifies read mode does not pretend success.

### Phase 8B - Physics Honesty

Owner: Physics subagent.

- Inspect `Physics/Private/Query/RaycastQuery.cpp`.
- Inspect `Physics/Private/Collider.cpp`.
- Replace fake-success stub behavior with explicit unsupported/failure diagnostics or a minimal real implementation.
- Add focused validation in `PhysicsWorldIntegrationValidation`.
- Run `PhysicsWorldIntegrationValidation`.

### Phase 8C - Audio Honesty

Owner: Audio subagent.

- Inspect `Resource/Private/Types/AudioResource.cpp`.
- Inspect audio streaming placeholders under `Audio/Private`.
- Make unsupported streaming/fallback paths explicit.
- Add focused validation in `AudioSpatialValidation` or an existing audio-related test.
- Run `AudioSpatialValidation`.

### Phase 8D - Terrain Honesty

Owner: Terrain subagent.

- Inspect `Terrain/Private/Heightmap.cpp`.
- Inspect `Terrain/Private/TerrainMaterial.cpp`.
- Inspect `Terrain/Private/TerrainLOD.cpp`.
- Keep deterministic simplified fallbacks only when they are named and diagnosed honestly.
- Add focused Terrain validation if an existing target can own it cleanly.
- Build `RVX_Terrain` and run the chosen validation.

### Phase 8E - Stub/TODO Gate

Owner: driver.

- Re-run targeted `rg -i "stub|placeholder|TODO"` over Runtime production modules.
- Classify remaining hits as:
  - explicit unsupported with test,
  - deterministic fallback with diagnostic,
  - Editor-only deferred work,
  - issue-backed future work.
- Add or update honesty tests for any production path that can otherwise fake success.

### Phase 9 - Showcase Runtime Entry Points

Owner: driver plus optional subagents.

- Keep `Samples/Basic` as low-level examples.
- Keep `Samples/Showcase` as Runtime showcase and visual regression entry.
- Normalize showcase CLI:
  - `--backend`
  - `--smoke`
  - `--frames`
  - `--screenshot`
  - `--report`
  - `--width`
  - `--height`
  - `--quality`
  - `--diagnostics`
- Add/finish showcase reports for:
  - rendering/post-process/material/lighting,
  - interaction,
  - particle FX,
  - terrain/water,
  - resource runtime,
  - physics/audio.
- Hook core showcase smoke into `ImageContentValidation` or `VisualGoldenValidation` where stable.

### Phase 10 - Visual Effects Production Path

Owner: render-focused implementation stages.

- Keep all effects Runtime + RenderGraph first.
- Expand `RenderVisualQualityPreset` usage.
- Ensure `PostProcessStackExecuteStats` records requested/supported/scheduled/reason/formats.
- Prioritize:
  - ToneMapping,
  - Bloom,
  - FXAA,
  - ColorGrading,
  - Vignette,
  - ChromaticAberration,
  - FilmGrain,
  - SSAO low tier,
  - sky/atmosphere baseline,
  - CSM quality diagnostics.
- Advanced effects remain capability-gated until real low-tier implementations exist.

### Phase 11 - Feature Render/RHI Boundary Migration

Owner: driver, one feature at a time.

Order:

1. Particle
2. Water
3. Terrain

For each feature:

- Feature public headers must not expose `Render/` or `RHI/`.
- Feature module owns CPU simulation/config/state only.
- Render-facing data moves into `RenderContracts` or feature snapshot contracts.
- `RenderExtraction` builds snapshots.
- `Render` owns GPU/RHI calls.
- Lower the corresponding legacy budget after each migrated edge.
- Remove the legacy edge when budget reaches zero.

### Phase 12 - Final Merge Gate

- Worktree clean.
- Small reviewable commits.
- Architecture baseline green.
- Relevant feature validation green.
- Runtime can initialize, load cooked/package resources, build scene snapshot, render a frame.
- Unsupported/fallback paths have structured diagnostics and do not fake success.

## Verification Commands

```powershell
cmake --build build\win_x64_debug --config Debug --target CoreDebugConfigValidation RHIContractValidation RenderGraphValidation RenderHonestyValidation RenderSceneValidation ActorComponentValidation AppModeBoundaryValidation ResourceInstantiationValidation ResourceRuntimePolicyValidation SystemIntegrationTest
.\Scripts\run_architecture_baseline.ps1 -BuildDir build\win_x64_debug -Configuration Debug
```

Feature-specific commands should be run before each feature commit:

```powershell
cmake --build build\win_x64_debug --config Debug --target PhysicsWorldIntegrationValidation AudioSpatialValidation ParticleValidation RenderPassValidation RenderGraphValidation
ctest --test-dir build\win_x64_debug -C Debug -R "PhysicsWorldIntegrationValidation|AudioSpatialValidation|ParticleValidation|RenderHonestyValidation" --output-on-failure
```
