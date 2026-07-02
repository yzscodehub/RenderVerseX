# RQ8 - Tangent Basis Robustness And Normal-Map Honesty Plan

Date: 2026-06-10
Parent program: render-quality continuation after RQ7
Previous stage: RQ7 - Object Normal Matrix Main-Path Correction

## 1. Stage Decision

Make the normal-map path honest and robust. The renderer already samples tangent-space normal maps in
`DefaultLit.hlsl`, and the glTF importer already attempts to generate missing tangents. However, the current path still
has two quality risks:

- `Mesh::GenerateTangents()` can normalize a zero tangent when UVs are degenerate, producing invalid tangent data.
- Opaque/transparent rendering can bind a material with a normal map even when the mesh has no complete tangent basis.

RQ8 keeps this narrow: harden tangent generation, expose mesh attribute availability from GPU uploads, and make material
binding clear the normal-map flag when the draw mesh cannot support tangent-space normal mapping. This improves PBR
reliability without introducing MikkTSpace, skinning, instancing, GBuffer, SSAO, TAA, or BRDF changes.

## 2. Current Engine Evidence

- `DefaultLit.hlsl` expects separate vertex buffers: position, normal, UV, and tangent.
- `DefaultLit.hlsl` samples normal maps only when `MATERIAL_TEXTURE_NORMAL` is set.
- `GLTFImporter::ConvertPrimitive()` calls `mesh->GenerateTangents()` when tangents are missing and normals/UVs exist.
- `Mesh::GenerateTangents()` skips degenerate UV triangles but later normalizes accumulated tangents without a finite
  fallback.
- `GPUResourceManager::UploadMesh()` tracks `hasNormals`, `hasUVs`, and `hasTangents` internally, but
  `MeshGPUBuffers` only exposes raw buffer pointers.
- `OpaquePass` and `TransparentPass` call `MaterialSystem::PrepareMaterialBinding()` without mesh attribute context.
- `MaterialSystem::ResolveMaterialTextures()` sets `HasNormal` when the normal texture is GPU-ready, independent of
  whether the mesh can consume tangent-space normal maps.
- `GLTFImporter::ConvertPrimitive()` leaves primitives with no `indices` accessor unindexed; later GPU upload rejects
  meshes with empty index data.

## 3. Scope

1. Harden tangent generation.
   - Add safe normalization/fallback logic inside `Mesh::GenerateTangents()`.
   - Vertices with invalid or zero accumulated tangents should receive a deterministic tangent orthogonal to the normal.
   - Tangent handedness should default to `+1` when bitangent data is invalid.
   - The generated tangent attribute must contain finite values.

2. Support non-indexed glTF triangle primitives.
   - When a glTF primitive has no `indices` accessor and is triangle-list topology, synthesize sequential `uint32`
     indices from vertex count.
   - This keeps existing indexed draw paths and enables normal/tangent generation plus GPU upload for valid unindexed
     triangle glTF assets.

3. Expose mesh attribute availability to render passes.
   - Add `hasNormals`, `hasUVs`, and `hasTangents` to `MeshGPUBuffers`.
   - Populate them in `GPUResourceManager::GetMeshBuffers()`.
   - Add a small helper such as `HasNormalMapTangentBasis()` that returns true only when normal, UV, and tangent buffers
     are all available.

4. Add material normal-map honesty options.
   - Add `MaterialBindingOptions` with `allowNormalMap = true` by default.
   - Extend `MaterialSystem::PrepareMaterialBinding()` and internal resolve/build paths to accept those options.
   - If `allowNormalMap == false` and the material has a normal texture, bind/use the default normal path, clear
     `MaterialTextureFlags::HasNormal`, mark `fallbackTextureFlags` with `HasNormal`, and return a visible fallback
     status/message.
   - If `allowNormalMap == false` but the material omitted a normal texture, do not mark fallback and do not set
     `fallbackTextureFlags::HasNormal`; omitted optional textures must remain distinct from explicit fallback.
   - Preserve default `allowNormalMap == true` behavior: a ready normal texture still sets `HasNormal` and does not
     become fallback.
   - Preserve existing default behavior for callers that do not pass options.

5. Wire main draw passes.
   - `OpaquePass` and `TransparentPass` pass material binding options derived from `buffers.HasNormalMapTangentBasis()`.
   - Depth and shadow passes do not need material normal-map options.

6. Documentation and phase log.
   - Record the stage in `Docs/superpowers/specs/phase-log.md`.

## 4. Out of Scope

- MikkTSpace integration, tangent-space sign compatibility audits beyond the current shader convention, skinning,
  morph targets, GPU instancing, GBuffer/SSAO/TAA/SSR, shader BRDF changes, texture compression, material variant
  explosion, and visual golden recapture unless a visual diff appears.

## 5. Expected Files

- `Docs/superpowers/specs/2026-06-10-rq8-tangent-basis-normal-map-honesty-plan.md`
- `Docs/superpowers/specs/phase-log.md`
- `Scene/Private/Mesh.cpp`
- `Resource/Private/Importer/GLTFImporter.cpp`
- `Render/Include/Render/GPUResourceManager.h`
- `Render/Private/GPUResourceManager.cpp`
- `Render/Include/Render/Material/MaterialSystem.h`
- `Render/Private/Material/MaterialSystem.cpp`
- `Render/Private/Passes/OpaquePass.cpp`
- `Render/Private/Passes/TransparentPass.cpp`
- `Tests/ResourceInstantiationValidation/main.cpp`
- `Tests/GPUResourceManagerValidation/main.cpp`
- `Tests/MaterialSystemValidation/main.cpp`
- `Tests/RenderPassValidation/main.cpp`

## 6. Required Tests

- `Mesh::GenerateTangents()` handles degenerate UV triangles with finite tangent output, a reasonable unit length,
  near-orthogonality to the vertex normal, and finite handedness; when bitangent data is invalid, `w` defaults to `+1`.
- `GLTFImporter` synthesizes indices for a valid non-indexed triangle primitive and can generate tangents.
- `GPUResourceManager::GetMeshBuffers()` exposes `hasNormals`, `hasUVs`, and `hasTangents`, and the tangent-basis helper
  reports correctly.
- `MaterialSystem::PrepareMaterialBinding()` with `allowNormalMap=false` clears `HasNormal`, reports fallback, and marks
  `fallbackTextureFlags`.
- `MaterialSystem::PrepareMaterialBinding()` with `allowNormalMap=false` and no normal texture does not mark fallback and
  does not set `fallbackTextureFlags::HasNormal`.
- `MaterialSystem::PrepareMaterialBinding()` default options keep a ready normal texture enabled with `HasNormal` and no
  fallback.
- Opaque/transparent pass source or behavior guard proves material binding options are derived from
  `buffers.HasNormalMapTangentBasis()`.
- Focused regression gates remain green:
  - `ResourceInstantiationValidation`
  - `GPUResourceManagerValidation`
  - `MaterialSystemValidation`
  - `RenderPassValidation`
  - `PipelineCacheValidation`
  - `RenderSceneValidation`
- ModelViewer visual smoke/golden gates pass if practical:
  - `ModelViewerSmoke`
  - `VisualGoldenValidation`
  - `ModelViewerPBRMaterialSmoke`
  - `PBRMaterialVisualGoldenValidation`

## 7. Validation Plan

```powershell
$B = "build/win_x64_debug"
cmake --build $B --config Debug --target ResourceInstantiationValidation GPUResourceManagerValidation MaterialSystemValidation RenderPassValidation PipelineCacheValidation RenderSceneValidation ModelViewer VisualGoldenValidation
ctest --test-dir $B -C Debug --output-on-failure -R "ResourceInstantiationValidation|GPUResourceManagerValidation|MaterialSystemValidation|RenderPassValidation|PipelineCacheValidation|RenderSceneValidation"
ctest --test-dir $B -C Debug --output-on-failure -R "ModelViewerSmoke|VisualGoldenValidation|ModelViewerPBRMaterialSmoke|PBRMaterialVisualGoldenValidation"
git diff --check
```

## 8. Risks

- Clearing normal-map flags per draw can increase descriptor/cache variants if implemented by mutating descriptors
  unnecessarily. Keep the option in material binding and reuse the default normal view when normal maps are disabled.
- Some assets may have normal maps but no tangents; they should still render with vertex normals rather than failing the
  draw.
- Sequential indices should only be synthesized for triangle-list primitives in this stage, because render passes do not
  currently vary pipeline topology per mesh primitive.
- Robust tangent fallback improves stability but is not a MikkTSpace replacement.

## 9. Acceptance Criteria

- No generated tangents contain NaN/Inf on degenerate UV input.
- Non-indexed glTF triangle primitives become drawable indexed meshes.
- Normal-map sampling is disabled visibly when a mesh lacks a complete tangent basis.
- Existing render and ModelViewer gates pass.
- Spark plan review and Spark code review have no blockers before commit.

## 10. Operating Rule For This Stage

Before implementation starts, reread this document and use only RQ8 scope as the task list. Do not expand into
MikkTSpace, GBuffer, SSAO/TAA/SSR, BRDF, skinning, or ECS/RenderProxy work.

## 11. Spark Review

- Plan review: PASS (`gpt-5.5`, xhigh; agent Pauli `019ead8c-e21b-78c3-add0-12e95fc7bb43`)
- Code review: PASS (`gpt-5.5`, xhigh; agent Pauli `019ead8c-e21b-78c3-add0-12e95fc7bb43`)
- Commit message: `fix(render): disable normal maps without tangent basis`
