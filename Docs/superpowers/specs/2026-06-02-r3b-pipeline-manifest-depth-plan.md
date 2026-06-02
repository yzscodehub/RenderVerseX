# R3b Pipeline Manifest and Depth Baseline Implementation Plan

**Date:** 2026-06-02
**Source roadmap:** `Docs/superpowers/specs/2026-05-30-render-program-plan-v1.md`
**Parent plan:** `Docs/superpowers/specs/2026-06-02-r3-pipeline-pso-foundation-plan.md`
**Source section:** `8. R3 - Pipeline and PSO Foundation`
**Sub-stage:** R3b - PipelineCache manifest plus depth baseline
**Prerequisite:** R3a committed as `17204a9`

---

## 1. Stage Goal

R3b finishes the Pipeline/PSO foundation by adding durable engine-side PipelineCache metadata and making the depth convention explicit:

- Persist a lightweight PipelineCache manifest that records PSO state inputs and detects stale/corrupt cache metadata.
- Switch the default PipelineCache and SceneRenderer depth resource format from D24S8 to `RHIFormat::D32_FLOAT`.
- Add explicit forward-Z and reverse-Z depth-state helpers so the PSO contract can be tested.
- Keep reverse-Z opt-in for this sub-stage because current camera/projection code still uses forward-Z projection matrices; making reverse-Z the runtime default before projection migration would be a visual regression risk.

This stage does not implement native backend pipeline cache blobs, RenderGraph visual pass completion, projection-matrix migration, or golden-image validation.

Plan review gate:

- Spark plan review should judge whether this plan is safe for the current codebase, has enough tests, and keeps R3b scoped.
- The deliberate reverse-Z decision is: D32F becomes the default depth format now; reverse-Z becomes explicit and testable now; reverse-Z runtime default is deferred to R7 with projection migration.

Visual gate: N/A for R3b. R7 owns golden-image validation and projection/depth convention visual acceptance.

---

## 2. Current Code Observations

- R3a added `PipelineCacheConfig`, stats, deterministic PSO hashes, and `PipelineCacheValidation`.
- `PipelineCacheConfig::depthStencilFormat` still defaults to `RHIFormat::D24_UNORM_S8_UINT`; `SceneRenderer::EnsureDepthBuffer()` also creates D24S8 texture/view formats.
- `PipelineCacheConfig::manifestDirectory` and manifest stats fields exist but no manifest is loaded or saved.
- `PipelineCacheConfig::reverseZ` exists but depth state creation still passes `RHIDepthStencilState::Default()` and `ReadOnly()`, which use `RHICompareOp::Less`.
- `Scene/Private/Components/CameraComponent.cpp` and `Runtime/Private/Camera/Camera.cpp` use normal forward-Z perspective construction. R3b must not silently flip runtime depth compare to reverse-Z by default.
- `OpaquePass` and `DepthPrepass` hardcode clear depth `1.0f`. R3b can centralize the convention helper without changing runtime behavior while reverse-Z remains opt-in.

---

## 3. Approved Scope

### R3b.1 - Manifest metadata

Files:

- `Render/Include/Render/PipelineCache.h`
- `Render/Private/PipelineCache.cpp`
- `Tests/PipelineCacheValidation/main.cpp`

Tasks:

- Add a private manifest structure in `PipelineCache.cpp`.
- Persist a versioned engine-side manifest after successful initialization when `manifestDirectory` is non-empty.
- Store only engine-visible inputs:
  - manifest magic/version;
  - backend type;
  - shader source/dependency hashes and permutation/output hashes as represented by the PSO hash inputs;
  - render target format;
  - depth stencil format;
  - reverse-Z flag;
  - opaque/masked/transparent pipeline state hashes.
- Load an existing manifest before save and set stats:
  - missing manifest: cold init, no error, `manifestLoaded=false`, `manifestValid=false`, `manifestInvalidated=false`;
  - matching manifest: `manifestLoaded=true`, `manifestValid=true`, `manifestInvalidated=false`;
  - stale/corrupt/version-mismatched manifest: `manifestLoaded=true`, `manifestValid=false`, `manifestInvalidated=true`, then overwrite after successful init.
- Use a strict, deterministic text manifest format with a magic header and named scalar fields. Reject missing, duplicate, malformed, or unknown required fields instead of partially trusting the file.
- Write manifests through a temporary file, move an existing final manifest to a `.bak` file before replacement, and restore the backup if replacement fails.
- Manifest write failure is non-fatal: log a warning and keep initialization successful because runtime PSO creation already succeeded.
- Treat `reverseZ` as a strict boolean scalar: only `0` and `1` are valid manifest values.
- Do not claim or load native backend pipeline binaries.

Acceptance:

- Tests can create a manifest, reload it as valid, mutate config/source-relevant input, and observe invalidation.
- Corrupt manifest content is visible through stats without failing initialization.

### R3b.2 - D32F depth format baseline

Files:

- `Render/Include/Render/PipelineCache.h`
- `Render/Private/PipelineCache.cpp`
- `Render/Private/Renderer/SceneRenderer.cpp`
- `Tests/PipelineCacheValidation/main.cpp`

Tasks:

- Change `PipelineCacheConfig::depthStencilFormat` default to `RHIFormat::D32_FLOAT`.
- Ensure generated default-lit pipeline descs use the configured depth format.
- Change `SceneRenderer::EnsureDepthBuffer()` depth texture and view formats to `RHIFormat::D32_FLOAT`.
- Add tests that capture pipeline descs and assert default D32F.

Acceptance:

- PipelineCache default pipeline descs use `D32_FLOAT`.
- SceneRenderer depth texture/view creation has a single D32F format constant or helper instead of duplicated D24S8 values.

### R3b.3 - Explicit depth convention helpers

Files:

- `Render/Include/Render/PipelineCache.h`
- `Render/Private/PipelineCache.cpp`
- `Render/Private/Passes/OpaquePass.cpp`
- `Render/Private/Passes/DepthPrepass.cpp`
- `Tests/PipelineCacheValidation/main.cpp`

Tasks:

- Add helper methods to centralize depth convention:
  - forward-Z clear depth is `1.0f`;
  - reverse-Z clear depth is `0.0f`;
  - forward-Z writable/read-only compare is `Less`;
  - reverse-Z writable/read-only compare is `GreaterEqual`.
- Keep `PipelineCacheConfig::reverseZ` default `false` for R3b because projection migration is deferred.
- Route PipelineCache opaque/masked/transparent depth states through the helper.
- Route OpaquePass and DepthPrepass clear-depth values through `PipelineCache::GetDepthClearValue()` when a PipelineCache is available. With the R3b default this preserves current clear value.
- Do not route ShadowPass through the scene PipelineCache clear-depth helper in R3b because shadow-map projection/depth convention is not migrated with the scene camera path.
- Add tests for forward-Z default and reverse-Z opt-in pipeline descs.

Acceptance:

- Current default runtime remains forward-Z and mechanically safe.
- Reverse-Z PSO state is explicit, opt-in, and test-covered.
- R7 can flip the default only after projection matrices and visual validation are updated.

---

## 4. Out of Scope

- Projection matrix migration or camera API changes.
- Changing runtime default to reverse-Z before projection migration.
- Native DX12/Vulkan/Metal pipeline cache blobs.
- RenderGraph pass scheduling or visual pass completion.
- RenderProxy, Material bind model, Asset pipeline, or ModelViewer final acceptance.

---

## 5. Implementation Order

1. Backfill the R3a commit hash in `phase-log.md`.
2. Add manifest helpers and stats updates in `PipelineCache`.
3. Add manifest validation tests for missing, valid reload, stale config, and corrupt content.
4. Add strict manifest parser coverage for malformed boolean fields and a non-fatal manifest write-failure test.
5. Switch D32F depth format defaults in PipelineCache and SceneRenderer.
6. Add depth convention helpers and route PipelineCache/pass clear-depth usage through them without enabling reverse-Z by default.
7. Add tests for default forward-Z D32F and opt-in reverse-Z compare/clear values.
8. Build and run validation.
9. Run Spark code review.
10. Update `phase-log.md`.
11. Commit R3b before moving to R4.

---

## 6. Validation Commands

Expected commands:

```powershell
cmake --build build/win_x64_debug --config Debug --target PipelineCacheValidation MaterialSystemValidation DX12Validation
build\win_x64_debug\Tests\Debug\PipelineCacheValidation.exe
ctest --test-dir build/win_x64_debug -C Debug --output-on-failure -R "PipelineCacheValidation|MaterialSystemValidation|DX12Validation"
git diff --check
```

Optional if touched code risk warrants:

```powershell
cmake --build build/win_x64_debug --config Debug --target RenderHonestyValidation VulkanValidation
ctest --test-dir build/win_x64_debug -C Debug --output-on-failure -R "RenderHonestyValidation|VulkanValidation"
```

---

## 7. Done Criteria

- Spark plan review result: PASS.
- Manifest save/load/invalidation is implemented and testable.
- Default PipelineCache and SceneRenderer depth formats are D32F.
- Forward-Z remains the safe runtime default until projection migration.
- Reverse-Z compare/clear convention is explicit and opt-in test-covered.
- Required validation targets pass.
- Spark code review result: PASS.
- `phase-log.md` records R3b evidence.
- R3b commit is created before R4 starts.
