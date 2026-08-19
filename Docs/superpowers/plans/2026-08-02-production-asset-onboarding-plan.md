# Production Asset Onboarding Plan

**Status:** Stages 1-2 implemented; Stage 3 external DX12 tests registered and
awaiting native execution
**Date:** 2026-08-02
**Scope:** Runtime asset ingestion and validation for engine-core development;
Editor workflows remain out of scope

## Objective

Turn a developer model library into explicit, reproducible engine evidence
without coupling normal builds to hundreds of megabytes of third-party content.
The pipeline must distinguish five outcomes:

1. source file and dependencies are present;
2. the importer understands every required feature;
3. scene instantiation produces renderable primitives;
4. the active camera sees submitted work;
5. direct and GPU-driven paths produce equivalent output.

No earlier outcome implies a later one.

## Current Evidence

- The developer library contains 9 glTF/GLB models, 114 textures, and about
  454.6 MB of texture data.
- All seven external `.gltf` packages have complete referenced buffers/images.
- DamagedHelmet renders through the DX12 direct path with one draw.
- Porsche and Spartan parsed and instantiated before this change, but both
  produced the same background-only capture and zero visible/direct draws under
  ModelViewer's fixed camera.
- Porsche is the best current static stress asset: CC-BY-4.0, 75 meshes,
  14 materials, 75 static mesh nodes, and 27 textures. Its 30-file,
  74,214,810-byte source package has aggregate manifest SHA-256
  `54f715c0b6c12fc39326476f4cf6794c9720857fbaccde57326ad5e43c46b381`
  (SHA-256 of sorted `relative-path:file-sha256` lines).
- Spartan is a useful future skeletal stress asset: CC-BY-4.0, 10 meshes,
  9 materials, and 9 skinned mesh nodes.

## Runtime Feature Matrix

| Feature | Current status | Required behavior |
|---|---|---|
| glTF/GLB buffers, images, base metallic-roughness PBR | Supported | Qualification baseline |
| OPAQUE / MASK / BLEND material routing | Imported | Validate each pass independently |
| Nested node transforms | Imported | Include in camera bounds and parity tests |
| `KHR_materials_clearcoat`, `KHR_materials_specular`, `KHR_materials_transmission` | Not consumed | Emit explicit optional-feature degradation; use base PBR fallback |
| `KHR_materials_pbrSpecularGlossiness` when required | Not consumed | Fail import as unsupported-required, never silently render with wrong material semantics |
| glTF skins and animations | Not wired into ModelResource instantiation | Report unsupported/deferred; implement before using skeletal assets as qualification evidence |
| Cooked texture compression, mip generation, mesh optimization, streaming | Not yet production-ready | Derived-data pipeline; source assets remain immutable |

## Implementation Stages

### Stage 1: Asset governance and audit - implemented

- Standardize `assets/models/<asset-id>` and adjacent `license.txt` layout.
- Inventory mesh/material/node counts, alpha modes, extensions, skins,
  animations, dependencies, total size, and attribution.
- Keep large downloaded content as an external developer/CI library by default.
- Reject qualification candidates without redistribution and attribution data.

### Stage 2: Deterministic inspection - implemented

- Add reusable, aspect-aware bounds camera framing.
- Interactive ModelViewer defaults to automatic fit; smoke mode defaults to the
  existing fixed camera so old goldens do not change silently.
- Add explicit `--camera-fit auto` for real-asset smoke tests.
- Add final-frame `--expect-model-visible` and multi-batch GPU-driven readiness
  assertions.
- Report primitive counts, renderable static meshes, world bounds, target,
  distance, and clip planes.
- Cover framing math with CPU-only tests for landscape/portrait, very small and
  very large unit scales, and malformed bounds.

### Stage 3: External real-asset regression - registered, execution pending

- Enable with `RVX_ENABLE_EXTERNAL_ASSET_TESTS=ON` and
  `RVX_EXTERNAL_TEST_ASSET_ROOT=<asset-root>`.
- Fail CMake configuration if the selected model or its license is missing.
- Run Porsche forced-direct and forced-GPU DX12 smokes with automatic fitting,
  visible-object assertions, multi-batch indirect assertions, Debug Layer, and
  zero-tolerance cross-path parity.
- Keep the tests labeled `external-assets;qualification-candidate`; they do not
  close the production manifest gate.
- Record adapter/driver, source content hash, capture hashes, timing, peak
  decoded bytes, and parity metrics before treating Stage 3 as complete.

### Stage 4: Import honesty and capability diagnostics

- Add a structured import report containing required/optional extensions,
  supported/degraded/rejected features, primitive/material counts, and source
  dependency hashes.
- Reject unknown required extensions and required specular-glossiness before
  resource publication.
- Warn once per asset for ignored optional extensions and expose the warning in
  tool artifacts, not only transient logs.
- Add fixtures for required-extension rejection and optional-extension base-PBR
  degradation.

### Stage 5: Hermetic qualification asset

- Author or optimize a small CC0/CC-BY asset with multiple meshes/materials,
  OPAQUE and MASK primitives, textures, and nested transforms.
- Check the asset, attribution, and stable source hash into the repository.
- Add DX12 direct/GPU goldens, GBV, repeated-frame/resize, and parity gates.
- Close `RealAssetRegression` only after this checked-in suite passes.

### Stage 6: Cooked asset pipeline

- Introduce immutable source assets, versioned import settings, content-addressed
  derived data, and platform/backend-neutral cooked metadata.
- Generate mip chains and GPU-native texture payloads; avoid decoding all source
  images to large RGBA staging allocations at first render.
- Add mesh index/vertex optimization, bounds validation, optional LOD data, and
  deterministic dependency manifests.
- Separate CPU parse/decode, asynchronous IO, copy-queue upload, residency, and
  render-ready publication with memory/time diagnostics.

### Stage 7: Skeletal and extended material support

- Wire glTF skins, inverse bind matrices, skeleton hierarchy, animation clips,
  Animator/Skeleton components, and GPU skinning palette limits.
- Define GPU-driven behavior for skinned primitives explicitly: supported
  indirect path, dedicated skinned path, or honest direct fallback.
- Add clearcoat/specular/transmission in priority order after base PBR fidelity
  is stable; retain required-extension fail-closed behavior.

### Stage 8: Cross-backend asset matrix

- Reuse the same source/cooked asset hashes and camera framing on DX12, Vulkan,
  and Metal first, then OpenGL/DX11 fallback paths.
- Track backend qualification independently. A DX12 asset pass must not promote
  Vulkan or Metal.
- Store adapter, driver, backend, validation mode, image metrics, and resource
  diagnostics in machine-readable reports.

## Immediate Acceptance Criteria

The current implementation slice is ready when:

- ModelViewer and `ModelCameraFramingValidation` build;
- all CPU framing tests pass;
- existing GPU-driven unit/multi-batch tests remain green;
- the three optional Porsche tests register only when explicitly enabled;
- a native post-change run proves Porsche has visible objects, multiple
  indirect batches/draws, a working direct fallback, and direct/GPU parity.

The last item remains the only open acceptance item for Stage 3 because the
local GUI execution approval quota blocked the post-change DX12 launch.
