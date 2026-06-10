# RQ13 - Directional Shadow Receiver Normal Bias

Date: 2026-06-10
Program: Render Quality & Verification Program v2
Previous stage: RQ12 - ChromaticAberration LDR Post-Process Activation

## 1. Stage Decision

Improve the existing directional shadow receiver path by wiring `ShadowPassConfig::normalBias`
into `ViewConstants` and `DefaultLit.hlsl`, so receivers project a normal-offset world position
when sampling the directional shadow map. This is a bounded quality fix for shadow acne on sloped
and normal-mapped surfaces, and it converts an existing dormant configuration field into a visible,
test-covered rendering control.

This stage deliberately does not implement full CSM consumption. Current evidence shows
`ShadowPass` can generate multiple cascade resources, but `OpaquePass` and `DefaultLit` only consume
cascade 0 through a single `Texture2D<float>` frame binding. Reworking that correctly needs texture
array or descriptor-array design, cascade selection data, and visual acceptance gates. RQ13 keeps
the current single-consumed directional shadow map honest and higher quality before taking that
larger step.

## 2. Current State Evidence

- `Render/Include/Render/Passes/ShadowPass.h` exposes `ShadowPassConfig::normalBias`, default `0.02f`.
- `Render/Private/Passes/OpaquePass.cpp` uploads `shadowBias`, `shadowMapSize`, and
  `filterRadiusTexels`, but not `normalBias`.
- `Render/Include/Render/Renderer/ViewData.h` has depth bias, strength, inverse map size, and filter
  radius fields, but no directional shadow normal-bias field.
- `Render/Include/Render/PipelineCache.h` `ViewConstants` contains only one shadow parameter vector:
  `DirectionalShadowParams` / `directionalShadowParams`.
- `Render/Shaders/DefaultLit.hlsl` samples `SampleDirectionalShadow(input.WorldPos)` and applies only
  depth bias before PCF comparison.
- Existing validation already covers depth bias and filter step upload, so RQ13 should extend that
  path rather than create an unverified side channel.

## 3. Scope

1. Extend `ViewData` with `directionalShadowNormalBias`, defaulting to the current
   `ShadowPassConfig::normalBias` value.
2. Extend `ViewConstants` and `DefaultLit.hlsl` with a second directional-shadow parameter vector,
   e.g. `DirectionalShadowReceiverParams`.
3. Upload normal bias from `OpaquePass` using `shadowConfig.normalBias`, clamped to a finite
   non-negative value in `PipelineCache::UpdateViewConstants`.
4. Update `DefaultLit.hlsl` so shadow sampling offsets the receiver world position along the shading
   normal before projection:
   `biasedWorldPos = worldPos + normal * normalBias`.
   The call site must use the final pixel normal after normal-map evaluation:
   `SampleDirectionalShadow(input.WorldPos, normal)`.
5. Keep depth bias behavior unchanged and still controlled by `DirectionalShadowParams.y`.
6. Keep transparent pass behavior unchanged except for any required zero/default field assignment.
7. Add source and runtime tests proving:
   - `ViewConstants` uploads normal bias.
   - `OpaquePass` forwards `ShadowPassConfig::normalBias`.
   - `DefaultLit.hlsl` samples directional shadows with the receiver normal.
   - Normal bias is clamped for invalid/negative input.
   - Existing directional shadow enable/fallback behavior remains unchanged.

## 4. Out of Scope

- Full CSM cascade selection or multi-cascade sampling.
- Texture arrays, descriptor arrays, bindless shadow maps, or per-cascade frame bindings.
- PCSS/contact shadows/VSM/EVSM/MSM.
- Shadow atlas packing.
- Shadow-map rendering bias/rasterizer state changes.
- Normal-map aware shadow-map rendering.
- New golden image baselines unless the existing shadow visual tests require a deliberate update.
- UI/editor shadow controls.

## 5. Expected Files

- `Render/Include/Render/Renderer/ViewData.h`
- `Render/Include/Render/PipelineCache.h`
- `Render/Private/PipelineCache.cpp`
- `Render/Private/Passes/OpaquePass.cpp`
- `Render/Private/Passes/TransparentPass.cpp` if default field reset is needed
- `Render/Shaders/DefaultLit.hlsl`
- `Tests/PipelineCacheValidation/main.cpp`
- `Tests/RenderPassValidation/main.cpp`
- `Docs/superpowers/specs/phase-log.md`

## 6. Required Tests

1. `PipelineCacheValidation`:
   - Extend `UpdateViewConstantsUploadsDirectionalShadowParamsAndBackendConvention` or add a focused
     test proving `directionalShadowReceiverParams.x == view.directionalShadowNormalBias`.
   - Add a clamp case proving negative or non-finite normal bias uploads `0.0f` or the chosen finite
     fallback, not a negative/NaN value.
   - Extend `ViewConstantsLayoutMatchesDefaultLitCBufferPacking` with the new receiver params offset
     and final struct size.
   - Extend the DefaultLit source guard to require:
     - `DirectionalShadowReceiverParams`
     - `float SampleDirectionalShadow(float3 worldPos, float3 worldNormal)`
     - receiver offset using `worldNormal * DirectionalShadowReceiverParams.x`.
     - the PS call site uses `SampleDirectionalShadow(input.WorldPos, normal)`.
2. `RenderPassValidation`:
   - Extend `OpaquePassDeclaresDirectionalShadowReadDuringSetup` to set a non-default
     `shadowConfig.normalBias` and assert the uploaded view constants contain it.
   - Preserve existing assertions for depth bias, filter step, and frame shadow binding.
3. Regression:
   - `RenderPassValidation`
   - `PipelineCacheValidation`
   - `RenderSceneValidation`
   - `RenderHonestyValidation`
   - `ModelViewerSmoke`
   - `VisualGoldenValidation`
   - `ModelViewerShadowSmoke`
   - `ShadowVisualGoldenValidation`
   - `ImageCompareValidation`

## 7. Validation Plan

```powershell
cmake --build build\win_x64_debug --config Debug --target PipelineCacheValidation RenderPassValidation RenderSceneValidation RenderHonestyValidation ModelViewer VisualGoldenValidation
ctest --test-dir build\win_x64_debug -C Debug --output-on-failure -R "PipelineCacheValidation|RenderPassValidation"
ctest --test-dir build\win_x64_debug -C Debug --output-on-failure -R "RenderSceneValidation|RenderHonestyValidation|ModelViewerSmoke|VisualGoldenValidation|ModelViewerShadowSmoke|ShadowVisualGoldenValidation|ImageCompareValidation"
git diff --check
```

## 8. Risks

- `ViewConstants` layout changes must match `DefaultLit.hlsl` cbuffer packing.
- Existing goldens may shift if the shadow test scene relies on receiver acne; if so, update only the
  affected shadow golden after confirming the rendered change is intended.
- Offset magnitude is world-space, so the default must stay conservative and be clamped.
- Normal offset can cause peter-panning if too high; this stage wires the control and preserves the
  existing default rather than retuning scene-specific values.

## 9. Acceptance Criteria

- Directional shadow receiver normal bias is uploaded from `ShadowPassConfig` to `ViewConstants`.
- `DefaultLit.hlsl` uses the receiver normal to offset the shadow projection position.
- Depth bias, filter radius, shadow strength, and fallback behavior continue to pass existing tests.
- Required validation commands pass.
- Spark plan review PASS before implementation.
- Spark code review PASS before commit.
- Stage implementation and phase-log are committed separately.

## 10. Spark Review

Plan review: PASS (`gpt-5.5`, xhigh)
Code review: PASS (`gpt-5.5`, xhigh)
Commit message: `feat(render): apply directional shadow normal bias`
