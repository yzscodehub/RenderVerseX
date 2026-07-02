# R9f - IBL-Approximate Ambient Minimum Path Plan

Date: 2026-06-06

## Source Documents

- Parent plan: `Docs/superpowers/specs/2026-05-30-render-program-plan-v1.md`
- Parent section: `15. R9 - Render Pass Completion`
- Previous phase log: `Docs/superpowers/specs/phase-log.md`, R9e notes

## Stage Decision

R9f implements the smallest honest IBL-approximate runtime foundation still aligned with R9:

- Make environment/IBL-like ambient lighting shader-visible through view/frame constants.
- Replace the current hard-coded DefaultLit ambient terms with parameterized diffuse/specular IBL approximation.
- Preserve the current ModelViewer default look by choosing defaults equivalent to the old constants.

This phase does not implement full cubemap IBL and does not sample cubemap, irradiance, prefiltered environment, or BRDF LUT resources. It creates the minimal parameter bridge that later stages can feed from `SkyboxComponent`, HDR loader output, irradiance maps, prefiltered maps, and BRDF LUTs.

## Current State

- `DefaultLit.hlsl` computes ambient as hard-coded constants:
  - diffuse strength: `0.12`
  - specular strength: `0.04`
- `Lighting.hlsli` has an IBL-approximate ambient helper, but it remains independent from
  `DefaultLit` view constants and does not sample environment textures.
- `PipelineCache::ViewConstants` only uploads view-projection, camera position, time, and primary light direction.
- `ViewData` has no environment/IBL fields.
- `SkyboxPass` is still honestly unsupported and is not the right R9f target.
- `Resource::HDRTextureLoader` can generate IBL-like CPU texture resources, but those resources are not connected to the render frame path.

## Scope

1. Add explicit IBL-approximate ambient settings to `ViewData`.
   - Environment diffuse color.
   - Environment specular color.
   - Diffuse intensity, default `0.12`.
   - Specular intensity, default `0.04`.
   - Enable flag or scalar that can disable the approximation without changing other lighting.

2. Extend `PipelineCache::ViewConstants` and `DefaultLit.hlsl` cbuffer layout.
   - Keep existing fields in the same order.
   - Append IBL fields to minimize compatibility risk for existing shaders that read only the prefix.
   - Keep CPU/HLSL packing aligned with `float4` boundaries.

3. Replace hard-coded ambient in `DefaultLit.hlsl`.
   - Diffuse term must use view constants and preserve the old default output by using diffuse color `1,1,1` and intensity `0.12`.
   - Specular term must use view constants and preserve the old default output by using specular color `1,1,1` and intensity `0.04`.
   - Keep direct lighting, materials, textures, alpha modes, and shadows unchanged.
   - Update related shader comments so `DefaultLit.hlsl` and `Lighting.hlsli` both describe the path as an approximation, not complete texture IBL.

4. Add tests.
   - Verify `UpdateViewConstants()` uploads default IBL-approximate values equivalent to old ambient constants.
   - Verify custom `ViewData` IBL-approximate values are uploaded.
   - Verify C++ cbuffer size and field offsets preserve float4 packing expected by HLSL.
   - Verify DefaultLit shader text uses IBL constants instead of hard-coded ambient magic constants.
   - Verify the pass/pipeline validation count remains honest after the cbuffer extension.
   - Run visual gate to confirm default ModelViewer output is unchanged.

5. Record the phase and commit.

## Out of Scope

- Cubemap, irradiance, prefiltered environment, or BRDF LUT descriptor bindings.
- Sampling HDR/IBL textures in `DefaultLit.hlsl`.
- Generating GPU IBL convolution passes.
- Making `SkyboxPass` draw.
- Connecting `SkyboxComponent` to `ViewData`.
- TAA or additional post-process integration.
- Changing ModelViewer default visual output.

## Acceptance Criteria

- `DefaultLit.hlsl` no longer depends on hidden hard-coded ambient strengths for IBL-like ambient.
- `ViewData` exposes explicit IBL-approximate ambient settings with defaults matching the previous shader output.
- `PipelineCache::UpdateViewConstants()` uploads those values.
- Tests cover default/custom upload paths, C++/HLSL layout guardrails, and shader-source guardrails.
- `PipelineCacheValidation`, `RenderPassValidation`, `RenderSceneValidation`, `ModelViewerSmoke`, and `VisualGoldenValidation` pass.
- Spark plan review and Spark code review have no blockers before moving to the next stage.

## Validation Plan

```powershell
cmake --build build/win_x64_debug --config Debug --target PipelineCacheValidation RenderPassValidation RenderSceneValidation ModelViewer VisualGoldenValidation ImageCompareValidation
build\win_x64_debug\Tests\Debug\PipelineCacheValidation.exe
build\win_x64_debug\Tests\Debug\RenderPassValidation.exe
ctest --test-dir build/win_x64_debug -C Debug --output-on-failure -R "PipelineCacheValidation|RenderPassValidation|RenderSceneValidation|ModelViewerSmoke|VisualGoldenValidation|ImageCompareValidation"
ctest --test-dir build/win_x64_debug -C Debug --output-on-failure -R "ClusteredLightingValidation|RenderGraphValidation|RenderHonestyValidation|RenderSceneValidation|RenderPassValidation|MaterialSystemValidation|ResourceInstantiationValidation|PipelineCacheValidation|ModelViewerSmoke|VisualGoldenValidation|ImageCompareValidation"
git diff --check
```

## Risks

- Cbuffer packing mismatch between C++ and HLSL.
  - Mitigation: append packed `Vec4` fields and add upload tests.
- Default visual output drift.
  - Mitigation: default values mirror old shader constants and visual golden must pass.
- Overclaiming IBL completeness.
  - Mitigation: phase name and comments call this an IBL-approximate ambient minimum path, not cubemap/prefilter IBL.
