# RQ7 - Object Normal Matrix Main-Path Correction Plan

Date: 2026-06-10
Parent program: render-quality continuation after RQ6
Previous stage: RQ6 - Texture Mip Chain And Material Sampler Quality

## 1. Stage Decision

Use the render object's existing normal matrix in the main DefaultLit draw path. The engine already computes/carries `RenderObject::normalMatrix` and `RenderPrimitiveProxy::normalMatrix`, but `PipelineCache::UpdateObjectConstants()` uploads only `World`, and `DefaultLit.hlsl` transforms normals/tangents with `World`. That is visually wrong under non-uniform scale and makes normal maps less reliable.

RQ7 is a narrow correctness stage: extend object constants to include `NormalMatrix`, bind it through existing draw passes, and make `DefaultLit.hlsl` use it for normal transformation. This improves material lighting correctness without changing BRDF math, pass topology, ECS, or RenderProxy.

## 2. Current Engine Evidence

- `RenderObject` has `normalMatrix`.
- `RenderPrimitiveProxy` has `normalMatrix`.
- `RenderSceneCollector` already computes `glm::inverseTranspose(Mat4(Mat3(worldMatrix)))`.
- `RenderScene::ApplyProxySnapshot()` copies proxy normal matrices into render objects.
- `PipelineCache::ObjectConstants` currently contains only `Mat4 world`.
- `PipelineCache::UpdateObjectConstants(const Mat4& worldMatrix)` uploads only world.
- `OpaquePass`, `TransparentPass`, `DepthPrepass`, and `ShadowPass` all call `UpdateObjectConstants(obj.worldMatrix)`.
- `DefaultLit.hlsl` object cbuffer contains only `World` and uses `mul((float3x3)World, input.Normal)`.
- `PBRLit.hlsl` already has a proven pattern with `WorldInverseTranspose`.

## 3. Scope

1. Extend object constants.
   - Add `Mat4 normalMatrix` to `PipelineCache::ObjectConstants` after `world`.
   - Update `PipelineCache::UpdateObjectConstants()` to accept both world and normal matrices.
   - Keep a safe default/overload only if needed for non-render test helpers; draw passes should pass real object normal matrices.

2. Wire all existing draw pass call sites.
   - `OpaquePass`, `TransparentPass`, `DepthPrepass`, and `ShadowPass` pass `obj.normalMatrix` along with `obj.worldMatrix`.
   - Depth/shadow shaders may ignore the extra matrix; the cbuffer can be larger than the shader-visible fields as long as the first `World` matrix remains at offset 0.

3. Update `DefaultLit.hlsl`.
   - Add `float4x4 NormalMatrix` to the `ObjectConstants` cbuffer after `World`.
   - Transform vertex normals with `NormalMatrix`.
   - Continue transforming tangents with `World`, then let the pixel shader's TBN construction orthogonalize tangent against the normal as it already does.

4. Tests.
   - Add/extend `PipelineCacheValidation` layout guard:
     - `ObjectConstants` is standard layout.
     - `world` offset is 0.
     - `normalMatrix` offset is 64.
     - size is 128.
   - Add a `PipelineCacheValidation` upload test proving `UpdateObjectConstants(world, normalMatrix)` writes both matrices to the object constant buffer.
   - Add a guard that the object constant stride/range is derived from `AlignConstantBufferSize(sizeof(ObjectConstants))` after the size increase.
   - Add a shader source guard proving `DefaultLit.hlsl` declares `NormalMatrix` and uses it to transform `input.Normal`.
   - Add/extend `RenderPassValidation` or source guard proving draw passes call `UpdateObjectConstants(obj.worldMatrix, obj.normalMatrix)`.

5. Documentation and phase log.
   - Record the stage in `Docs/superpowers/specs/phase-log.md`.

## 4. Out of Scope

- Tangent generation/reconstruction for meshes missing tangents, MikkTSpace integration, normal-map handedness changes, skinning normal matrices, GPU instancing, object constant ring-buffer redesign, BRDF changes, visual golden recapture, or ECS/RenderProxy refactors.

## 5. Expected Files

- `Docs/superpowers/specs/2026-06-10-rq7-object-normal-matrix-plan.md`
- `Docs/superpowers/specs/phase-log.md`
- `Render/Include/Render/PipelineCache.h`
- `Render/Private/PipelineCache.cpp`
- `Render/Private/Passes/OpaquePass.cpp`
- `Render/Private/Passes/TransparentPass.cpp`
- `Render/Private/Passes/DepthPrepass.cpp`
- `Render/Private/Passes/ShadowPass.cpp`
- `Render/Shaders/DefaultLit.hlsl`
- `Tests/PipelineCacheValidation/main.cpp`
- `Tests/RenderPassValidation/main.cpp` if source/call-site guard is added there

## 6. Required Tests

- `PipelineCacheValidation` object constants layout and upload tests pass.
- `PipelineCacheValidation` or `RenderPassValidation` source guard proves `DefaultLit.hlsl` uses `NormalMatrix` for normals.
- Draw-pass call-site guard proves the real render passes pass `obj.normalMatrix`.
- Existing focused gates remain green:
  - `PipelineCacheValidation`
  - `RenderPassValidation`
  - `RenderSceneValidation`
  - `ModelViewerPBRMaterialSmoke`
  - `PBRMaterialVisualGoldenValidation`
- Extra ModelViewer visual gates pass if practical:
  - `ModelViewerSmoke`
  - `VisualGoldenValidation`
  - `ModelViewerIBLSmoke`
  - `ModelViewerShadowSmoke`
  - `ShadowVisualGoldenValidation`
- `git diff --check` passes.

## 7. Validation Plan

```powershell
$B = "build/win_x64_debug"
cmake --build $B --config Debug --target PipelineCacheValidation RenderPassValidation RenderSceneValidation ModelViewer VisualGoldenValidation
ctest --test-dir $B -C Debug --output-on-failure -R "PipelineCacheValidation|RenderPassValidation|RenderSceneValidation|ModelViewerPBRMaterialSmoke|PBRMaterialVisualGoldenValidation"
ctest --test-dir $B -C Debug --output-on-failure -R "ModelViewerSmoke|VisualGoldenValidation|ModelViewerIBLSmoke|ModelViewerShadowSmoke|ShadowVisualGoldenValidation"
git diff --check
```

## 8. Risks

- Object constant buffer size changes from 64 to 128 bytes. Tests must verify stride/upload alignment and HLSL packing.
- Depth/shadow shaders still declare only `World`; the extra CPU-side data must not move the first matrix offset.
- If any draw path keeps using the old one-argument call, normal matrix data will silently stay identity/default. Call-site guards should prevent this.
- Visual output may change for non-uniformly scaled assets. Existing goldens likely use simple transforms, so recapture should not be needed unless a visual diff is observed.

## 9. Acceptance Criteria

- Main DefaultLit shader normals are transformed by `NormalMatrix`.
- Object constants upload both world and normal matrices with verified layout.
- All current draw passes pass `obj.normalMatrix`.
- Focused render and ModelViewer visual gates pass.
- Spark plan review and Spark code review have no blockers before commit.

## 10. Operating Rule For This Stage

Before implementation starts, reread this document and use only RQ7 scope as the task list. Do not expand into tangent generation, skinning, instancing, BRDF changes, or pass architecture work.

## 11. Spark Review

- Plan review: PASS (`gpt-5.5`, xhigh; agent Pauli `019ead8c-e21b-78c3-add0-12e95fc7bb43`)
- Code review: PASS (`gpt-5.5`, xhigh; agent Pauli `019ead8c-e21b-78c3-add0-12e95fc7bb43`)
- Commit message: `fix(render): upload object normal matrices`
