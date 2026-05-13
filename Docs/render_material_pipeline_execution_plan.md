# Render Material Pipeline Execution Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make ModelViewer's material rendering path stable enough for real glTF base-color textures and material factors, while keeping resource lifetime and validation under control.

**Architecture:** Keep the current `SceneRenderer -> OpaquePass -> PipelineCache -> GPUResourceManager` shape. Add small, explicit APIs for texture state/lifetime and material constants instead of introducing a large material system rewrite in this pass.

**Tech Stack:** C++20, CMake/MSBuild, RenderVerseX RHI, HLSL, standalone validation executables.

---

## Phase 1: Descriptor And View Lifetime Hardening

**Files:**
- Modify: `Render/Include/Render/PipelineCache.h`
- Modify: `Render/Private/PipelineCache.cpp`
- Modify: `Render/Private/Passes/OpaquePass.cpp`

- [x] Add a public `PipelineCache::ClearMaterialDescriptorSets()` method.
- [x] Use it before any path that may invalidate texture views used by material descriptor sets.
- [x] Keep descriptor set caching keyed by `RHITextureView*` for now because `ResourceViewCache` owns view creation.
- [x] Verify `ModelViewer` still builds.

## Phase 2: Material Base Color Constants

**Files:**
- Modify: `Render/Include/Render/PipelineCache.h`
- Modify: `Render/Private/PipelineCache.cpp`
- Modify: `Render/Private/Passes/OpaquePass.cpp`
- Modify: `Render/Shaders/DefaultLit.hlsl`

- [x] Add `MaterialConstants` with `Vec4 baseColorFactor`.
- [x] Add a material constant upload buffer and bind it at `b4`.
- [x] Add `PipelineCache::UpdateMaterialConstants(const Vec4&)`.
- [x] Update `OpaquePass` before each submesh draw using the submesh material's base color.
- [x] Multiply sampled base color by `BaseColorFactor` in `DefaultLit.hlsl`.
- [x] Verify `ModelViewer` still builds.

## Phase 3: Validation Coverage

**Files:**
- Modify: `Tests/GPUResourceManagerValidation/main.cpp`

- [x] Keep the texture transition regression test.
- [x] Add focused coverage for descriptor/material helper behavior when practical without a real backend.
- [x] Run `GPUResourceManagerValidation.exe`.

## Phase 4: Runtime Visual Verification

**Commands:**
- `cmake --build build\Debug --config Debug --target ModelViewer`
- `build\Debug\Samples\ModelViewer\Debug\ModelViewer.exe`

- [x] Launch ModelViewer locally.
- [ ] Confirm the model is still visible.
- [ ] Confirm base-color textures and base-color factors affect final shading.
- [x] If runtime launch cannot be automated safely, report exact build/test evidence and the remaining manual visual check.

## Phase 5: Next Optimization Batch

**Candidate work after this plan:**
- Only prefetch texture slots currently consumed by `DefaultLit.hlsl` (`albedo` at this stage).
- Add normal/metallic-roughness texture slots.
- Add descriptor invalidation integration between `GPUResourceManager`, `ResourceViewCache`, and `PipelineCache`.
- Reduce first-frame diagnostic logs once the render path is stable.
- Move object/material constants from single upload buffers to per-frame ring or frame resources.
