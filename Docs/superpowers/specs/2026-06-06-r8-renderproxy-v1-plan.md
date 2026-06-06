# R8 - RenderProxy-v1 Bridge and Main Path Switch Implementation Plan

Date: 2026-06-06

## Source Of Truth

- Primary plan: `Docs/superpowers/specs/2026-05-30-render-program-plan-v1.md`
- Section: `14. R8 - RenderProxy-v1 Bridge and Main Path Switch`
- Lines checked: 403-481
- Previous stage: R7 completed in `7a275bc`; phase-log correction committed in `9319c79`.

## Current Code Findings

- `SceneRenderer::SetupView()` still calls `m_renderScene.CollectFromWorld(world)`.
- `RenderScene::CollectFromWorld()` still delegates to `RenderSceneCollector::Collect`.
- `RenderSceneCollector` collects registered `PrimitiveComponent` objects first, then walks the legacy `SceneEntity` tree for `MeshRendererComponent` and `LightComponent`.
- `PrimitiveComponent` still exposes only `HasRenderData()` and `CollectRenderData(RenderScene&)`.
- `StaticMeshComponent::CollectRenderData()` already has the mesh/material/bounds/transform/shadow mapping needed for a primitive proxy.
- `LightComponent` has no proxy interface; light extraction currently lives only in the legacy entity walk.
- `RenderSceneValidation` covers culling, material draw-list routing, static mesh collection, and legacy collector compatibility, but not a proxy-main path.
- R7 visual gate now exists, so R8 must run `ModelViewerSmoke` and `VisualGoldenValidation` before completion.

## Approved Scope

1. Add render-only proxy data types:
   - Add `Render/Include/Render/Renderer/RenderProxy.h`.
   - Define `RenderProxyId`, `RenderPrimitiveProxy`, `RenderLightProxy`, `RenderProxySnapshot`, and a minimal `RenderProxyCommand` for future queueing.
   - Proxy data may contain render data only: matrices, bounds, mesh ids/resources, material ids/resources, flags, layer mask, sort key, and numeric owner id.
   - Proxy data must not contain `Actor*`, `SceneEntity*`, `Component*`, or gameplay-owned pointers other than the CPU resource pointers already consumed by `RenderObject`.

2. Extend primitive components without removing legacy extraction:
   - Add `PrimitiveComponent::HasRenderProxy()` and `PrimitiveComponent::CreateRenderProxy(RenderPrimitiveProxy&)`.
   - Keep `HasRenderData()` and `CollectRenderData(RenderScene&)` for R8 fallback/comparison.
   - Implement `StaticMeshComponent::CreateRenderProxy()` using the current render data mapping, but do not perform `SceneEntity` identity lookup inside the proxy structure creation.
   - Keep `SceneEntity` owner-id compatibility lookup in the Scene-to-Render bridge only.

3. Add a synchronous Scene-to-Render bridge:
   - Add `RenderProxySceneBridge` under `Render/Private/Renderer`.
   - `BuildSnapshot(World*, RenderProxySnapshot&, RenderProxySceneBridgeResult*)` rebuilds a full snapshot each frame.
   - Define stable `RenderProxySceneBridgeFallbackReason` values for `None`, null world, null scene manager, and legacy-only renderer requirements; every result path must set a deterministic reason.
   - It collects `SceneManager::GetPrimitives()`, calls `primitive->CreateRenderProxy(...)`, fills numeric owner id through the compatibility adapter, and skips inactive/disabled/hidden primitives honestly.
   - It extracts lights in the same stage by walking active scene entities for enabled `LightComponent` and writing `RenderLightProxy` data.
   - It detects legacy-only renderers that still need `RenderSceneCollector` and returns a visible fallback result instead of silently producing an incomplete proxy snapshot.

4. Add `RenderScene::ApplyProxySnapshot(const RenderProxySnapshot&)`:
   - It clears the scene and converts primitive proxies into existing `RenderObject` values.
   - It converts light proxies into existing `RenderLight` values.
   - Culling, sorting, material draw-list building, and passes stay unchanged.

5. Switch `SceneRenderer::SetupView()` to proxy-preferred collection:
   - Create and own a `RenderProxySceneBridge` in `SceneRenderer`.
   - Prefer `BuildSnapshot()` + `RenderScene::ApplyProxySnapshot()`.
   - Use `RenderScene::CollectFromWorld()` only as an audited legacy fallback.
   - Add collection stats exposed from `SceneRenderer`, including last path, proxy frame count, legacy fallback count, last proxy primitive/light counts, and last fallback reason.
   - Log fallback with the reason; no silent fallback.

6. Mark legacy paths for future cleanup:
   - Update comments/docs around `CollectRenderData()`, `RenderScene::CollectFromWorld()`, and `RenderSceneCollector` to describe them as legacy/fallback entry points after R8.
   - Do not delete or rewrite `RenderSceneCollector`.

7. Add focused validation:
   - `StaticMeshComponent` proxy creation maps mesh id/resource, material ids/resources, bounds, transform, visibility, layer mask, and shadow flags.
   - `RenderScene::ApplyProxySnapshot()` produces correct object and light counts/data.
   - The bridge builds a proxy snapshot from registered primitives.
   - The bridge preserves lights in the proxy path.
   - Legacy-only renderers produce a visible fallback result.
   - Unsupported or non-proxy primitive states trigger fallback only when a legacy renderer path is actually required.
   - Hidden/disabled primitives do not enter the proxy main path and do not silently fall back when they are already primitive-controlled.
   - Transform changes appear in a subsequent proxy snapshot.
   - `SceneRenderer` collection stats expose deterministic proxy/fallback counters for tests.
   - Existing legacy collector compatibility tests remain valid.

## Out Of Scope

- Full ECS/Object migration.
- Deleting `SceneEntity`.
- Removing `RenderSceneCollector` or old component collection APIs.
- Persistent proxy lifetime optimization.
- Multi-threaded render command queue.
- Rewriting draw passes, material binding, RHI, or RenderGraph behavior.
- Cross-backend visual golden expansion.

## Implementation Sequence

1. Add `RenderProxy.h` and conversion-safe types.
2. Add primitive proxy methods and `StaticMeshComponent::CreateRenderProxy()`.
3. Add `RenderScene::ApplyProxySnapshot()` and tests for object/light conversion.
4. Add `RenderProxySceneBridge` with primitive collection, light extraction, legacy-only renderer detection, and result stats.
5. Add bridge tests for proxy success, visible fallback, hidden/disabled primitives, lights, and transform updates.
6. Switch `SceneRenderer::SetupView()` to proxy-preferred collection and expose collection stats.
7. Update CMake source lists.
8. Build and run R8 tests plus R7 visual gate.
9. Run Spark code review; only then update phase-log and commit.

## Validation Commands

```powershell
$BuildDir = "build/win_x64_debug"
cmake --build $BuildDir --config Debug --target RenderSceneValidation ResourceInstantiationValidation ModelViewer VisualGoldenValidation ImageCompareValidation
ctest --test-dir $BuildDir -C Debug --output-on-failure -R "RenderSceneValidation|ResourceInstantiationValidation|ModelViewerSmoke|VisualGoldenValidation|ImageCompareValidation"
ctest --test-dir $BuildDir -C Debug --output-on-failure -R "RenderGraphValidation|RenderHonestyValidation|RenderSceneValidation|RenderPassValidation|MaterialSystemValidation|ResourceInstantiationValidation|ModelViewerSmoke|VisualGoldenValidation|ImageCompareValidation"
git diff --check
```

## Done Criteria

- `SceneRenderer::SetupView()` no longer depends on `RenderScene::CollectFromWorld()` as the normal path.
- `RenderScene` can be populated from a `RenderProxySnapshot` without traversing `World` or `SceneEntity`.
- Meshes and lights survive the proxy path.
- Legacy collector fallback is visible through stats/logs and covered by tests.
- Existing legacy collector behavior remains available for fallback.
- `ModelViewerSmoke` and `VisualGoldenValidation` pass with the R7 fixture.
