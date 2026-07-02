# R5b-2 Material Binding Status Plan

**Date:** 2026-06-06
**Parent plan:** `Docs/superpowers/specs/2026-05-30-render-program-plan-v1.md`
**Current R-SP:** R5b - Material System SceneRenderer Wiring, sub-stage 2
**Prerequisite:** R5b-1 completed as `1e61ee3`, with phase-log hash correction `2eb50f1`

## 1. Current Code Findings

- R5b-1 fixed renderer-side material draw routing and pass variant selection.
- `MaterialSystem::UpdateMaterialConstants()` still returns `void`; missing initialization, missing constant buffer, and map failure are not visible to pass callers.
- `MaterialSystem::GetOrCreateMaterialSet()` can silently return the default material set if descriptor creation fails.
- `OpaquePass` and `TransparentPass` still call `UpdateMaterialConstants()` and `GetOrCreateMaterialSet()` separately, then draw even if constants were not updated or the material descriptor was unavailable.
- Texture/default fallback is legitimate for missing or nonresident textures, but it needs a structured, inspectable result so it is not confused with real material binding.

## 2. Approved Scope

R5b-2 completes the material binding status contract for the main renderer path.

1. Add a structured material binding result.
   - Add `MaterialBindingStatus` and `MaterialBindingResult` to `MaterialSystem.h`.
   - The result must expose descriptor set, dynamic constant offsets, status, `constantsUpdated`, `usedFallback`, and a message.
   - Required statuses: ready, explicit fallback, not initialized, unavailable, and error. A neutral/none value is acceptable only before any operation.

2. Add a single material binding preparation entry point.
   - Add `PrepareMaterialBinding(const Resource::MaterialResource*, ResourceViewCache*)`.
   - It must update constants, resolve or fall back material textures, create or reuse a descriptor set, and return a complete result.
   - It must not report success when the system is uninitialized, the constant buffer is missing, mapping fails, the descriptor set is unavailable, or the default fallback set is unavailable.

3. Keep compatibility helpers honest.
   - `UpdateMaterialConstants()` should return success/failure instead of `void`, or delegate to the same internal result path.
   - `GetOrCreateMaterialSet()` may remain for compatibility, but descriptor fallback and failure must update the last status/message.
   - Add `GetLastBindingResult()` and/or `GetLastBindingMessage()` if tests and callers need to inspect the last outcome.

4. Make passes obey binding results.
   - `OpaquePass` and `TransparentPass` must use the structured result before drawing.
   - Error results must skip the draw and log a visible reason.
   - Explicit fallback results may draw, but the fallback status must remain observable.

5. Add focused tests.
   - Missing initialization produces an error result and no drawable descriptor.
   - Map failure produces an error result and does not get overwritten by descriptor fallback.
   - Descriptor creation failure falls back only when the default set is available and reports explicit fallback.
   - Null material or missing/nonresident textures produce explicit fallback rather than hidden success.
   - Pass execution skips drawing when binding preparation returns error.

## 3. Out of Scope

- Draw-list helper or material variant routing changes already completed by R5b-1.
- RenderProxy work.
- ECS/Object/SceneEntity migration.
- RenderGraph hardening.
- Shader compiler, bindless, or new PBR shader feature work.
- Full material asset import/persistence redesign.
- Visual golden or final ModelViewer validation.

## 4. Expected Files

- `Render/Include/Render/Material/MaterialSystem.h`
- `Render/Private/Material/MaterialSystem.cpp`
- `Render/Private/Passes/OpaquePass.cpp`
- `Render/Private/Passes/TransparentPass.cpp`
- `Tests/MaterialSystemValidation/main.cpp`
- `Tests/RenderPassValidation/main.cpp`
- `Docs/superpowers/specs/phase-log.md`

## 5. Required Tests

Add or update automated coverage for:

- `PrepareMaterialBinding()` success with resident/default resources.
- Missing initialization error status and message.
- Constant-buffer map failure error status and message.
- Descriptor creation failure uses explicit fallback only when the default material set exists.
- Null material resource and missing/nonresident textures report explicit fallback.
- `OpaquePass` and `TransparentPass` skip draws when material binding result is error.
- `RenderPassValidation` must assert that error bindings do not call `DrawIndexed`.
- `RenderPassValidation` or `MaterialSystemValidation` must assert that fallback and error can be distinguished by status and message.

Existing regression targets remain in scope:

- `MaterialSystemValidation`
- `RenderPassValidation`
- `RenderSceneValidation`
- `RenderHonestyValidation`
- `PipelineCacheValidation`
- `DX12Validation`
- `VulkanValidation`

## 6. Validation Commands

Use the local build directory:

```powershell
cmake --build build/win_x64_debug --config Debug --target MaterialSystemValidation RenderPassValidation
build\win_x64_debug\Tests\Debug\MaterialSystemValidation.exe
build\win_x64_debug\Tests\Debug\RenderPassValidation.exe
cmake --build build/win_x64_debug --config Debug --target MaterialSystemValidation RenderPassValidation RenderSceneValidation RenderHonestyValidation PipelineCacheValidation DX12Validation VulkanValidation
ctest --test-dir build/win_x64_debug -C Debug --output-on-failure -R "MaterialSystemValidation|RenderPassValidation|RenderSceneValidation|RenderHonestyValidation|PipelineCacheValidation|DX12Validation|VulkanValidation"
git diff --check
```

## 7. Done Criteria

- Material binding has a declared result contract.
- Passes draw only on success or explicit fallback.
- Passes skip error bindings with visible logs.
- Default/fallback material usage is inspectable.
- Required validation passes.
- Spark code review returns PASS before phase-log update and commit.

## 8. Compatibility / Risk

- Existing `UpdateMaterialConstants()` and `GetOrCreateMaterialSet()` call sites should keep compiling; R5b-2 may change return value from `void` to `bool`, but no public caller should be forced into a larger API rewrite.
- New pass code should prefer `PrepareMaterialBinding()` so update + descriptor + dynamic offset stay an atomic material draw contract.
- R5b-2 should not change shader layouts or descriptor binding slots; it only makes existing set 2 binding results observable.
