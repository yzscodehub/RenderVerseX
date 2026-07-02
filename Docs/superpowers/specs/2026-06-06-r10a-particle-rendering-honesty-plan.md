# R10a - Particle Rendering Honesty Hardening Plan

Date: 2026-06-06

Parent program: `Docs/superpowers/specs/2026-05-30-render-program-plan-v1.md`

## 1. Stage Source

R10 is `Particle Rendering Honesty and Minimal Visual Path`.

R10 allows either:

- a CPU fallback visible path selected by the phase plan, or
- particles explicitly unsupported.

This sub-stage selects the explicit unsupported path. A CPU fallback visible path is intentionally deferred because the current renderer still lacks connected particle graphics pipelines, shader/pipeline creation, descriptor binding, and sample visual coverage.

## 2. Current Code Findings

- `ParticleSystemInstance` defaults simulation to unsupported and does not advance simulation time without a connected simulator.
- `ParticleSubsystem` does not currently obtain an RHI device from `RenderSubsystem`, so it cannot construct a real renderer path during normal initialization.
- `ParticleRenderer` always reports rendering unsupported because particle render pipelines are not implemented.
- `ParticleRenderer::DrawParticles()` and `DrawParticlesIndirect()` return `void`, making "draw skipped" harder to assert.
- `ParticlePass::IsEnabled()` returns false when the renderer is unsupported, but `ParticlePass` does not override `IsSupported()` or `GetUnsupportedReason()`, so `GetStatus()` can still report `supported=true`.
- `ParticlePass::Setup()` uses `m_colorTarget` and `m_depthTarget` without copying `view.colorTarget`/`view.depthTarget`; if a future renderer reports supported, the pass would not declare the intended RenderGraph resources.

## 3. Approved Scope

Implement only R10a honesty hardening:

- Override `ParticlePass::IsSupported()` and `GetUnsupportedReason()` so pass status reflects missing renderer or unsupported renderer state.
- Keep `ParticlePass::IsEnabled()` false unless the pass is requested, supported, and has renderable batches.
- Make `ParticlePass::Setup()` use `view.colorTarget` and `view.depthTarget` when enabled, and declare no resources when unsupported.
- Change `ParticleRenderer::DrawParticles()` and `DrawParticlesIndirect()` to return `bool`; return `true` only when a draw command is actually submitted.
- Ensure draw methods return `false` for unsupported renderer, missing instance/system, zero alive particles, missing simulator, missing pipeline, or missing required quad buffers.
- Add targeted honesty tests for renderer unsupported draw return values, particle pass status, unsupported setup no-op behavior, and future-enabled resource declaration readiness where feasible with test doubles.
- Update `phase-log.md` after implementation and Spark code review.

## 4. Out of Scope

- CPU particle fallback simulation connection.
- Particle graphics pipeline creation.
- Shader descriptor binding for particle buffers, alive index buffers, textures, depth, or constants.
- Particle sample scene or visual golden baseline.
- Integrating `ParticlePass` into `SceneRenderer`'s production pass chain.
- Full particle authoring, editor panels, or advanced simulation.

## 5. Expected Files

- `Docs/superpowers/specs/2026-06-06-r10a-particle-rendering-honesty-plan.md`
- `Docs/superpowers/specs/phase-log.md`
- `Particle/Include/Particle/Rendering/ParticleRenderer.h`
- `Particle/Private/Rendering/ParticleRenderer.cpp`
- `Particle/Include/Particle/Rendering/ParticlePass.h`
- `Particle/Private/Rendering/ParticlePass.cpp`
- `Tests/RenderHonestyValidation/main.cpp`
- Possibly `Tests/CMakeLists.txt` only if an additional validation executable is required; default plan is to extend `RenderHonestyValidation`.

## 6. Tests and Validation

Required:

```powershell
cmake --build build/win_x64_debug --config Debug --target RenderHonestyValidation
build\win_x64_debug\Tests\Debug\RenderHonestyValidation.exe
ctest --test-dir build/win_x64_debug -C Debug --output-on-failure -R "RenderHonestyValidation"
```

Render regression gate:

```powershell
cmake --build build/win_x64_debug --config Debug --target RenderHonestyValidation RenderPassValidation RenderSceneValidation ModelViewer VisualGoldenValidation ImageCompareValidation
ctest --test-dir build/win_x64_debug -C Debug --output-on-failure -R "RenderHonestyValidation|RenderPassValidation|RenderSceneValidation|ModelViewerSmoke|VisualGoldenValidation|ImageCompareValidation"
```

Final patch hygiene:

```powershell
git diff --check
```

Visual gate note: R10a does not render particles in a sample. The visual gate is still run to ensure this honesty hardening does not regress ModelViewer.

## 7. Acceptance Criteria

- `ParticlePass::GetStatus()` no longer reports supported when renderer support is unavailable.
- Unsupported particle renderer/pass paths are observable through return values, status, and non-empty unsupported reasons.
- Unsupported particle pass setup declares no RenderGraph resources and executes no draw commands.
- If a future supported renderer path is supplied in tests, `ParticlePass::Setup()` uses the active `ViewData` color/depth handles instead of stale member handles.
- Required tests and ModelViewer smoke/golden gate pass.

