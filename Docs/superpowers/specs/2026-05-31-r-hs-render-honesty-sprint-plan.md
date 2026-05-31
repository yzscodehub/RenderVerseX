# R-HS Render Honesty Sprint Implementation Plan

**Date:** 2026-05-31
**Source plan:** `Docs/superpowers/specs/2026-05-30-render-program-plan-v1.md`
**Source section:** `5. R-HS - Render Honesty Sprint`
**Mode:** Strict serial execution with Spark plan review before code changes.

---

## 1. Goal

Make render-facing false success visible before deeper render work starts.

R-HS does not implement missing production features. It changes stubbed or unsupported paths so callers can see one of these outcomes:

- The feature is supported and real.
- The feature is disabled or unsupported.
- The operation fails with a visible error.

Warn-only behavior is not sufficient when the caller can continue as if the feature succeeded.

---

## 2. Approved Scope

### R-HS.1 - Test harness and CPU-visible honesty

Files/modules:

- `Tests/CMakeLists.txt`
- `Tests/RenderHonestyValidation/main.cpp`
- `Core/Private/Serialization/Serialization.cpp`
- `Tools/Private/AssetPipeline.cpp`
- `Tools/Private/AssetDatabase.cpp`
- `Render/Private/Material/MaterialTemplate.cpp`
- `Render/Private/Material/MaterialBinder.cpp`
- `Render/Private/Debug/GPUProfiler.cpp`
- `Resource/Private/Loader/TextureLoader.cpp`

Tasks:

- Add `RenderHonestyValidation`.
- Assert invalid `JsonArchive` input does not parse as success.
- Assert placeholder asset importers do not return `success=true` without real output.
- Make `AssetDatabase::Save()` fail when it cannot write durable output.
- Make `AssetDatabase::Load()` fail when it cannot parse real persisted content.
- Add `RenderHonestyValidation` coverage for database empty path, missing file, and malformed/partial database content.
- Make `MaterialTemplate::Compile()` return failure instead of compiled success while pipeline creation is unimplemented.
- Add material binder state that lets tests distinguish default/fallback binding from real material binding.
- Make `GPUProfiler` report timestamp profiling unavailable until a query pool is actually created.
- Make texture reference fallback explicit and distinguishable from successful source texture load.

### R-HS.2 - RenderGraph honesty

Files/modules:

- `Render/Private/Graph/RenderGraphCompiler.cpp`
- `Render/Private/Graph/RenderGraphExecutor.cpp`
- `Render/Private/Graph/RenderGraphInternal.h`
- `Render/Include/Render/Graph/RenderGraph.h`
- `Tests/RenderGraphValidation/main.cpp`
- `Tests/RenderHonestyValidation/main.cpp`

Tasks:

- Replace incomplete topology execution-order fallback with a hard validation failure.
- Add a test-visible compile validity state if existing public API is insufficient.
- Mark async graph execution as graphics fallback with `asyncSupported=false`, or expose equivalent capability state.
- Mark aliasing as disabled/unsupported when explicit aliasing barriers are not emitted.
- Update tests that currently expect silent async fallback.
- Add illegal topology or cycle validation coverage that fails instead of silently repairing execution order.
- Add execution-order fallback coverage proving incomplete topology does not become success.
- Add aliasing coverage that asserts either explicit aliasing barriers are emitted or memory aliasing is disabled with a public/test-visible reason.
- Public/test-visible state belongs on `RenderGraph::CompileStats` or an adjacent `RenderGraph` query API during R-HS; R6 may replace this with the final RenderGraph validation model.

### R-HS.3 - Backend capability honesty

Files/modules:

- `RHI/Include/RHI/RHI.h`
- `RHI_Vulkan/Private/VulkanDevice.cpp`
- `RHI_Vulkan/Private/VulkanCommandContext.cpp`
- `Tests/RenderHonestyValidation/main.cpp`
- `Tests/VulkanValidation/main.cpp` if Vulkan is enabled

Scope note:

- R-HS.3 is Vulkan-only for this sprint because the identified warn-only query/timestamp stubs are in the Vulkan backend. Other backend capability parity belongs to R1.

Tasks:

- Ensure Vulkan query pool/timestamp/query command paths cannot be treated as available when unimplemented.
- Prefer capability reporting or visible unsupported status over warn-only no-op commands.
- Avoid deep Vulkan query implementation in this sprint.
- RHI/backend capability fields belong in the RHI capability layer, not in RenderGraph.
- R1 may refine capability names and backend parity after R-HS makes unsupported paths visible.

### R-HS.4 - Render feature and particle honesty audit

Files/modules:

- `Render/Private/Passes/SkyboxPass.cpp`
- `Render/Private/PostProcess/ToneMapping.cpp`
- `Render/Private/PostProcess/Bloom.cpp`
- `Render/Private/PostProcess/TAA.cpp`
- `Render/Private/PostProcess/FXAA.cpp`
- `Render/Private/PostProcess/SSAO.cpp`
- `Render/Private/PostProcess/SSR.cpp`
- `Render/Private/PostProcess/DOF.cpp`
- `Render/Private/PostProcess/MotionBlur.cpp`
- `Render/Private/PostProcess/Vignette.cpp`
- `Render/Private/PostProcess/ChromaticAberration.cpp`
- `Render/Private/PostProcess/FilmGrain.cpp`
- `Render/Private/PostProcess/ColorGrading.cpp`
- `Render/Private/PostProcess/VolumetricLighting.cpp`
- `Render/Private/Sky/AtmosphericScattering.cpp`
- `Particle/Private/ParticleSubsystem.cpp`
- `Particle/Private/ParticleSystemInstance.cpp`
- `Particle/Private/Rendering/ParticleRenderer.cpp`
- `Tests/RenderHonestyValidation/main.cpp`

Tasks:

- Add targeted assertions for enabled/default features that are known no-op stubs.
- Default renderer pass audit: `SkyboxPass` must not report an enabled successful draw path unless it has a valid pipeline and draw command.
- Post-process audit: listed post-process passes must default to disabled/unsupported or return a visible error when their TODO shader/dispatch path is reached.
- Atmospheric scattering audit: compute/render functions with TODO dispatch/render paths must report unsupported unless backed by real graph resources and dispatches.
- For particle rendering/simulation, report unsupported/disabled if simulator or renderer pipeline is disconnected.
- Do not implement full particle simulation or pass effects here.
- Completion criterion for each listed feature: either implemented already and test-visible, or disabled/unsupported with a test-visible state; log-only TODO is not sufficient.

---

## 3. Out of Scope

- Implementing full RenderGraph async compute/copy scheduling.
- Implementing Vulkan query pools or timestamp resolve.
- Implementing production material pipelines.
- Implementing full post-process effects.
- Implementing complete particle simulation.
- Changing ModelViewer visual output.
- RenderProxy work; that remains R8.

---

## 4. Red Lines

- No TODO path may set a success flag merely because the intended output path was computed.
- No compile/bind method may mark itself complete when required GPU resources were not created.
- No invalid JSON parse may return true.
- No RenderGraph validation error may be repaired silently into an execution order.
- No async path may advertise async support while executing synchronously on graphics.
- No Vulkan query/timestamp no-op may appear as supported.
- No fallback texture may be indistinguishable from a successfully loaded source texture.
- No asset database save/load may report success without durable write and parse-backed load.

---

## 5. Tests and Validation

Required new target:

- `RenderHonestyValidation`
  - Link libraries: `RVX::Core`, `RVX::RenderGraph`, `RVX::Render`, `RVX::Resource`, `RVX::Tools`, `RVX::RHI`.
  - Include paths: `Render/Include`, `Resource/Include`, `Tools/Include`.
  - Labels: `unit`.
  - Must be registered through `rvx_add_gtest` so CTest discovers it.

Required existing tests to run as relevant:

- `RenderGraphValidation`
- `MaterialSystemValidation`
- `GPUUploadServiceValidation`
- `GPUResourceManagerValidation`
- `ResourceInstantiationValidation`
- `VulkanValidation` when Vulkan is enabled and available

Preferred validation commands:

```powershell
cmake --build build --config Debug --target RenderHonestyValidation
.\build\Tests\Debug\RenderHonestyValidation.exe
cmake --build build --config Debug --target RenderGraphValidation
.\build\Tests\Debug\RenderGraphValidation.exe
ctest --test-dir build -C Debug --output-on-failure
```

Single-configuration generator fallback:

```powershell
ctest --test-dir build --output-on-failure
```

If the local build layout differs, record the actual executable paths in `phase-log.md`.

`RenderHonestyValidation` must be registered with CTest so the `ctest` command above executes it.

---

## 6. Implementation Order

1. Add `RenderHonestyValidation` with tests that expose the existing false-success paths.
2. Implement R-HS.1 fixes and run the new target.
3. Implement R-HS.2 RenderGraph compile/execution honesty and run `RenderGraphValidation`.
4. Implement R-HS.3 Vulkan capability honesty and run backend tests when enabled.
5. Implement R-HS.4 feature/particle honesty assertions.
6. Run the full relevant validation set.
7. Run Spark code review.
8. Update `phase-log.md`.
9. Commit R-HS.

---

## 7. Expected Commit Boundary

Commit message:

```text
fix(render): make render honesty failures visible
```

If R-HS becomes too large during implementation, split into:

- `test(render): add render honesty validation`
- `fix(render): make core render stubs honest`
- `fix(render): expose rendergraph unsupported fallbacks`
- `fix(vulkan): report query capability honestly`

Each split commit must still receive Spark code review for the files it changes.
