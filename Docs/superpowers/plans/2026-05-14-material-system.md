# Material System Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Keep the checklist updated as work completes.

**Goal:** Build a long-term split descriptor set material system for RenderVerseX so glTF-style PBR materials are represented once in engine data, uploaded through a dedicated material path, and bound independently from frame and object state.

**Architecture:** The renderer will use three stable descriptor sets: set 0 for frame/view/lighting resources, set 1 for per-object resources, and set 2 for per-material resources. `PipelineCache` remains responsible for compiling pipelines and owning the first phase frame/object constant buffers. A new `MaterialSystem` owns material GPU constants, texture descriptor sets, default fallback resources, and descriptor cache invalidation. Render passes bind frame, object, and material sets explicitly.

**Tech Stack:** C++20, Render module, RHI descriptor abstraction, HLSL register spaces, standalone source validation tests, ModelViewer smoke test.

---

## Descriptor Layout Target

Set 0: Frame
- `b0`: `ViewConstants`
- Future expansion: light constants, shadow maps, IBL textures, global samplers

Set 1: Object
- `b0`: `ObjectConstants` with dynamic offset

Set 2: Material
- `b0`: `MaterialConstants` with dynamic offset
- `t1`: base color texture
- `t2`: normal texture
- `t3`: metallic-roughness texture
- `t4`: occlusion texture
- `t5`: emissive texture
- `s6`: material sampler

Texture/sampler bindings intentionally avoid `0` because the current cross-backend
RHI descriptor abstraction uses a unified binding namespace, matching Vulkan.
DX12 register namespaces could reuse `b0/t0/s0`, but the portable layout should
not rely on that backend-specific distinction.

The first implementation keeps the existing forward pass and expands from Lambert/base-color to basic metallic-roughness material binding. Full BRDF, IBL, alpha sorting, clearcoat, transmission, and material variants are intentionally staged after descriptor ownership is correct.

## Implementation Tasks

- [x] Add source validation that requires split descriptor set binding in `OpaquePass`, `DepthPrepass`, and shader register spaces.
- [x] Add source validation that rejects same-frame dynamic constant cursor wrapping.
- [x] Update `DefaultLit.hlsl` to use explicit register spaces for frame, object, and material sets.
- [x] Extend `MaterialGPUConstants` to include PBR factors, texture flags, alpha mode, alpha cutoff, and workflow fields with constant-buffer-safe alignment.
- [x] Add `Render::MaterialSystem` public header and implementation.
  - Converts `Resource::MaterialResource` / `Scene::Material` data to `MaterialGPUConstants`.
  - Resolves texture views through `GPUResourceManager` and `ResourceViewCache`.
  - Uses default white/flat-normal/black fallback textures when slots are missing or not resident.
  - Caches descriptor sets by material id and texture view generation.
  - Exposes `BeginFrame()`, `UpdateMaterialConstants()`, and `GetOrCreateMaterialSet()`.
- [x] Refactor `PipelineCache` descriptor setup.
  - Create explicit frame, object, and material set layouts.
  - Expose frame/object descriptor sets and dynamic offset helpers.
  - Stop owning material texture descriptor sets.
- [x] Wire `MaterialSystem` through `SceneRenderer` and render passes.
  - `OpaquePass`: bind set 0 once, set 1 per object, set 2 per submesh material.
  - `DepthPrepass`: bind set 0 and set 1 only.
  - `TransparentPass`: use the same material path as opaque where applicable.
  - `ShadowPass`: bind frame/object data only unless the shader needs alpha masking.
- [x] Update texture residency transitions to cover all material textures used by a visible submesh.
- [x] Run validation and smoke tests.
  - Build `RenderPassBindingValidation`.
  - Run `RenderPassBindingValidation`.
  - Build `ModelViewer`.
  - Run `ModelViewer` and capture the application window to confirm the model renders.

## Validation Notes

The minimum acceptable first milestone is: ModelViewer still renders DamagedHelmet, the material path binds five texture slots with fallbacks, and no pass relies on the old single-set material descriptor layout.

If a backend rejects split sets, fix the backend descriptor binding implementation rather than collapsing the material system back to one set. DX12, Vulkan, and DX11 should all treat set index as a first-class part of the binding contract.
