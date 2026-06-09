# RQ4 - PBR Material Texture Visual Gate Plan

Date: 2026-06-10
Parent program: render-quality continuation after RQ3d
Previous stage: RQ3d - Directional Shadow Visual Gate

## 1. Stage Decision

Add a deterministic GPU visual gate proving that the current PBR material texture path is actually used by ModelViewer. RQ4 should validate base color, normal, metallic-roughness, occlusion, and emissive texture wiring with a small controlled fixture and an explicit material-ready smoke assertion.

This is a verification-and-framework stage for material visual quality. It does not redesign the BRDF, add new material extensions, or change the default ModelViewer/DamagedHelmet path.

## 2. Current Engine Evidence

- `DefaultLit.hlsl` already samples base color, normal, metallic-roughness, occlusion, emissive, IBL, and directional shadow inputs.
- `MaterialSystem` resolves material slots into descriptor bindings 1-5 and tracks texture flags in `MaterialGPUConstants`.
- `GLTFImporter` marks base color/emissive as sRGB color textures and normal/metallic-roughness/occlusion as non-sRGB data/normal textures.
- `TextureLoader` can load embedded glTF image data through `TextureReference`, so a self-contained material fixture is feasible.
- `SceneRenderer::GetMaterialSystem()` exposes `MaterialSystem::GetLastBindingResult()`, allowing ModelViewer smoke mode to fail when material textures fall back silently.
- `MaterialBindingResult` currently does not expose the final material texture flags or material identity; RQ4 must extend this before `--expect-material-ready` can be an honest gate.
- Current unit tests validate many material binding details, but there is no GPU visual golden proving that PBR texture maps affect the final ModelViewer frame.

## 3. Scope

1. Add a deterministic PBR material fixture.
   - Add `Tests/Fixtures/ModelViewer/PBRMaterialSwatch.gltf`.
   - Use embedded geometry and embedded tiny images/data URIs; do not depend on external texture files.
   - Keep the fixture opaque, single-draw, and single-material named `RQ4PBRMaterial` so `MaterialSystem::GetLastBindingResult()` is a meaningful final-frame readiness check.
   - Include tangents in the mesh so normal mapping does not depend on importer tangent generation.
   - Include baseColor, normal, metallicRoughness, occlusion, and emissive textures with obvious low-resolution patterns.

2. Add ModelViewer material gate options.
   - Add `--material-test-scene` to configure deterministic camera and a non-shadow directional light suitable for the swatch.
   - Extend `MaterialBindingResult` so the final binding exposes at least:
     - the resolved `textureFlags`
     - the bound material name or equivalent stable material identity
   - Add `--expect-material-ready` to fail smoke mode unless the final material binding result is ready:
     - `status == MaterialBindingStatus::Ready`
     - `usedFallback == false`
     - `constantsUpdated == true`
     - `descriptorSet != nullptr`
     - all five texture flags are present: baseColor, normal, metallicRoughness, occlusion, emissive
     - when `--material-test-scene` is active, the final material identity is `RQ4PBRMaterial`
   - Log the final material binding status/message on failure and log a success line on pass.
   - Require `--expect-material-ready` to be used with `--material-test-scene` in this stage so the assertion is tied to the controlled fixture, not an arbitrary last material in a scene.
   - Do not change existing default ModelViewer, DamagedHelmet, R7, IBL, or shadow smoke behavior.

3. Add a PBR material visual CTest gate.
   - Add `ModelViewerPBRMaterialSmoke` with:
     - `--smoke`
     - `--model Tests/Fixtures/ModelViewer/PBRMaterialSwatch.gltf`
     - `--material-test-scene`
     - `--expect-material-ready`
     - DX11, fixed resolution, fixed frame count, screenshot output.
   - Add `PBRMaterialVisualGoldenValidation`, dependent on `ModelViewerPBRMaterialSmoke`.
   - Prefer zero tolerance; if the DX11 Debug output is not exact-stable, document the measured diff and use the smallest explicit tolerance.

4. Store the inspected PBR material golden.
   - Capture `Tests/Golden/ModelViewer/RQ4_PBRMaterial_DX11_320x180.ppm`.
   - Commit only after inspecting that the image shows visible material texture variation, not just a flat shaded quad.
   - Inspection must explicitly confirm visible base-color patterning, normal-map lighting perturbation, metallic/roughness region contrast, occlusion darkening, and an emissive region.

5. Keep existing gates green.
   - Existing ModelViewer visual gates, shadow visual gate, and focused material/render unit tests must keep passing.

## 4. Out of Scope

- BRDF replacement, energy-compensation changes, clearcoat, sheen, transmission, anisotropy, subsurface, parallax, decals, virtual texturing.
- glTF texture transform and multiple UV set support.
- Runtime sampler-state correctness beyond existing loader metadata.
- DX12/Vulkan visual golden capture.
- DamagedHelmet default camera or lighting changes.
- ECS/Object/RenderProxy refactors.

## 5. Expected Files

- `Docs/superpowers/specs/2026-06-10-rq4-pbr-material-visual-gate-plan.md`
- `Docs/superpowers/specs/phase-log.md`
- `Render/Include/Render/Material/MaterialSystem.h`
- `Render/Private/Material/MaterialSystem.cpp`
- `Samples/ModelViewer/main.cpp`
- `Tests/CMakeLists.txt`
- `Tests/Fixtures/ModelViewer/PBRMaterialSwatch.gltf`
- `Tests/Golden/ModelViewer/RQ4_PBRMaterial_DX11_320x180.ppm`
- `Tests/MaterialSystemValidation/main.cpp`

## 6. Required Tests

- `ModelViewerPBRMaterialSmoke` passes and writes the PBR swatch screenshot.
- `PBRMaterialVisualGoldenValidation` passes.
- `MaterialSystemValidation` covers that `MaterialBindingResult` reports final texture flags and material identity for ready bindings.
- `ModelViewerPBRMaterialSmoke` uses `--no-ibl` to keep the gate focused on direct material texture wiring.
- Existing `ModelViewerSmoke`, `VisualGoldenValidation`, `ModelViewerShadowSmoke`, `ShadowVisualGoldenValidation`, and `ModelViewerIBLSmoke` pass.
- `MaterialSystemValidation`, `PipelineCacheValidation`, `RenderPassValidation`, and `RenderSceneValidation` pass.
- `git diff --check` passes.

## 7. Validation Plan

```powershell
$B = "build/win_x64_debug"
cmake --build $B --config Debug --target ModelViewer VisualGoldenValidation MaterialSystemValidation PipelineCacheValidation RenderPassValidation RenderSceneValidation
ctest --test-dir $B -C Debug --output-on-failure -R "ModelViewerPBRMaterialSmoke|PBRMaterialVisualGoldenValidation"
ctest --test-dir $B -C Debug --output-on-failure -R "ModelViewerSmoke|VisualGoldenValidation|ModelViewerShadowSmoke|ShadowVisualGoldenValidation|ModelViewerIBLSmoke"
ctest --test-dir $B -C Debug --output-on-failure -R "MaterialSystemValidation|PipelineCacheValidation|RenderPassValidation|RenderSceneValidation"
git diff --check
```

## 8. Risks

- A visual golden can pass while textures silently fall back if the smoke mode only compares pixels. RQ4 must assert the `MaterialSystem` final binding result and all five PBR texture flags.
- `GetLastBindingResult()` only represents the last prepared material; the fixture must stay single-draw/single-material for this stage, and the smoke check must verify the final material identity is `RQ4PBRMaterial`.
- Normal-map visibility depends on light/camera/tangent correctness; the fixture should include tangents and an obvious normal pattern.
- Embedded image handling must be validated through ModelViewer, not assumed from unit tests alone.
- Golden images are backend- and config-specific; RQ4 intentionally starts with DX11 Debug only.

## 9. Acceptance Criteria

- A committed deterministic PBR material fixture and DX11 golden exist.
- CTest has a material smoke test and PBR material golden test.
- The material smoke fails if final material binding uses fallback resources, does not update constants, does not bind `RQ4PBRMaterial`, or does not report all five PBR texture flags.
- Existing visual and material/render unit gates remain green.
- Spark plan review and Spark code review have no blockers before commit.

## 10. Operating Rule For This Stage

Before implementation starts, reread this document and use only RQ4 scope as the task list. Do not expand into BRDF redesign, KHR material extensions, texture transforms, multi-backend screenshot capture, or general ModelViewer redesign.

## 11. Spark Review

- Plan review: PASS (`gpt-5.5`, xhigh; agent Pauli `019ead8c-e21b-78c3-add0-12e95fc7bb43`)
- Code review: PASS (`gpt-5.5`, xhigh; agent Pauli `019ead8c-e21b-78c3-add0-12e95fc7bb43`)
- Commit message: `test(render): add pbr material visual gate`
