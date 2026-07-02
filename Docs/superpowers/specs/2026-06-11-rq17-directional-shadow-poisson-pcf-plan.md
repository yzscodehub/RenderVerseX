# RQ17 - Directional Shadow Poisson PCF Kernel

**Date:** 2026-06-11  
**Parent reference:** `Docs/superpowers/specs/2026-05-30-render-program-plan-v1.md` §15 R9 Render Pass Completion  
**Previous stage:** RQ16 Directional Shadow Caster Depth-Bias Pipeline (`3809a41`)  
**Status:** Plan draft; implementation blocked until Spark plan review PASS.

---

## 1. Stage Decision / Goal

Replace the current fixed 3x3 grid PCF for directional shadows with a deterministic Poisson-disc PCF kernel. RQ15
made CSM stable and RQ16 made caster-side bias explicit; RQ17 improves perceived shadow-edge quality by reducing
grid-shaped filtering artifacts while preserving the current shadow-map resources, descriptor layout, and ViewConstants
packing.

This is a first soft-shadow quality step, not PCSS or contact shadows.

---

## 2. Current State Evidence

- `Render/Shaders/DefaultLit.hlsl` implements `SampleDirectionalShadowPCF()` with nested loops over `x/y = -1..1`.
- `DirectionalShadowParams.w` already carries a UV-space filter step derived from `filterRadiusTexels / shadowMapSize`.
- `ShadowPassConfig::filterRadiusTexels` and `OpaquePass` upload path already let the CPU configure filter radius.
- The shader has no current tap pattern abstraction, no Poisson offsets, and no test guard that prevents regression to
  grid-shaped PCF.

---

## 3. Scope

1. Replace the 3x3 square-grid PCF implementation with a deterministic Poisson-disc sample pattern.
   - Use a fixed 16-tap table in `DefaultLit.hlsl`.
   - Treat offsets as unit-disc normalized values multiplied directly by the existing `DirectionalShadowParams.w`
     filter step so `filterRadiusTexels` keeps its current meaning.
   - Include a center or near-center tap so hard-contact behavior remains stable when radius is small.
   - Keep all taps deterministic; no per-frame random rotation/noise in RQ17.
2. Keep the existing `DirectionalShadowParams.w` meaning as the UV filter radius/step.
   - No ViewConstants layout change.
   - No descriptor/layout change.
3. Preserve existing guards:
   - If filter radius is zero or tiny, fall back to single shadow compare.
   - Ignore taps outside the shadow map UV range and average only valid taps.
   - Keep cascade fade logic from RQ15 unchanged.
4. Add source/behavior tests:
   - Shader source contains a named Poisson tap table.
   - Shader no longer uses the old nested `for (int y = -1; y <= 1)` / `for (int x = -1; x <= 1)` 3x3 kernel.
   - Shader still calls `CompareDirectionalShadowDepth()` and averages valid taps.
   - Existing upload tests continue proving filter radius flows into `DirectionalShadowParams.w`.
5. Run the visual gate. If the shadow golden changes, inspect actual/diff before updating it.

---

## 4. Out of Scope

- PCSS blocker search, contact shadows, ray-traced shadows, EVSM/MSM/VSM, or temporal shadow denoising.
- Changing CSM split/fade/stabilization math.
- Changing caster raster bias or receiver compare/normal bias semantics.
- Adding new shadow-map textures, atlases, or descriptor bindings.
- Adding per-pixel random noise or temporal rotation.

---

## 5. Expected Files

- `Docs/superpowers/specs/2026-06-11-rq17-directional-shadow-poisson-pcf-plan.md`
- `Render/Shaders/DefaultLit.hlsl`
- `Tests/PipelineCacheValidation/main.cpp`
- `Tests/Golden/ModelViewer/RQ3d_Shadow_DX11_320x180.ppm` only if visual inspection confirms an intended change.
- `Docs/superpowers/specs/phase-log.md` after implementation commit.

---

## 6. Required Tests

`PipelineCacheValidation`

- Add/update shader source guard for the Poisson PCF table.
- Add/update guard that rejects the old nested 3x3 grid loop.
- Keep existing shadow source guards for cascade selection, cascade fade, receiver normal bias, and filter step.
- Keep a source guard for the `filterStepUv <= 1.0e-7` single-compare fallback.

Visual gate

- `ModelViewerShadowSmoke`
- `ShadowVisualGoldenValidation`
- Inspect actual/diff before any golden update.

---

## 7. Validation Plan

```powershell
cmake --build build\win_x64_debug --config Debug --target PipelineCacheValidation ModelViewer VisualGoldenValidation ImageCompareValidation
ctest --test-dir build\win_x64_debug -C Debug --output-on-failure -R "PipelineCacheValidation"
ctest --test-dir build\win_x64_debug -C Debug --output-on-failure -R "ModelViewerSmoke|VisualGoldenValidation|ModelViewerShadowSmoke|ShadowVisualGoldenValidation|ImageCompareValidation"
git diff --check
```

If the shader change causes a golden mismatch:

1. Inspect actual and diff artifacts.
2. Confirm the change is limited to intended shadow-edge softness/shape.
3. Update only the shadow golden if the visual change is accepted.
4. Re-run the visual gate.

---

## 8. Risks

- More taps increase pixel shader cost. RQ17 should keep the tap count modest and fixed.
- Poisson PCF can soften more than the old 3x3 grid; any golden change must be inspected.
- A deterministic non-rotated pattern improves grid artifacts but does not solve all aliasing. PCSS/contact shadows remain
  later stages.
- Removing out-of-bounds taps can vary tap count near shadow-map borders; keep the current valid-tap averaging behavior.

---

## 9. Acceptance Criteria

- Directional shadow PCF uses a named deterministic 16-tap Poisson-disc table.
- Old nested 3x3 square-grid PCF is removed.
- Existing filter-radius upload semantics remain unchanged.
- Cascade selection/fade and bias behavior remain unchanged.
- Required build/test/visual gates pass.
- Spark plan review PASS before implementation.
- Spark code review PASS before commit.
- Stage implementation and phase-log are committed separately.

---

## 10. Spark Review

Plan review: PASS (`gpt-5.5`, xhigh)
Code review: PASS (`gpt-5.5`, xhigh)
Commit message: `feat(render): use poisson pcf for directional shadows`
