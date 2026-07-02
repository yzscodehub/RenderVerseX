# RQ5 - Texture Fallback Provenance Guard Plan

Date: 2026-06-10
Parent program: render-quality continuation after RQ4
Previous stage: RQ4 - PBR Material Texture Visual Gate

## 1. Stage Decision

Make texture fallback provenance visible all the way from `TextureLoader` to `MaterialSystem`. RQ4 added a PBR visual gate, but Spark correctly noted that a failed texture load can still return a default `TextureResource` and look like a real texture slot to material readiness checks. RQ5 closes that honesty gap: default replacement textures must carry fallback identity, and material binding must treat them as fallback instead of setting the corresponding material texture flag.

This is a correctness/diagnostic stage for the render asset path. It does not change BRDF math, add material features, or alter default visual output for valid assets.

## 2. Current Engine Evidence

- `TextureLoader::LoadFromReference()` already reports `TextureLoadStatus::FallbackInvalidReference` or `FallbackLoadFailed` through loader-local last-load status.
- The returned `TextureResource` does not currently carry that fallback provenance.
- `TextureLoader::GetDefaultTexture()` returns real `TextureResource*` objects, so a failed material texture slot can later be uploaded and treated as a valid material texture.
- `MaterialSystem::ResolveTextureView()` currently sets the material texture flag whenever the `TextureResource` is GPU-ready and has an SRV, regardless of whether it is a default replacement texture.
- RQ4 `--expect-material-ready` checks all five material texture flags and `usedFallback == false`; this becomes stronger if default replacement texture resources no longer set those flags.

## 3. Scope

1. Add fallback provenance to `TextureResource`.
   - Add a small, queryable fallback marker such as:
     - `bool IsDefaultFallback() const`
     - `void MarkDefaultFallback(std::string reason)`
     - `const std::string& GetFallbackReason() const`
   - Keep default state as non-fallback for normal loaded textures.
   - Avoid changing texture pixel data or metadata layout semantics beyond the marker.

2. Mark TextureLoader default resources.
   - `GetWhiteTexture()`, `GetNormalTexture()`, and `GetErrorTexture()` must mark their resources as default fallback textures.
   - `LoadFromReference()` invalid/load-failed fallback paths should return marked fallback resources.
   - Successful file/memory/reference loads must remain non-fallback, including cache hits for real textures.

3. Make MaterialSystem honor fallback provenance.
   - `ResolveTextureView()` must detect `textureResource->IsDefaultFallback()`.
   - A default fallback texture in a material slot must:
     - set `usedFallback = true`
     - not set that slot's material texture flag
     - return the MaterialSystem internal fallback view for the slot
   - Extend `MaterialBindingResult` with optional diagnostic flags if useful, for example `fallbackTextureFlags`, so tests and logs can show which slots fell back.
   - Existing valid-material behavior from RQ4 must remain Ready with all texture flags.

4. Tests.
   - Extend `RenderHonestyValidation.TextureReferenceFallbackIsObservable` or add a nearby test proving invalid `LoadFromReference()` returns a `TextureResource` with `IsDefaultFallback() == true`.
   - Add/extend tests proving successful file/memory/embedded reference loads and their cache hits remain `IsDefaultFallback() == false`.
   - Add a `MaterialSystemValidation` test proving a GPU-ready default fallback texture in a material slot reports `MaterialBindingStatus::Fallback`, `usedFallback == true`, and does not set the real texture flag.
   - Add a comparison test or assertions proving omitted optional texture slots are still normal missing textures, while explicit default fallback texture resources are treated as fallback provenance.
   - Ensure RQ4 `ModelViewerPBRMaterialSmoke` still passes with real embedded textures.

5. Documentation and phase log.
   - Record the stage in `Docs/superpowers/specs/phase-log.md`.

## 4. Out of Scope

- Per-source texture load manifests, asset database schema changes, editor UI, async asset retry, streaming, mip generation, sampler-state redesign, texture transform support, or new material features.
- Changing the default textures' pixel colors.
- Changing behavior for materials that simply omit optional texture slots.
- DX12/Vulkan visual golden expansion.

## 5. Expected Files

- `Docs/superpowers/specs/2026-06-10-rq5-texture-fallback-provenance-plan.md`
- `Docs/superpowers/specs/phase-log.md`
- `Resource/Include/Resource/Types/TextureResource.h`
- `Resource/Private/Types/TextureResource.cpp`
- `Resource/Private/Loader/TextureLoader.cpp`
- `Render/Include/Render/Material/MaterialSystem.h` if a diagnostic field is added
- `Render/Private/Material/MaterialSystem.cpp`
- `Tests/RenderHonestyValidation/main.cpp`
- `Tests/MaterialSystemValidation/main.cpp`

## 6. Required Tests

- `RenderHonestyValidation` fallback texture provenance test passes.
- `RenderHonestyValidation` proves successful file/memory/embedded texture loads and cache hits are not marked as fallback.
- `MaterialSystemValidation` fallback texture resource binding test passes.
- `MaterialSystemValidation` distinguishes omitted optional material texture slots from explicit TextureLoader default fallback resources.
- Existing RQ4 visual gates remain green:
  - `ModelViewerPBRMaterialSmoke`
  - `PBRMaterialVisualGoldenValidation`
- Existing focused material/render gates remain green:
  - `MaterialSystemValidation`
  - `RenderHonestyValidation`
  - `RenderPassValidation`
  - `RenderSceneValidation`
  - `PipelineCacheValidation`
- `git diff --check` passes.

## 7. Validation Plan

```powershell
$B = "build/win_x64_debug"
cmake --build $B --config Debug --target RenderHonestyValidation MaterialSystemValidation ModelViewer VisualGoldenValidation
ctest --test-dir $B -C Debug --output-on-failure -R "RenderHonestyValidation|MaterialSystemValidation"
ctest --test-dir $B -C Debug --output-on-failure -R "ModelViewerPBRMaterialSmoke|PBRMaterialVisualGoldenValidation"
cmake --build $B --config Debug --target PipelineCacheValidation RenderPassValidation RenderSceneValidation
ctest --test-dir $B -C Debug --output-on-failure -R "PipelineCacheValidation|RenderPassValidation|RenderSceneValidation"
git diff --check
```

## 8. Risks

- Marking TextureLoader default resources as fallback must not mark legitimately loaded textures as fallback through cache hits.
- Materials with omitted optional texture slots should continue to use MaterialSystem internal fallback views without being treated as texture-load failures.
- Shared default resources can be returned for multiple failures, so fallback diagnostics should be generic unless per-slot status is added later.
- If `ResolveTextureView()` returns the internal fallback view for default replacement resources, descriptor cache keys may change; unit tests should cover this as intended behavior.

## 9. Acceptance Criteria

- A failed `TextureLoader::LoadFromReference()` fallback resource is queryably marked as default fallback.
- Material binding treats a default fallback texture resource as fallback and does not set the corresponding real material texture flag.
- RQ4 material-ready gate remains green for the real PBR fixture.
- Focused material/render regressions pass.
- Spark plan review and Spark code review have no blockers before commit.

## 10. Operating Rule For This Stage

Before implementation starts, reread this document and use only RQ5 scope as the task list. Do not expand into texture streaming, mip generation, sampler rewrites, asset database schemas, or material feature work.

## 11. Spark Review

- Plan review: PASS (`gpt-5.5`, xhigh; agent Pauli `019ead8c-e21b-78c3-add0-12e95fc7bb43`)
- Code review: PASS (`gpt-5.5`, xhigh; agent Pauli `019ead8c-e21b-78c3-add0-12e95fc7bb43`)
- Commit message: `feat(render): track texture fallback provenance`
