# R3 Pipeline and PSO Foundation Implementation Plan

**Date:** 2026-06-02
**Source roadmap:** `Docs/superpowers/specs/2026-05-30-render-program-plan-v1.md`
**Source section:** `8. R3 - Pipeline and PSO Foundation`
**Sub-stage:** R3 - Pipeline and PSO Foundation
**Prerequisite:** R2 committed as `91b2ddb`

---

## 1. Stage Goal

R3 turns the default render pipeline path into a stable, inspectable PSO foundation before material and pass completion:

- Default lit pipeline creation must be driven by shader reflection metadata instead of hand-maintained descriptor layout duplication.
- Pipeline state must have a deterministic key/hash covering shaders, formats, fixed-function state, input layout, backend, and depth convention.
- PipelineCache must expose visible failures for missing shaders, missing reflection, incompatible layouts, invalid formats, and backend pipeline creation failure.
- PipelineCache must persist a lightweight manifest of its pipeline state inputs so stale hashes/configurations can be detected and invalidated.
- The depth baseline must move to D32F and make reverse-Z state explicit and testable.

This stage is a PSO/cache contract stage. It does not implement the full material binding model, shader permutation authoring UI, render pass visual quality, or final ModelViewer validation.

Plan review gate:

- Spark plan review should judge whether this document correctly scopes R3, names implementable code changes, and defines sufficient tests.
- The current gaps in section 2 are the implementation targets for R3 and are not expected to be fixed before plan review.
- A plan-review blocker should be a document/scope/ordering/testability problem, not the mere existence of current code gaps that R3 is designed to close.

Visual gate: N/A for R3 because R7 creates the visual golden baseline. R3 must remain covered by unit/backend validation and must not claim visual completion.

Sub-stage split:

- **R3a:** PipelineCache diagnostics/config surface, reflection-driven default-lit layout, deterministic PSO hash/key, and `PipelineCacheValidation` core tests. R3a does not switch the runtime depth baseline.
- **R3b:** PipelineCache manifest save/load/invalidation plus D32F/reverse-Z baseline and depth format wiring.
- Each sub-stage follows the same Spark plan/code review gates and gets its own commit before the next sub-stage starts.

---

## 2. Current Code Observations

From the current codebase:

- `PipelineCache::CreatePipelineLayout()` hardcodes the descriptor set layouts for `DefaultLit.hlsl` even though R2 now provides reflection metadata in `ShaderCompileResult`.
- `PipelineCache::CreateDefaultLitPipeline()` constructs `RHIGraphicsPipelineDesc` ad hoc and does not validate required shaders/layout/formats before calling the backend.
- There is no deterministic PSO key for a default lit pipeline variant, so cache invalidation and state identity are not testable.
- There is no PipelineCache manifest or serialization layer. The class creates fresh pipelines every initialization and cannot detect whether a saved cache is stale.
- `PipelineCache` uses `D24_UNORM_S8_UINT`; `SceneRenderer::EnsureDepthBuffer()` also creates a D24S8 depth target. The roadmap calls for a D32F baseline.
- Reverse-Z is not represented in pipeline state. Depth compare defaults to `Less`, and render pass clear depths are hardcoded to `1.0f`.
- There is no `PipelineCacheValidation` test target.
- Existing `MaterialSystemValidation` checks material alpha-mode classification, but not PipelineCache state.

---

## 3. R3 Scope

### R3.1 - PipelineCache diagnostics and configuration

Files:

- `Render/Include/Render/PipelineCache.h`
- `Render/Private/PipelineCache.cpp`
- `Tests/PipelineCacheValidation/main.cpp`

Tasks:

- Add a small configuration surface to `PipelineCache`:
  - depth format field;
  - reverse-Z enabled flag;
  - optional pipeline cache manifest directory.
- R3a may expose these fields with the current runtime defaults. R3b owns switching the default depth format baseline to `RHIFormat::D32_FLOAT` and making reverse-Z explicit/testable; reverse-Z remains opt-in until R7 migrates projection matrices.
- Add `GetLastError()` and lightweight stats/diagnostics:
  - last pipeline state hash;
  - manifest loaded/valid/invalidated state;
  - pipeline creation count/cache hit/miss counts where observable.
- Ensure all initialization failure paths set a clear last error string.

Acceptance:

- Tests can assert failure reasons without parsing logs.
- Existing call sites still compile without new required arguments.

### R3.2 - Reflection-driven default lit pipeline layout

Files:

- `Render/Private/PipelineCache.cpp`
- `ShaderCompiler/Private/ShaderLayout.cpp` only if deterministic ordering/validation fixes are required
- `Tests/PipelineCacheValidation/main.cpp`

Tasks:

- Build default lit descriptor set layout descriptions from `m_vsCompileResult->reflection` and `m_psCompileResult->reflection`.
- Preserve the existing set contract:
  - set 0: frame constants;
  - set 1: object constants;
  - set 2: material constants/textures/sampler.
- Convert the object and material constant-buffer bindings to dynamic uniform bindings because runtime descriptor sets use dynamic offsets.
- Sort generated layout entries deterministically by binding/type.
- Validate that required default-lit bindings exist:
  - set 0 binding 0 uniform buffer;
  - set 1 binding 0 uniform buffer;
  - set 2 binding 0 uniform buffer;
  - set 2 texture bindings 1-5;
  - set 2 sampler binding 6.
- Fail explicitly if reflection is missing or any required default-lit binding is incompatible.
- Future optional shader bindings may be ignored with a visible diagnostic in R3a as long as they do not collide with the required default-lit contract.

Acceptance:

- Descriptor layouts are generated from reflection metadata, not duplicated by hand.
- Incompatible reflection fails before backend descriptor/pipeline creation.

### R3.3 - PSO state contract and hashing

Files:

- `Render/Include/Render/PipelineCache.h`
- `Render/Private/PipelineCache.cpp`
- `Tests/PipelineCacheValidation/main.cpp`

Tasks:

- Add a deterministic hash/key for the default lit pipeline variant.
- Hash inputs must include:
  - backend type;
  - vertex/pixel shader source dependency hashes or bytecode fallback hashes;
  - material variant;
  - render target format;
  - depth stencil format;
  - depth compare/write/read state;
  - blend state;
  - rasterizer state;
  - primitive topology;
  - input layout.
- Route opaque, masked, and transparent pipelines through a single `GetOrCreateDefaultLitPipeline()` helper keyed by state.
- Fail explicitly when vertex shader, pixel shader, pipeline layout, or required formats are missing.
- Canonicalization rules:
  - hash enum/integer fields by their underlying value;
  - hash floating-point fields by their raw IEEE bits after normalizing `-0.0f` to `0.0f`;
  - hash bool fields as `0` or `1`;
  - hash input layout elements in vector order after replacing null semantic names with empty strings;
  - hash blend render targets up to `RVX_MAX_RENDER_TARGETS` so inactive defaults remain deterministic.

Acceptance:

- Equivalent state generates the same key; changing format/depth/blend/shader hash changes the key.
- Pipeline variants are cacheable and inspectable.

### R3.4 - PipelineCache manifest serialization/invalidation

Files:

- `Render/Private/PipelineCache.cpp`
- `Render/Include/Render/PipelineCache.h` if stats/config are exposed
- `Tests/PipelineCacheValidation/main.cpp`

Tasks:

- Persist a lightweight manifest file in the configured cache directory after successful initialization.
- Store engine-facing inputs, not backend binary blobs:
  - manifest magic/version;
  - backend;
  - shader source hashes;
  - render/depth formats;
  - reverse-Z flag;
  - pipeline variant hashes.
- On initialization, load the manifest if present and mark it valid only if all relevant inputs still match.
- Invalidate stale manifests visibly and continue by recreating pipelines.
- Missing manifest is a cold init, not an error.
- Corrupt, partial, or version-mismatched manifest is recorded as invalidated and then treated as cold init.
- R3 manifest persistence stores engine metadata only; no backend binary cache blob is claimed.

Acceptance:

- Tests can create a manifest, reload it as valid, then mutate shader/config input and observe invalidation.
- No backend native pipeline cache is claimed in this stage.

### R3.5 - D32F and reverse-Z baseline

Files:

- `Render/Include/Render/PipelineCache.h`
- `Render/Private/PipelineCache.cpp`
- `Render/Private/Renderer/SceneRenderer.cpp`
- render pass files that hardcode depth clear values if required for consistency
- `Tests/PipelineCacheValidation/main.cpp`

Tasks:

- Switch `PipelineCache` depth-stencil format to `RHIFormat::D32_FLOAT` by default.
- Switch `SceneRenderer` depth texture/view creation to `RHIFormat::D32_FLOAT`.
- Make reverse-Z state explicit and opt-in:
  - reverse-Z pipeline compare should be `GreaterEqual`;
  - forward-Z pipeline compare remains `Less`;
  - clear depth convention is represented in one helper, so render passes do not each invent a value.
- Because current camera/projection code is still forward-Z, keep runtime render pass behavior conservative by default, expose and test the reverse-Z PSO convention as opt-in, and record the visual follow-up for R7.
- Projection matrix migration and visual validation are deferred to R7 unless the user explicitly approves a separate sub-stage. R3 only makes the PSO/depth-resource convention explicit and test-covered.

Acceptance:

- Pipeline descs use D32F by default.
- Tests can assert reverse-Z opt-in vs forward-Z default compare/clear convention.

### R3.6 - Tests

Required tests:

- Add `PipelineCacheValidation`.
- Cover initialization failure for:
  - null device;
  - missing shader directory/file;
  - failed shader creation;
  - incompatible/missing reflection;
  - backend pipeline creation failure.
- Cover reflection-driven layout:
  - fake device captures descriptor set layouts;
  - generated layouts contain required default-lit bindings;
  - object/material uniform buffers become dynamic uniform buffers.
- Cover PSO key:
  - stable for identical state;
  - changes when render target format, depth format, variant blend/depth state, or shader hash changes.
- Cover manifest:
  - saved after successful init;
  - loaded as valid on same inputs;
  - invalidated after shader/source hash or config changes.
- Cover D32F/reverse-Z:
  - default depth format is `D32_FLOAT`;
  - reverse-Z compare is `GreaterEqual`;
  - forward-Z compare is `Less`.
- Continue to run `MaterialSystemValidation`.
- Run backend validation for the active primary backend, at minimum DX12 on this Windows setup.

---

## 4. Out of Scope

- Native backend pipeline cache blobs and driver-level PSO persistence.
- Full material binding redesign or shader permutation authoring beyond the default lit variants.
- RenderGraph render-target/depth resource ownership changes.
- Visual golden validation or ModelViewer final acceptance.
- Full reverse-Z projection-matrix migration if it requires camera/runtime visual changes beyond PSO state and D32F depth target.

---

## 5. Implementation Order

R3a:

1. Add `PipelineCacheValidation` target and fake RHI device/resources for deterministic tests.
2. Add `PipelineCache` diagnostics/configuration and failure reasons.
3. Add default-lit reflection layout generation and validation.
4. Add PSO state key/hash and route opaque/masked/transparent creation through keyed helpers.
5. Build and run R3a validation.
6. Run Spark code review.
7. Update `phase-log.md`.
8. Commit R3a.

R3b:

1. Add manifest save/load/invalidation.
2. Add D32F default depth format and explicit opt-in reverse-Z/forward-Z depth-state helpers.
3. Update `SceneRenderer` depth buffer format to D32F if the code path remains mechanically safe.
4. Build and run R3b validation.
5. Run Spark code review.
6. Update `phase-log.md`.
7. Commit R3b.

---

## 6. Validation Commands

Expected commands after implementation:

```powershell
cmake --build build/win_x64_debug --config Debug --target PipelineCacheValidation MaterialSystemValidation DX12Validation
build\win_x64_debug\Tests\Debug\PipelineCacheValidation.exe
ctest --test-dir build/win_x64_debug -C Debug --output-on-failure -R "PipelineCacheValidation|MaterialSystemValidation|DX12Validation"
git diff --check
```

Optional if touched code risk warrants:

```powershell
cmake --build build/win_x64_debug --config Debug --target VulkanValidation RenderHonestyValidation
ctest --test-dir build/win_x64_debug -C Debug --output-on-failure -R "VulkanValidation|RenderHonestyValidation"
```

---

## 7. Done Criteria

- Spark plan review result: PASS.
- PipelineCache default lit layouts are generated from shader reflection and validated.
- Pipeline variants have deterministic state keys.
- PipelineCache manifest save/load/invalidation is testable.
- Missing/incompatible shader/layout/pipeline paths fail with visible errors.
- D32F and reverse-Z PSO state are explicit and covered by tests.
- Required validation targets pass.
- Spark code review result: PASS.
- `phase-log.md` records R3 evidence.
- R3 commit is created before starting R4.
