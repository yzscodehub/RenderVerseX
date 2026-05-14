# Material System Phased Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a long-term RenderVerseX material system that renders glTF PBR materials through stable frame/object/material descriptor sets and validates each phase before moving forward.

**Architecture:** Keep `Scene::Material` as CPU semantic data and `Resource::MaterialResource` as the asset/dependency layer. Add a render-side `MaterialSystem` that owns GPU material constants, default textures, material descriptor caching, texture readiness fallback, and material binding. Split descriptors into set 0 frame, set 1 object, and set 2 material so render passes do not own material binding logic.

**Tech Stack:** C++20, CMake, RenderVerseX RHI, DX12/Vulkan/DX11 source validations, HLSL shaders, standalone validation executables, ModelViewer visual smoke test.

---

## Execution Contract

This plan is the source of truth. Each phase must be executed from this document, and task checkboxes should be updated as work completes.

Do not start a later phase until the current phase satisfies all of these gates:

- The phase-specific validation commands pass.
- `ModelViewer` is run when the phase is expected to affect visible rendering.
- If the phase is not expected to produce a visual difference yet, the listed non-visual validation must pass instead.
- A `GPT-5.5` review subagent with `reasoning_effort: xhigh` reviews the phase diff.
- A `GPT-5.3-Codex-Spark` review subagent with `reasoning_effort: xhigh` reviews the same phase diff.
- All blocking/high findings from both agents are fixed.
- The relevant validation commands are rerun after fixes.

Suggested review prompt after each phase:

```text
Review only the changes for Material System Phase N. Focus on correctness, RHI compatibility, lifetime hazards, descriptor/pipeline binding bugs, and missing validation. Report blocking/high issues first with file/line references. Do not suggest broad unrelated refactors.
```

ModelViewer visual verification procedure:

```powershell
cmake --build build\Debug --config Debug --target ModelViewer
E:\WorkSpace\RenderVerseX\build\Debug\Samples\ModelViewer\Debug\ModelViewer.exe
```

After launch, capture only the ModelViewer window, confirm the model is visible, and check `build\Debug\Samples\ModelViewer\Debug\RenderVerseX.log` for material upload/binding errors. Do not use full-screen capture.

---

## File Map

Create:

- `Render/Include/Render/Material/MaterialSystem.h` - render-side material manager public API.
- `Render/Private/Material/MaterialSystem.cpp` - material GPU constants, default textures, descriptor cache, binding implementation.
- `Tests/MaterialSystemValidation/main.cpp` - CPU/GPU material conversion and descriptor cache validation.

Modify:

- `Render/CMakeLists.txt` - add `MaterialSystem.cpp`.
- `Tests/CMakeLists.txt` - add `MaterialSystemValidation`.
- `Render/Include/Render/Material/MaterialGPUData.h` - expand GPU material constants and flags.
- `Render/Include/Render/PipelineCache.h` - expose long-term set layout/pipeline access and remove material descriptor ownership.
- `Render/Private/PipelineCache.cpp` - create set 0/1/2 layouts and material-compatible pipelines.
- `Render/Private/Renderer/SceneRenderer.cpp` - initialize/shutdown/material texture preload integration.
- `Render/Include/Render/Renderer/SceneRenderer.h` - own `MaterialSystem`.
- `Render/Private/Renderer/RenderFrameResourceBinder.cpp` - bind set 0 frame resources.
- `Render/Private/Passes/OpaquePass.cpp` - bind set 1 object and set 2 material through `MaterialSystem`.
- `Render/Private/Passes/DepthPrepass.cpp` - bind set 1 object only, use depth/material alpha policy as needed.
- `Render/Private/Passes/TransparentPass.cpp` - bind transparent material path.
- `Render/Shaders/PBRLit.hlsl` - primary PBR shader using set 0/1/2 mapping.
- `Render/Shaders/DefaultLit.hlsl` - keep as debug/simple shader or adapt to shared layout if necessary.
- `RHI_DX12/Private/DX12Pipeline.cpp` - verify register space/set mapping if required.
- `RHI_Vulkan/Private/VulkanPipeline.cpp` - verify descriptor set mapping if required.
- `RHI_DX11/Private/DX11Pipeline.cpp` and `RHI_DX11/Private/DX11CommandContext.cpp` - verify emulated set/binding mapping.

---

## Phase 1: Material GPU Data And Conversion

**Visible effect:** No required visual change. Validate through `MaterialSystemValidation`.

**Files:**

- Modify: `Render/Include/Render/Material/MaterialGPUData.h`
- Create: `Render/Include/Render/Material/MaterialSystem.h`
- Create: `Render/Private/Material/MaterialSystem.cpp`
- Modify: `Render/CMakeLists.txt`
- Create: `Tests/MaterialSystemValidation/main.cpp`
- Modify: `Tests/CMakeLists.txt`

- [ ] **Step 1: Add failing material conversion validation**

Add `Tests/MaterialSystemValidation/main.cpp` with tests that create a `Scene::Material`, set base color, metallic, roughness, normal scale, occlusion strength, emissive color/strength, alpha mode, and texture flags, then verify conversion into `MaterialGPUConstants`.

Run:

```powershell
cmake --build build_codex --config Release --target MaterialSystemValidation
```

Expected before implementation: target or symbols are missing.

- [ ] **Step 2: Expand `MaterialGPUConstants`**

Add fields needed by glTF PBR:

```cpp
Vec4 baseColorFactor;
float metallicFactor;
float roughnessFactor;
float normalScale;
float occlusionStrength;
Vec3 emissiveColor;
float emissiveStrength;
uint32 textureFlags;
uint32 alphaMode;
float alphaCutoff;
uint32 workflow;
```

Keep the structure 16-byte aligned. Add static assertions for size and alignment.

- [ ] **Step 3: Add `MaterialSystem::BuildGPUConstants`**

Implement a pure conversion path:

```cpp
MaterialGPUConstants BuildGPUConstants(const Resource::MaterialResource* materialResource) const;
```

This method must not touch RHI resources. It only converts CPU material data and marks texture flags based on material texture handles.

- [ ] **Step 4: Run validation**

Run:

```powershell
cmake --build build_codex --config Release --target MaterialSystemValidation
E:\WorkSpace\RenderVerseX\build_codex\Tests\Release\MaterialSystemValidation.exe
```

Expected: all material conversion tests pass.

- [ ] **Step 5: Subagent review gate**

Run both review agents on Phase 1 diff. Fix all blocking/high findings and rerun `MaterialSystemValidation`.

---

## Phase 2: Descriptor Set Layout Split

**Visible effect:** ModelViewer should still render at least the previous base-color-lit model. This phase is a structural refactor, so no PBR improvement is required yet.

**Files:**

- Modify: `Render/Include/Render/PipelineCache.h`
- Modify: `Render/Private/PipelineCache.cpp`
- Modify: `Render/Private/Renderer/RenderFrameResourceBinder.cpp`
- Modify: `Render/Private/Passes/OpaquePass.cpp`
- Modify: `Render/Private/Passes/DepthPrepass.cpp`
- Modify: `Render/Private/Passes/TransparentPass.cpp`
- Modify: `Render/Shaders/DefaultLit.hlsl`
- Modify: `Tests/RenderPassBindingValidation/main.cpp`

- [ ] **Step 1: Add failing binding layout tests**

Extend `RenderPassBindingValidation` to assert:

- set 0 contains view/frame bindings.
- set 1 contains object dynamic constants.
- set 2 contains material dynamic constants and material textures.
- object dynamic offsets are passed only with set 1.
- material dynamic offsets are passed only with set 2.

- [ ] **Step 2: Refactor pipeline layout creation**

Create three explicit descriptor set layouts in `PipelineCache` instead of one reflection-derived mixed set:

```text
set 0: b0 ViewConstants, optional b1 LightConstants, global samplers
set 1: b0 ObjectConstants dynamic
set 2: b0 MaterialConstants dynamic, t0-t4 material textures, s0 material sampler
```

Reflection can still validate compatibility, but it must not collapse all resources back into one set.

- [ ] **Step 3: Update shader register mapping**

Use register spaces to represent sets on DX12-compatible HLSL:

```hlsl
cbuffer ViewConstants : register(b0, space0)
cbuffer ObjectConstants : register(b0, space1)
cbuffer MaterialConstants : register(b0, space2)
Texture2D BaseColorMap : register(t0, space2)
SamplerState MaterialSampler : register(s0, space2)
```

Backends that do not expose native spaces must map through the existing RHI layout abstraction.

- [ ] **Step 4: Update pass binding calls**

`OpaquePass` should bind:

```text
ctx.SetDescriptorSet(0, frameSet)
ctx.SetDescriptorSet(1, objectSet, objectDynamicOffset)
ctx.SetDescriptorSet(2, materialSet, materialDynamicOffset)
```

At the end of Phase 2, material set may still bind only base color plus fallbacks.

- [ ] **Step 5: Run validation and ModelViewer**

Run:

```powershell
cmake --build build_codex --config Release --target RenderPassBindingValidation RVX_Render
E:\WorkSpace\RenderVerseX\build_codex\Tests\Release\RenderPassBindingValidation.exe
cmake --build build\Debug --config Debug --target ModelViewer
```

Launch ModelViewer and confirm the model is visible.

- [ ] **Step 6: Subagent review gate**

Run both review agents on Phase 2 diff. Fix all blocking/high findings and rerun validation plus ModelViewer smoke.

---

## Phase 3: Default Material Resources And Material Descriptor Cache

**Visible effect:** ModelViewer should still render. Visual PBR improvement is not required yet, but missing textures must use correct fallbacks.

**Files:**

- Modify: `Render/Include/Render/Material/MaterialSystem.h`
- Modify: `Render/Private/Material/MaterialSystem.cpp`
- Modify: `Render/Private/Renderer/SceneRenderer.cpp`
- Modify: `Render/Include/Render/Renderer/SceneRenderer.h`
- Modify: `Tests/MaterialSystemValidation/main.cpp`
- Modify: `Tests/ResourceViewCacheValidation/main.cpp` if generation invalidation coverage is needed.

- [ ] **Step 1: Add failing descriptor cache tests**

Add tests proving descriptor cache keys include:

- base color view
- normal view
- metallic-roughness view
- occlusion view
- emissive view
- sampler identity
- `ResourceViewCache` generation

Two materials that differ only in normal map must not share a material descriptor.

- [ ] **Step 2: Create default textures in `MaterialSystem`**

Create these GPU textures during initialization:

```text
White RGBA: base color and occlusion fallback
Black RGBA: emissive fallback
FlatNormal RGBA: 128,128,255,255
DefaultMetallicRoughness RGBA: R=255, G=255, B=0, A=255
```

Transition them to shader-resource state before use.

- [ ] **Step 3: Resolve material texture views**

Implement a `MaterialTextureSet` that resolves ready textures from `GPUResourceManager` and `ResourceViewCache`, with fallback to default views when textures are missing or still uploading.

- [ ] **Step 4: Cache material descriptor sets**

Descriptor cache key must include all five texture views and generation. Invalidation must clear stale descriptor sets when texture views are invalidated.

- [ ] **Step 5: Run validation and ModelViewer**

Run:

```powershell
cmake --build build_codex --config Release --target MaterialSystemValidation ResourceViewCacheValidation RVX_Render
E:\WorkSpace\RenderVerseX\build_codex\Tests\Release\MaterialSystemValidation.exe
E:\WorkSpace\RenderVerseX\build_codex\Tests\Release\ResourceViewCacheValidation.exe
cmake --build build\Debug --config Debug --target ModelViewer
```

Launch ModelViewer and confirm the model remains visible.

- [ ] **Step 6: Subagent review gate**

Run both review agents on Phase 3 diff. Fix all blocking/high findings and rerun validation plus ModelViewer smoke.

---

## Phase 4: PBR Shader Integration

**Visible effect:** ModelViewer should visibly improve: DamagedHelmet should use base color, metallic-roughness, normal, AO, and emissive inputs.

**Files:**

- Modify: `Render/Shaders/PBRLit.hlsl`
- Modify: `Render/Shaders/Include/BRDF.hlsli`
- Modify: `Render/Shaders/Include/Lighting.hlsli`
- Modify: `Render/Private/PipelineCache.cpp`
- Modify: `Render/Private/Passes/OpaquePass.cpp`
- Modify: `Tests/RenderPassBindingValidation/main.cpp`

- [ ] **Step 1: Add shader layout validation**

Extend validation to assert shader layout uses set 0/1/2 mapping and material textures t0-t4 in the material set.

- [ ] **Step 2: Switch opaque model path to `PBRLit.hlsl`**

Keep `DefaultLit.hlsl` available for debug/simple rendering, but use PBR shader for glTF model rendering.

- [ ] **Step 3: Implement material sampling**

In shader:

- base color uses sRGB texture data as provided by texture loader.
- metallic-roughness follows glTF packing: G roughness, B metallic.
- normal map uses tangent if available.
- AO modulates ambient term.
- emissive adds to final color.
- alpha mask discards below cutoff.

- [ ] **Step 4: Handle missing tangent fallback**

If a mesh does not provide tangent data, the material path must not crash. Either select a no-normal-map variant or use vertex normal fallback.

- [ ] **Step 5: Run validation and ModelViewer**

Run:

```powershell
cmake --build build_codex --config Release --target RenderPassBindingValidation RVX_Render
E:\WorkSpace\RenderVerseX\build_codex\Tests\Release\RenderPassBindingValidation.exe
cmake --build build\Debug --config Debug --target ModelViewer
```

Launch ModelViewer, capture the ModelViewer window only, and verify DamagedHelmet renders with detailed surface material rather than flat base-color-only shading.

- [ ] **Step 6: Subagent review gate**

Run both review agents on Phase 4 diff. Fix all blocking/high findings and rerun validation plus ModelViewer visual verification.

---

## Phase 5: Pipeline Variants And Render Queue Routing

**Visible effect:** Opaque materials should still render. Masked/transparent materials can be validated through synthetic tests if ModelViewer asset does not contain them.

**Files:**

- Modify: `Render/Include/Render/PipelineCache.h`
- Modify: `Render/Private/PipelineCache.cpp`
- Modify: `Render/Include/Render/Material/MaterialSystem.h`
- Modify: `Render/Private/Material/MaterialSystem.cpp`
- Modify: `Render/Private/Passes/OpaquePass.cpp`
- Modify: `Render/Private/Passes/TransparentPass.cpp`
- Modify: `Render/Private/Passes/DepthPrepass.cpp`
- Modify: `Tests/MaterialSystemValidation/main.cpp`
- Modify: `Tests/RenderPassBindingValidation/main.cpp`

- [ ] **Step 1: Add pipeline key tests**

Add tests for `MaterialPipelineKey`:

- opaque lit material maps to opaque pass.
- alpha mask material maps to masked pipeline.
- alpha blend material maps to transparent pass.
- double-sided material changes rasterizer cull mode.

- [ ] **Step 2: Implement `MaterialPipelineKey`**

Key fields:

```cpp
MaterialDomain domain;
MaterialShadingModel shadingModel;
MaterialBlendMode blendMode;
bool doubleSided;
bool hasNormalMap;
uint32 vertexLayoutMask;
RenderPassType passType;
```

- [ ] **Step 3: Add PSO lookup by material key**

`PipelineCache` should return the graphics pipeline whose `MaterialPipelineKey` exactly matches the current pass and material key. Avoid compiling new PSOs inside the draw loop.

- [ ] **Step 4: Route transparent materials**

`TransparentPass` owns alpha blend draw order. `OpaquePass` must skip alpha blend materials.

- [ ] **Step 5: Run validation and ModelViewer**

Run:

```powershell
cmake --build build_codex --config Release --target MaterialSystemValidation RenderPassBindingValidation RVX_Render
E:\WorkSpace\RenderVerseX\build_codex\Tests\Release\MaterialSystemValidation.exe
E:\WorkSpace\RenderVerseX\build_codex\Tests\Release\RenderPassBindingValidation.exe
cmake --build build\Debug --config Debug --target ModelViewer
```

Launch ModelViewer and confirm opaque DamagedHelmet still renders.

- [ ] **Step 6: Subagent review gate**

Run both review agents on Phase 5 diff. Fix all blocking/high findings and rerun validation plus ModelViewer smoke.

---

## Phase 6: Resource Residency And Async Material Texture Uploads

**Visible effect:** ModelViewer may show fallback material briefly, then transition to full PBR textures once uploads complete. It must not block the render thread.

**Files:**

- Modify: `Render/Private/Renderer/SceneRenderer.cpp`
- Modify: `Render/Private/Material/MaterialSystem.cpp`
- Modify: `Render/Private/GPUResourceManager.cpp`
- Modify: `Tests/GPUResourceManagerValidation/main.cpp`
- Modify: `Tests/MaterialSystemValidation/main.cpp`

- [ ] **Step 1: Add residency tests**

Validate:

- material texture upload requests are deduplicated.
- missing material textures use fallback views.
- descriptor cache refreshes after texture upload completion.
- rendering can proceed while texture resources are uploading.

- [ ] **Step 2: Request visible material texture uploads**

During visible scene preparation, request uploads for all textures referenced by visible material resources, with high priority.

- [ ] **Step 3: Refresh material proxies after upload completion**

When GPU textures become ready or are invalidated, `MaterialSystem` must rebuild affected descriptor entries or clear generation-aware caches.

- [ ] **Step 4: Run validation and ModelViewer**

Run:

```powershell
cmake --build build_codex --config Release --target GPUResourceManagerValidation MaterialSystemValidation RVX_Render
E:\WorkSpace\RenderVerseX\build_codex\Tests\Release\GPUResourceManagerValidation.exe
E:\WorkSpace\RenderVerseX\build_codex\Tests\Release\MaterialSystemValidation.exe
cmake --build build\Debug --config Debug --target ModelViewer
```

Launch ModelViewer and check that loading does not reintroduce render-thread blocking or DX12 device removal.

- [ ] **Step 5: Subagent review gate**

Run both review agents on Phase 6 diff. Fix all blocking/high findings and rerun validation plus ModelViewer visual verification.

---

## Phase 7: Cross-Backend Source Validation And Cleanup

**Visible effect:** No new visual requirement beyond ModelViewer still rendering on the available backend.

**Files:**

- Modify: `Tests/RHIDX12SourceValidation/main.cpp`
- Modify: `Tests/RHIDX11SourceValidation/main.cpp`
- Modify: `Tests/RHIVulkanSourceValidation/main.cpp`
- Modify: backend files only if validation reveals set mapping gaps.
- Modify: `Docs/superpowers/plans/2026-05-13-material-system-phased-implementation.md` to mark completed tasks.

- [ ] **Step 1: Add backend source validations**

Validate:

- DX12 supports register space/set mapping for set 0/1/2.
- Vulkan preserves descriptor set index mapping.
- DX11 emulates set/binding mapping deterministically.
- Dynamic uniform offsets are applied to the correct set and binding.

- [ ] **Step 2: Run full material/render validation set**

Run:

```powershell
cmake --build build_codex --config Release --target RVX_Render MaterialSystemValidation RenderPassBindingValidation GPUResourceManagerValidation ResourceViewCacheValidation RHIDX12SourceValidation RHIDX11SourceValidation RHIVulkanSourceValidation
E:\WorkSpace\RenderVerseX\build_codex\Tests\Release\MaterialSystemValidation.exe
E:\WorkSpace\RenderVerseX\build_codex\Tests\Release\RenderPassBindingValidation.exe
E:\WorkSpace\RenderVerseX\build_codex\Tests\Release\GPUResourceManagerValidation.exe
E:\WorkSpace\RenderVerseX\build_codex\Tests\Release\ResourceViewCacheValidation.exe
E:\WorkSpace\RenderVerseX\build_codex\Tests\Release\RHIDX12SourceValidation.exe
E:\WorkSpace\RenderVerseX\build_codex\Tests\Release\RHIDX11SourceValidation.exe
E:\WorkSpace\RenderVerseX\build_codex\Tests\Release\RHIVulkanSourceValidation.exe
```

- [ ] **Step 3: Run final ModelViewer visual smoke**

Build and launch:

```powershell
cmake --build build\Debug --config Debug --target ModelViewer
E:\WorkSpace\RenderVerseX\build\Debug\Samples\ModelViewer\Debug\ModelViewer.exe
```

Capture only the ModelViewer window and verify:

- DamagedHelmet is visible.
- base color texture is visible.
- normal map detail is visible.
- metallic-roughness changes lighting response.
- no `0x887A0005` or device removed errors appear in the latest log segment.

- [ ] **Step 4: Final subagent review gate**

Run both review agents on the complete material-system diff. Fix all blocking/high findings and rerun the full validation set plus ModelViewer visual smoke.

---

## Commit Strategy

Commit after each completed phase:

```powershell
git status --short
git add <phase files>
git commit -m "Implement material system phase N"
```

Do not commit generated directories:

```text
build*
ShaderCache
.codex
local tool worktrees
```

If the working tree contains unrelated user changes, stage exact paths for the phase instead of `git add -A`.

---

## Phase Completion Checklist

For every phase, record:

```text
Phase:
Implementation commit:
Build commands:
Validation commands:
ModelViewer screenshot path or non-visual validation reason:
GPT-5.5 xhigh review result:
GPT-5.3-Codex-Spark xhigh review result:
Remaining known non-blocking issues:
```

Only move to the next phase when this checklist is complete and has no unresolved blocking/high review findings.


持续执行直到完成整个材质系统的功能
