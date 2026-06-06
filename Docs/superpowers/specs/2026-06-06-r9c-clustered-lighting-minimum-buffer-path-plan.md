# R9c Clustered Lighting Minimum Buffer Path Plan

**Date:** 2026-06-06
**Status:** Spark plan review passed with non-blocking suggestions
**Parent R-SP:** R9 - Render Pass Completion

---

## 1. Source Check

- Roadmap: `Docs/superpowers/specs/2026-05-30-render-program-plan-v1.md`
- Section reread before this plan: `15. R9 - Render Pass Completion`
- Source lines checked: roadmap lines 485-513
- Phase log reread: `Docs/superpowers/specs/phase-log.md`, latest committed entry `R9b: Shadow/PSSM Minimum Resource Path`

R9 remaining scope includes:

- Clustered lighting minimum path.
- Tone mapping.
- Bloom and/or TAA minimum path as selected in the phase plan.
- IBL minimum path.
- Pass resource declarations through RenderGraph.
- Stub passes implement-or-disable.

R9b completed only the Shadow/PSSM minimum resource path. This sub-stage implements only the clustered lighting minimum buffer path.

---

## 2. Goal

Make `ClusteredLighting` an honest, testable minimum clustered-light infrastructure path:

- It must not report initialized/ready when required GPU buffers are missing.
- It must validate clustering configuration before allocating resources.
- It must preserve the device across reconfiguration.
- It must build per-frame cluster data, assign point/spot lights, and upload cluster buffers to GPU-visible buffers.
- It must expose enough deterministic state for tests to prove work occurred.

This stage does not make ModelViewer visually different. `DefaultLit.hlsl` currently uses `ViewConstants.LightDirection` and does not consume clustered light buffers. Shader lighting integration is a later R9 sub-stage.

---

## 3. Scope

### ClusteredLighting Honesty

- Add explicit initialized/ready state independent from `m_device`.
- Add `GetLastError()` and clear/set visible error messages for invalid device/config/buffer/upload paths.
- Validate `ClusteringConfig`:
  - cluster dimensions must be non-zero,
  - near/far planes must be finite and ordered,
  - `maxLightsPerCluster` must be non-zero,
  - total cluster and light-index allocation sizes must not overflow `uint64`.
- Keep the existing public class shape where possible, but return `bool` from per-frame operations if needed. Existing callers can ignore the return value.

### Buffer Allocation And Reconfiguration

- Create four buffers only after config validation passes:
  - cluster AABB structured buffer,
  - cluster offset/count structured buffer,
  - light index structured buffer,
  - cluster constants buffer.
- Assign debug names and strides where appropriate.
- `Reconfigure()` must keep the previous device pointer instead of losing it through `Shutdown()`.
- Failed buffer creation must leave `IsInitialized() == false` and set a visible last error.

### Per-Frame Minimum Path

- `BeginFrame()` must reject calls before initialization and reject zero viewport dimensions.
- Build cluster AABBs deterministically from current view/projection and config.
- `AssignLights()` must reject calls before a frame begins.
- Assign point and spot lights from `LightManager` into cluster light-index lists without exceeding `maxLightsPerCluster`.
- Track deterministic stats:
  - cluster count,
  - light index count,
  - active cluster count,
  - total light assignments,
  - max lights in a cluster,
  - average lights per active cluster.

### GPU Upload Minimum Path

- `UpdateGPUBuffers()` must upload cluster data, light indices, and constants into the created buffers.
- Empty light-index lists must be handled honestly without stale success assumptions.
- Upload failure due to unmappable buffers must be visible through return value and `GetLastError()`.

---

## 4. Out of Scope

- Binding clustered buffers into `PipelineCache` descriptor layouts.
- Changing `DefaultLit.hlsl`, `PBRLit.hlsl`, material shaders, or ModelViewer lighting.
- Adding a new render pass to the default `SceneRenderer` pass chain.
- Compute-shader cluster building or GPU light culling.
- Shadow-map sampling, clustered shadow assignment, IBL, ToneMapping, Bloom, or TAA.
- ECS/Object migration or RenderProxy changes.

---

## 5. Expected Files

- `Docs/superpowers/specs/2026-06-06-r9c-clustered-lighting-minimum-buffer-path-plan.md`
- `Docs/superpowers/specs/phase-log.md`
- `Render/Include/Render/Lighting/ClusteredLighting.h`
- `Render/Private/Lighting/ClusteredLighting.cpp`
- `Tests/CMakeLists.txt`
- `Tests/ClusteredLightingValidation/main.cpp`

Only add other files if a narrow compile break proves necessary.

---

## 6. Risks

- Current `ClusteredLighting::Reconfigure()` loses the device pointer through `Shutdown()`; this is a known bug this stage must fix.
- Current cluster AABB math is approximate. R9c should keep it deterministic and safe, not claim production-quality clustered shading.
- Large configs can overflow allocation sizes or create huge upload buffers if validation is missing.
- Because shader integration is out of scope, the visual golden should remain unchanged.

---

## 7. Tests To Add Or Update

### ClusteredLightingValidation

- Null device and invalid configs are rejected visibly.
- Valid initialization creates all required buffers with expected size/usage/stride/debug names.
- `Reconfigure()` preserves the device and rebuilds buffers for the new config.
- `BeginFrame()` builds the configured cluster count and clears frame stats.
- `AssignLights()` with point and spot lights produces non-zero active clusters and light-index counts.
- `AssignLights()` with no point/spot lights leaves all per-frame stats at zero and does not preserve stale assignments.
- `UpdateGPUBuffers()` writes cluster data, light indices, and constants to fake upload buffers.
- Map/upload failure is reported instead of pretending success.
- Partial upload failure is reported visibly and does not enter a success path.

### Existing Gates

- `RenderGraphValidation`
- `RenderSceneValidation`
- `RenderPassValidation`
- `VisualGoldenValidation`
- `ModelViewerSmoke`

---

## 8. Build And Validation Commands

```powershell
cmake --build build/win_x64_debug --config Debug --target ClusteredLightingValidation RenderGraphValidation RenderSceneValidation RenderPassValidation ModelViewer VisualGoldenValidation ImageCompareValidation
build\win_x64_debug\Tests\Debug\ClusteredLightingValidation.exe
ctest --test-dir build/win_x64_debug -C Debug --output-on-failure -R "ClusteredLightingValidation|RenderGraphValidation|RenderSceneValidation|RenderPassValidation|ModelViewerSmoke|VisualGoldenValidation|ImageCompareValidation"
ctest --test-dir build/win_x64_debug -C Debug --output-on-failure -R "ClusteredLightingValidation|RenderGraphValidation|RenderHonestyValidation|RenderSceneValidation|RenderPassValidation|MaterialSystemValidation|ResourceInstantiationValidation|PipelineCacheValidation|ModelViewerSmoke|VisualGoldenValidation|ImageCompareValidation"
git diff --check
```

---

## 9. Visual Gate

Required and expected to pass without golden changes.

- `ModelViewerSmoke`: PASS required
- `VisualGoldenValidation`: PASS required

If the visual golden changes, treat it as a blocker because R9c does not connect clustered buffers to lighting shaders.

---

## 10. Spark Review Gates

- Spark plan review model: `gpt-5.3-codex-spark`
- Spark plan review agent: `019e9d39-7fdc-7e50-bc6b-6b39a6dabcbe`
- Spark plan review status: `PASS_WITH_NON_BLOCKING_SUGGESTIONS`
- Non-blocking suggestions adopted before implementation: add empty-light assignment regression, explicit reconfigure device-preservation regression, and partial upload failure regression.
- Spark code review model: `gpt-5.3-codex-spark`
- Spark code review agent: `019e9d45-af0b-7d01-9009-f24e97d3dd31`
- Spark code review status: `PASS_WITH_NON_BLOCKING_SUGGESTIONS`, no blocking findings.

Code changes must not start until Spark plan review returns no blocking findings.

---

## 11. Phase Log And Commit

Phase-log entry to append after validation and Spark code review:

- R-SP: `R9c: Clustered Lighting Minimum Buffer Path`
- Commit message: `Implement R9c clustered lighting buffer path`
