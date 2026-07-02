# R5b-1 Material Draw Routing Plan

**Date:** 2026-06-02
**Parent plan:** `Docs/superpowers/specs/2026-05-30-render-program-plan-v1.md`
**Current R-SP:** R5b - Material System SceneRenderer Wiring, sub-stage 1
**Prerequisite:** R5a completed as `48c6d2b`, with phase-log hash correction `200014a`
**Spark blocker addressed:** first R5b plan review blocked the broader plan and required pass execution tests plus a split into R5b-1/R5b-2.

## 1. Current Code Findings

- `SceneRenderer::SetupView()` already collects `RenderObject` data and calls `BuildMaterialDrawLists()`.
- `BuildMaterialDrawLists()` already splits opaque, masked, and transparent `RenderDrawItem`s, but the logic is private and cannot be validated without initializing a renderer.
- `OpaquePass::Execute()` currently binds the opaque pipeline once, then draws both opaque and masked items through that pipeline. Masked routing is therefore only a CPU-list concept and is not honored by actual pass execution.
- `TransparentPass::Execute()` uses the transparent pipeline and material draw-item list.
- `PipelineCache` already exposes `GetPipelineForVariant(MaterialPipelineVariant)`, `GetOpaquePipeline()`, `GetMaskedPipeline()`, and `GetTransparentPipeline()`.

## 2. Approved Scope

R5b-1 fixes and tests the renderer-side material draw routing only.

1. Extract a testable draw-list builder from `SceneRenderer::BuildMaterialDrawLists()`.
   - Add a small helper in `RenderDrawItem.h/.cpp`, for example `BuildMaterialDrawLists(...)`.
   - Inputs: `RenderScene`, visible object indices, camera position.
   - Outputs: opaque, masked, and transparent draw item vectors.
   - Preserve existing material classification, submesh identity, `materialResource`, `materialId`, sort keys, and transparent back-to-front order.

2. Make `SceneRenderer::BuildMaterialDrawLists()` delegate to the helper.
   - Preserve current world collection, culling, sorting, and GPU upload request behavior.

3. Make pass execution honor material variant routing.
   - `OpaquePass` must bind the opaque pipeline only for opaque draw items.
   - `OpaquePass` must bind the masked pipeline for masked draw items.
   - Missing opaque or masked pipeline must skip that draw group with visible logging; it must not reuse a previous/wrong pipeline.
   - `TransparentPass` must continue to use the transparent pipeline for transparent draw items.

4. Add pass execution tests.
   - Tests must construct pass inputs with fake command/RHI dependencies and assert actual `SetPipeline` order/identity and draw count.
   - Tests must catch the current bug where masked draw items are drawn after only the opaque pipeline is bound.
   - Tests must cover missing masked pipeline skip behavior.

## 3. Out of Scope

- `MaterialSystem` status/result redesign. This is R5b-2.
- `MaterialSystem::UpdateMaterialConstants()` return type changes. This is R5b-2.
- Descriptor fallback status/result structs. This is R5b-2.
- RenderProxy or Scene-to-render proxy bridge work.
- ECS/Object/SceneEntity deletion or migration.
- RenderGraph correctness hardening.
- Shader compiler changes, bindless descriptors, or new PBR shader features.
- Final ModelViewer acceptance; that remains R12.

## 4. Expected Files

- `Render/Include/Render/Renderer/RenderDrawItem.h`
- `Render/Private/Renderer/RenderDrawItem.cpp`
- `Render/Private/Renderer/SceneRenderer.cpp`
- `Render/Private/Passes/OpaquePass.cpp`
- `Render/Private/Passes/TransparentPass.cpp` if execution assertions require a small observable guard
- `Tests/RenderSceneValidation/main.cpp`
- `Tests/MaterialSystemValidation/main.cpp` only if shared fake RHI helpers are extended there
- `Tests/RenderPassValidation/main.cpp`
- `Tests/CMakeLists.txt` if a new validation target is added
- `Docs/superpowers/specs/phase-log.md`

## 5. Required Tests

Add or update automated coverage for:

- Draw-list builder routes opaque, masked, and transparent submeshes into the correct vectors.
- Draw-list builder preserves `materialResource`, `materialId`, `submeshIndex`, and `renderMode`.
- Transparent draw items are sorted back-to-front; opaque/masked use stable sort keys.
- `OpaquePass` execution binds opaque and masked pipelines separately and draws both groups when both pipelines exist.
- `OpaquePass` execution skips masked draw items when the masked pipeline is missing and does not draw them with the opaque pipeline.
- `OpaquePass` execution skips opaque draw items when the opaque pipeline is missing and does not draw them with the masked pipeline.
- Existing transparent draw routing remains covered by either a focused `TransparentPass` execution test or an assertion that its pipeline selection still uses the transparent variant.

## 6. Validation Commands

Use the local build directory discovered in previous phases:

```powershell
cmake --build build/win_x64_debug --config Debug --target RenderSceneValidation RenderPassValidation MaterialSystemValidation
build\win_x64_debug\Tests\Debug\RenderSceneValidation.exe
build\win_x64_debug\Tests\Debug\RenderPassValidation.exe
build\win_x64_debug\Tests\Debug\MaterialSystemValidation.exe
cmake --build build/win_x64_debug --config Debug --target RenderSceneValidation RenderPassValidation MaterialSystemValidation RenderHonestyValidation PipelineCacheValidation DX12Validation VulkanValidation
ctest --test-dir build/win_x64_debug -C Debug --output-on-failure -R "RenderSceneValidation|RenderPassValidation|MaterialSystemValidation|RenderHonestyValidation|PipelineCacheValidation|DX12Validation|VulkanValidation"
git diff --check
```

## 7. Done Criteria

- `SceneRenderer` uses a declared, test-covered material draw-list contract.
- Opaque and masked draw items route to different pipeline variants at actual pass execution time.
- Missing masked/opaque pipeline does not silently draw with the wrong pipeline.
- Transparent routing remains intact.
- `RenderSceneValidation` and `RenderPassValidation` both pass independently before broader CTest.
- Required validation passes.
- Spark code review returns PASS before phase-log update and commit.

## 8. Next Sub-Stage

R5b-2 will handle `MaterialSystem` status/result contracts:

- `UpdateMaterialConstants()` success/failure return or result struct.
- Structured last status/message for map failure, missing initialization, missing buffer, descriptor fallback, and descriptor creation failure.
- Passes obey material update/descriptors by drawing only on success or explicit fallback.
