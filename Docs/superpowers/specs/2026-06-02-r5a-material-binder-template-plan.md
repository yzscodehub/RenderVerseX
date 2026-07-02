# R5a Material Binder and Template Minimum Wiring Implementation Plan

**Date:** 2026-06-02
**Source roadmap:** `Docs/superpowers/specs/2026-05-30-render-program-plan-v1.md`
**Source section:** `10. R5a - Material Binder and Template Minimum Wiring`
**Sub-stage:** R5a - Material Binder and Template Minimum Wiring
**Prerequisite:** R4 committed as `de1118c` with log correction `c07883d`

---

## 1. Stage Goal

R5a removes the remaining material compile/bind ambiguity without pulling in R5b's
SceneRenderer material wiring:

- `MaterialTemplate::Compile()` must either produce a real compiled pipeline or
  fail with a specific visible reason.
- `MaterialBinder::Bind*()` must either bind/update the data it owns or expose a
  precise error/unsupported/fallback status.
- CPU `Material` properties must convert into `MaterialGPUConstants` honestly
  instead of returning a hard-coded TODO default.
- Explicit fallback material binding must be observable through status and log
  evidence.

R5a is not a renderer integration stage. SceneRenderer material table/descriptor
set wiring remains R5b.

Plan review gate:

- Spark plan review should judge whether this plan closes the R5a honesty and
  minimum wiring gaps without starting R5b.

Visual gate: N/A. R7 owns visual golden baselines, and R12 owns final ModelViewer
acceptance.

---

## 2. Current Code Observations

- `MaterialTemplate::Compile()` already returns `false`, clears compiled state,
  and records a visible "not implemented" error. It does not distinguish invalid
  inputs from missing pipeline compilation support.
- `MaterialTemplate` has shader path fields and parameter layout state, but no
  direct shader compiler or pipeline cache dependency. R5a should not invent a
  new compiler path outside the existing pipeline foundation.
- `MaterialBinder::Initialize()` rejects null devices and creates an upload
  constant buffer when possible.
- `MaterialBinder::Bind(const Material&)` updates the constant buffer, then marks
  `Unsupported` because no concrete descriptor/pipeline layout binding exists.
  If mapping fails, the path can still be overwritten to `Unsupported`, which
  blurs error vs unsupported.
- `MaterialBinder::ConvertToGPU()` is a TODO placeholder that ignores the
  `Material` properties and always returns hard-coded defaults.
- `MaterialBinder::Bind(uint64 materialId)` explicitly falls back to default
  material, but the fallback reason is only logged and not externally inspectable.
- `MaterialSystemValidation` already has a fake RHI capable of material constant
  buffers, texture views, samplers, and descriptor set capture.
- `RenderHonestyValidation` already ensures `MaterialTemplate::Compile()` does
  not report placeholder success and `MaterialBinder` rejects initialization
  without a device.

---

## 3. Approved Scope

### R5a.1 - MaterialTemplate compile diagnostics

Files:

- `Render/Include/Render/Material/MaterialTemplate.h`
- `Render/Private/Material/MaterialTemplate.cpp`
- `Tests/MaterialSystemValidation/main.cpp`
- `Tests/RenderHonestyValidation/main.cpp` only if an existing honesty assertion
  needs to be sharpened

Tasks:

- Keep `Compile()` conservative: do not report success unless a real pipeline is
  created.
- Add specific visible failures for:
  - null device,
  - missing vertex or pixel shader path,
  - shader paths present but standalone template pipeline compilation is not
    wired in R5a.
- Cover the shader-paths-present case as an explicit missing-pipeline test so it
  cannot be conflated with missing shader configuration.
- Ensure `m_compiled == false`, `m_pipeline == nullptr`, and
  `GetLastCompileError()` is non-empty on every failure.

Acceptance:

- Tests prove missing shader paths and unsupported standalone compilation fail
  visibly and do not set compiled state.

### R5a.2 - MaterialBinder status and message contract

Files:

- `Render/Include/Render/Material/MaterialBinder.h`
- `Render/Private/Material/MaterialBinder.cpp`
- `Tests/MaterialSystemValidation/main.cpp`

Tasks:

- Add an inspectable last bind message/reason alongside
  `GetLastBindStatus()`.
- Ensure initialization, constant-buffer creation, buffer mapping, unsupported
  material bind, and explicit fallback paths set distinct status/message values.
- Tests should assert status/message consistency for errors, unsupported binds,
  and explicit fallback binds.
- Make `UpdateConstantBuffer()` return success/failure so map/create failures
  cannot be overwritten by later unsupported status.
- Keep `Bind(const Material&)` honest: upload constants when possible, then
  report `Unsupported` until R5b supplies concrete material descriptor/pipeline
  binding. Do not count it as a successful bind.
- Keep `Bind(uint64)` as explicit default fallback until material-id lookup is
  implemented.

Acceptance:

- Tests can distinguish `Error`, `Unsupported`, and `BoundDefaultMaterial`.
- Failed constant-buffer creation or mapping remains `Error`.
- Explicit fallback material binding is observable through status/message.

### R5a.3 - Material to GPU constants conversion

Files:

- `Render/Private/Material/MaterialBinder.cpp`
- `Tests/MaterialSystemValidation/main.cpp`

Tasks:

- Implement `ConvertToGPU(const Material&)` using `Scene::Material` properties:
  base color, metallic, roughness, normal scale, occlusion strength, emissive
  color/strength, alpha mode/cutoff, workflow, and double-sided.
- Set texture flags from the material's optional texture infos:
  base color, normal, metallic-roughness, occlusion, emissive.
- Keep default constants explicit and stable.
- Add texture flag tests for both missing-texture and present-texture cases.

Acceptance:

- Tests verify representative PBR factors, alpha/workflow enums, double-sided,
  and texture flags.

---

## 4. Out of Scope

- Creating real material graphics pipelines in `MaterialTemplate` without an
  existing shader compiler/pipeline cache dependency.
- SceneRenderer material descriptor set binding or per-object material routing.
- Texture residency resolution in `MaterialBinder`; R4/R5b own the
  `MaterialSystem` texture-view path.
- Transparent/masked draw-list routing; R5b owns renderer integration.
- Visual golden and ModelViewer final validation.

---

## 5. Implementation Order

1. Recheck R5a section and existing material code before editing.
2. Add/adjust `MaterialTemplate::Compile()` diagnostics.
3. Add `MaterialBinder` last-message/status helpers and make constant-buffer
   update failure sticky.
4. Implement `MaterialBinder::ConvertToGPU()`.
5. Extend `MaterialSystemValidation` for compile diagnostics, missing-pipeline
   diagnostics, conversion, texture flags, error, unsupported, and explicit
   fallback paths.
6. Run required validation.
7. Run Spark code review.
8. Update `phase-log.md`.
9. Commit R5a before starting R5b.

---

## 6. Validation Commands

Expected commands:

```powershell
cmake --build build/win_x64_debug --config Debug --target MaterialSystemValidation RenderHonestyValidation
build\win_x64_debug\Tests\Debug\MaterialSystemValidation.exe
build\win_x64_debug\Tests\Debug\RenderHonestyValidation.exe
ctest --test-dir build/win_x64_debug -C Debug --output-on-failure -R "MaterialSystemValidation|RenderHonestyValidation"
git diff --check
```

Optional if changes unexpectedly touch shared render interfaces:

```powershell
cmake --build build/win_x64_debug --config Debug --target PipelineCacheValidation DX12Validation VulkanValidation
ctest --test-dir build/win_x64_debug -C Debug --output-on-failure -R "PipelineCacheValidation|DX12Validation|VulkanValidation"
```

---

## 7. Done Criteria

- Spark plan review result: PASS.
- `MaterialTemplate::Compile()` has specific visible failure reasons and never
  reports success without a real pipeline.
- `MaterialBinder` exposes distinct, testable bind status/message states.
- `MaterialBinder::ConvertToGPU()` reflects real `Material` properties and
  texture presence flags.
- Explicit default fallback is externally observable.
- Required validation targets pass.
- Spark code review result: PASS.
- `phase-log.md` records R5a evidence.
- R5a commit is created before R5b starts.
