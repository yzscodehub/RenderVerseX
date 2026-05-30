# RenderVerseX Render Program Plan v1

**Date:** 2026-05-30
**Status:** Authoritative render-first roadmap.
**Scope:** Rendering systems and the infrastructure required to make rendering honest, complete, and verifiable.
**Supersedes for current work:** `2026-05-30-engine-program-plan-v2.md` is reference-only while this render-first program is active.
**Execution discipline:** Each R-SP must follow `2026-05-30-framework-execution-protocol.md`.
**Phase log:** `Docs/superpowers/specs/phase-log.md`.

---

## 0. Program Intent

This plan intentionally narrows the current effort to render-related systems:

- RHI contracts and backend capability honesty.
- Shader compiler lifetime, reflection, and binding metadata.
- Pipeline state, pipeline cache, and render-state contracts.
- Render asset GPU upload and render asset persistence needed by ModelViewer.
- Material binding and SceneRenderer integration.
- RenderGraph correctness, barriers, async capability honesty, and validation.
- Render proxies and render-side scene consumption.
- Render passes and post-processing needed for a production ModelViewer path.
- Visual validation and final ModelViewer acceptance.

This plan does not make the full engine architecture migration the current critical path.

---

## 1. Explicit Non-Goals

The following are deferred unless a render SP explicitly needs a minimal bridge:

- Full `Object` base rollout.
- Complete `SceneEntity` deletion.
- Full Editor implementation.
- Full Physics, Audio, Networking, Scripting, AI, Terrain, or Water completion.
- Full particle simulation. Only rendering-facing honesty or a minimal visible path is in scope.
- Advanced bindless parity, indirect rendering, or GPU-driven renderer unless R11 starts.

`SceneEntity` may remain during this program. Render-facing work must reduce renderer dependence on legacy full-frame tree extraction, but full gameplay model cleanup is outside this roadmap.

---

## 2. Execution Rules

Every R-SP uses the same gate:

1. Re-read this plan and identify the current R-SP.
2. Write a phase implementation plan with scope, out-of-scope, touched files, tests, and validation commands.
3. Run Spark plan review using `gpt-5.3-codex-spark`.
4. Implement only the approved scope.
5. Run the required validation.
6. Run Spark code review using `gpt-5.3-codex-spark`.
7. Fix review blockers.
8. Update `phase-log.md`.
9. Commit this R-SP or sub-stage.
10. Continue to the next R-SP.

Default execution is strict serial. Parallel work is dependency information only, not the active operating mode. XL stages may be split into sub-stages, but each sub-stage must pass the same gate and commit independently.

No render-producing R-SP can be marked complete without the required visual gate once R7 exists. If no local GPU/headless visual environment can run the gate, the R-SP is blocked unless the user explicitly switches to a non-render SP.

---

## 3. Validation Baseline

Preferred commands, adjusted per local build layout:

```powershell
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

Render or backend SPs add the relevant standalone tests:

```powershell
.\build\Tests\Debug\RenderGraphValidation.exe
.\build\Tests\Debug\RenderSceneValidation.exe
.\build\Tests\Debug\GPUUploadServiceValidation.exe
.\build\Tests\Debug\GPUResourceManagerValidation.exe
.\build\Tests\Debug\MaterialSystemValidation.exe
.\build\Tests\Debug\ResourceInstantiationValidation.exe
.\build\Tests\Debug\DX12Validation.exe
.\build\Tests\Debug\VulkanValidation.exe
.\build\Tests\Debug\DX11Validation.exe
.\build\Tests\Debug\CrossBackendValidation.exe
```

New validation targets introduced by this program:

- `RenderHonestyValidation`
- `ShaderCompilerValidation`
- `PipelineCacheValidation`
- `VisualGoldenValidation`
- `ModelViewerSmoke`

These targets must be created before a later R-SP depends on them.

Final acceptance:

```powershell
cmake --build build --config Release --target ModelViewer
ctest --test-dir build -C Release --output-on-failure
.\build\Samples\Release\ModelViewer.exe <fixed-fixture-gltf>
```

The actual executable path must be recorded in `phase-log.md` if the generator emits a different layout.

---

## 4. R0 - Documentation and Scope Lock

**Priority:** P0  
**Size:** S  
**Purpose:** Establish the render-first source of truth before code changes.

Scope:

- Add this plan as the active roadmap.
- Add `phase-log.md` template.
- Mark full-engine v2 as reference-only for current render work.
- Define strict serial execution and Spark gates.
- Define render visual gate policy.

Done:

- This document exists and is referenced by future phase plans.
- `phase-log.md` exists.
- Spark plan review has no blockers for R0.
- Commit contains documentation only.

---

## 5. R-HS - Render Honesty Sprint

**Priority:** P0  
**Size:** M  
**Purpose:** Make render-related false success impossible before deeper work starts.

Rule:

- Do not deep-implement features in this sprint.
- Any unimplemented path must be `supported=false`, disabled, or return a visible error.
- Warn-only fallback is not sufficient when the caller can still treat the feature as successful.

Scope:

- RenderGraph compiler fallback: illegal topology, cycles, and incomplete execution order must not silently become success.
- RenderGraph async fallback: fallback must be capability-visible and must not advertise async support.
- RenderGraph aliasing: aliasing barriers must be disabled by capability or made visible as unsupported until implemented.
- Vulkan query/timestamp/query pool: no warn-only pseudo-success.
- GPUProfiler timestamp path: respect backend capability; no fake profiling data.
- AssetPipeline render importers: no TODO followed by `success=true`.
- Texture loading fallback: default texture fallback must be explicit and distinguishable from successful source load.
- MaterialTemplate and MaterialBinder TODO paths: no success-as-placeholder for compile or bind.
- Stub render passes and post-process passes: implement-or-disable; no silent visual pass-through marked as feature success.
- Particle rendering path: GPU/CPU particle rendering must report capability honestly if simulation or draw setup is disconnected.
- AssetDatabase/render cache persistence: save/load must not report success without durable output when render assets depend on it.
- JsonArchive in render asset persistence paths: invalid input must not parse as success.

Required tests:

- Add `RenderHonestyValidation`.
- Add or update targeted tests for false-success paths.
- Existing render validation remains green.

Done:

- `RenderHonestyValidation` fails before the fixes and passes after them.
- No known render-facing stub returns success while doing nothing.
- Spark code review has no blockers.

---

## 6. R1 - RHI Core Contract

**Priority:** P0  
**Size:** XL  
**Purpose:** Stabilize the backend contract that all higher render work depends on.

Scope:

- Submit, wait, and signal semantics.
- Separate CPU wait from GPU queue wait.
- Descriptor and barrier base contract.
- DX12 and Vulkan contract alignment.
- Query/timestamp capability reporting or implementation.
- Backend capability matrix for render features.

Out of scope:

- Full bindless parity.
- Advanced descriptor arrays/update-after-bind unless needed by the base contract.

Required tests:

- `DX12Validation`
- `VulkanValidation`
- `DX11Validation`
- `CrossBackendValidation` when available
- Relevant `RenderHonestyValidation` cases

Done:

- Backends expose honest capabilities.
- Higher layers can branch on capability without warn-only pseudo-success.

---

## 7. R2 - ShaderCompiler and Reflection

**Priority:** P0  
**Size:** M  
**Purpose:** Make shader compilation and binding metadata stable enough for pipeline and material work.

Scope:

- Async compile queue owns stable copies.
- Include dependency invalidation.
- Shader reflection and binding metadata.
- OpenGL hot reload uses GLSL path, not SPIR-V by accident.
- Compile/load boundaries return visible errors.

Required tests:

- Add `ShaderCompilerValidation`.
- Shader include invalidation case.
- Async lifetime case.

Done:

- Pipeline layout generation can depend on reflection metadata.

---

## 8. R3 - Pipeline and PSO Foundation

**Priority:** P0/P1  
**Size:** L  
**Purpose:** Create a stable pipeline state contract before material and pass completion.

Scope:

- PSO state contract.
- Pipeline layout from shader reflection.
- PipelineCache hash, invalidation, and serialization.
- Reverse-Z and D32F baseline.
- Clear error path for missing shaders or incompatible layouts.

Required tests:

- Add or ensure `PipelineCacheValidation`.
- Ensure `MaterialSystemValidation`.
- Ensure backend validation for at least the active primary backend.

Done:

- SceneRenderer and passes can request pipelines without ad hoc state construction.

---

## 9. R4 - Asset GPU Upload

**Priority:** P0  
**Size:** L  
**Purpose:** Ensure render assets become real GPU resources with stable identity.

Scope:

- glTF mesh buffer upload to real RHI buffers.
- Texture upload to real RHI textures.
- Material texture references resolved to GPU resources.
- GPUResourceManager cache identity.
- Upload service synchronization/fence correctness.
- Minimal render asset GUID/cache persistence needed by ModelViewer.

Required tests:

- Ensure `GPUUploadServiceValidation`.
- Ensure `GPUResourceManagerValidation`.
- Ensure `ResourceInstantiationValidation`.
- Ensure `RenderHonestyValidation` asset cases.

Done:

- ModelResource can provide renderable GPU resources without fake importer success.

---

## 10. R5a - Material Binder and Template Minimum Wiring

**Priority:** P0  
**Size:** M  
**Purpose:** Remove placeholder material compile/bind success before broader material integration.

Scope:

- `MaterialTemplate::Compile` must compile or return a visible failure.
- `MaterialBinder::Bind*` must bind required resources or report missing capability/data.
- Fallback material is explicit and logged as fallback.
- No TODO path may be counted as successful material binding.

Required tests:

- Ensure `MaterialSystemValidation`.
- Add or ensure cases for missing texture, missing pipeline, and fallback material.

Done:

- SceneRenderer can distinguish real material bind from fallback/error.

---

## 11. R5b - Material System SceneRenderer Wiring

**Priority:** P1  
**Size:** M  
**Purpose:** Connect material data to rendered objects in the main renderer path.

Scope:

- MaterialSystem to SceneRenderer wiring.
- Per-material GPU constants/table.
- Stable PBR parameter layout.
- Transparent and masked routing.
- Legacy collector path may remain until R8, but material behavior must be honest.

Required tests:

- `MaterialSystemValidation`
- `RenderSceneValidation`
- ModelViewer smoke if R7 is already available.

Done:

- Rendered objects use material state through a declared contract rather than hidden defaults.

---

## 12. R6 - RenderGraph Hardening

**Priority:** P0  
**Size:** L  
**Purpose:** Make RenderGraph correctness enforceable.

Prerequisite:

- R-HS has already made silent fallback visible or disabled.

Scope:

- Cycle and illegal dependency hard errors.
- Incomplete topology cannot be repaired by appending execution order.
- Read/write hazard validation.
- Resource lifetime validation.
- Aliasing barriers are emitted and tested, or aliasing is off by default with capability state.
- Async compute/copy uses real submission or is explicitly graphics fallback with `asyncSupported=false`.

Required tests:

- `RenderGraphValidation`
- Cycle/illegal dependency negative tests.
- Aliasing barrier assertion or disabled-capability assertion.
- Async capability assertion replacing success-as-fallback tests.

Done:

- Invalid graphs fail predictably.
- RenderGraph behavior is test-visible, not log-only.

---

## 13. R7 - Visual Gate Baseline

**Priority:** P0  
**Size:** M  
**Purpose:** Establish the visual gate used by all later render-producing SPs.

Scope:

- Fixed fixture glTF.
- Fixed camera, time, frame count, resolution, backend, and render options.
- Screenshot output path.
- Golden image storage.
- ImageCompare threshold.
- Failure artifact retention: actual image, expected image, diff image, log.
- `VisualGoldenValidation` target.
- `ModelViewerSmoke` target or script.

Required tests:

- `VisualGoldenValidation`
- `ModelViewerSmoke`

Done:

- At least one local backend can run the gate reproducibly.
- Every later render-producing SP references this gate.

---

## 14. R8 - RenderProxy-v1 Bridge and Main Path Switch

**Priority:** P0/P1  
**Size:** L  
**Purpose:** Decouple renderer consumption from legacy full-frame scene tree extraction without requiring full ECS rewrite.

Current state to confirm before implementation:

- `SceneRenderer::SetupView()` still calls `m_renderScene.CollectFromWorld(world)`.
- `RenderScene::CollectFromWorld()` still delegates to `RenderSceneCollector`.
- `RenderSceneCollector` still mixes registered `PrimitiveComponent` extraction with legacy `SceneEntity` tree walking.
- `PrimitiveComponent` exposes `CollectRenderData(RenderScene&)`, but no `CreateRenderProxy(...)`.
- `StaticMeshComponent::CollectRenderData()` already contains most of the data-mapping logic needed by a primitive proxy.
- `RenderSceneValidation` still primarily validates the legacy collector path.

Scope:

- Add `Render/Include/Render/Renderer/RenderProxy.h`.
- Define `RenderProxyId`, `RenderPrimitiveProxy`, `RenderLightProxy`, `RenderProxySnapshot`, and a minimal `RenderProxyCommand` shape for future queueing.
- Proxy structs may contain render data only: transforms, normal matrix, bounds, mesh id/resource, material ids/resources, flags, layer/visibility, sort key, owner id.
- Proxy structs must not contain `Actor*`, `SceneEntity*`, `Component*`, or gameplay-owned pointers other than the existing CPU resource pointers already consumed by `RenderObject`.
- Extend `PrimitiveComponent` with `HasRenderProxy()` and `CreateRenderProxy(RenderPrimitiveProxy&)`.
- Keep `CollectRenderData(RenderScene&)` during R8 for comparison and fallback; mark it as legacy after proxy tests pass.
- Implement `StaticMeshComponent::CreateRenderProxy()` by moving or sharing the current `RenderObject` construction logic from `CollectRenderData()`.
- Keep any `SceneEntity` identity lookup in the Scene-side compatibility adapter only; do not leak `SceneEntity` into Render proxy data.
- Add a Scene-to-Render bridge, such as `RenderProxySceneBridge` or `SceneRenderProxyCollector`.
- Bridge v1 is synchronous and may rebuild the full snapshot each frame.
- Bridge v1 collects `SceneManager::GetPrimitives()` and calls `primitive->CreateRenderProxy(...)`.
- Add light proxy support in the same stage: `RenderLightProxy`, light component proxy creation or bridge-side light extraction, and `RenderScene` light population.
- Add `RenderScene::ApplyProxySnapshot(const RenderProxySnapshot&)`.
- `ApplyProxySnapshot()` converts primitive proxies into existing `RenderObject` values and light proxies into existing `RenderLight` values so culling, sorting, draw lists, and passes can remain stable.
- Change `SceneRenderer::SetupView()` to prefer the proxy path when the bridge returns a valid snapshot.
- `RenderSceneCollector` remains as an audited fallback only.
- Fallback must be visible through logs, statistics, and tests; silent fallback to legacy collection is not allowed.
- Track all remaining legacy collector dependencies for later ECS/Object cleanup.

Implementation sequence:

1. Add proxy data types and conversion helpers.
2. Add `PrimitiveComponent` proxy methods while preserving legacy render extraction.
3. Implement `StaticMeshComponent::CreateRenderProxy()`.
4. Add a synchronous proxy bridge that builds `RenderProxySnapshot` from the current scene.
5. Add `RenderScene::ApplyProxySnapshot()`.
6. Add light proxy support before switching the main renderer path, or explicitly record light fallback as a temporary R8 sub-stage blocker.
7. Update `SceneRenderer::SetupView()` to use proxy-preferred collection with audited legacy fallback.
8. Add tests for proxy creation, snapshot application, fallback visibility, transform updates, hidden/disabled primitives, and light preservation.
9. Switch the main path to proxy when tests and visual gate pass.
10. Mark `CollectRenderData()`, `CollectFromWorld()`, and `RenderSceneCollector` as legacy-only entry points for future cleanup.

Out of scope:

- Full ECS/Object migration.
- Deleting `SceneEntity`.
- Multi-threaded render command queue.
- Persistent proxy lifetime optimization.
- Removing legacy collector files.
- Rewriting draw passes.

Required tests:

- Ensure `RenderSceneValidation` covers legacy compatibility and proxy-main behavior.
- Add or ensure `StaticMeshComponent` proxy tests for mesh id/resource, material ids/resources, bounds, transform, visibility, and shadow flags.
- Add or ensure `RenderScene::ApplyProxySnapshot()` tests for object count, light count, and converted render data.
- Add or ensure fallback visibility tests: proxy bridge failure must increment/record a legacy fallback signal.
- Add or ensure disabled and hidden primitives do not enter the proxy main path.
- Add or ensure transform changes are reflected in a subsequent snapshot.
- Ensure `ResourceInstantiationValidation`.
- Ensure `VisualGoldenValidation`.
- Ensure `ModelViewerSmoke`.

Done:

- Main render path can render from proxies.
- `SceneRenderer::SetupView()` no longer depends on `RenderScene::CollectFromWorld()` as the normal path.
- `RenderScene` can be populated without traversing `World` or `SceneEntity`.
- Legacy collector is no longer the only working path.
- Legacy collector fallback is visible and test-covered.
- Meshes and lights survive the proxy path.
- ModelViewer renders the same fixture glTF through the proxy path.

---

## 15. R9 - Render Pass Completion

**Priority:** P1  
**Size:** XL  
**Purpose:** Complete the production visual pass chain required by ModelViewer.

Scope:

- Shadow/PSSM minimum path.
- Clustered lighting minimum path.
- Tone mapping.
- Bloom and/or TAA minimum path as selected in the phase plan.
- IBL minimum path.
- Pass resource declarations through RenderGraph.
- Stub passes implement-or-disable.

Required tests:

- `RenderGraphValidation`
- `RenderSceneValidation`
- `VisualGoldenValidation`
- `ModelViewerSmoke`

Done:

- ModelViewer renders through the declared pass chain.
- No enabled pass is a silent no-op.

---

## 16. R10 - Particle Rendering Honesty and Minimal Visual Path

**Priority:** P1  
**Size:** M  
**Purpose:** Make particle rendering behavior honest and minimally visible if enabled.

Scope:

- Capability honesty for GPU particles.
- CPU fallback visible path if selected by phase plan.
- Particle renderer draw setup does not report success if simulation or buffers are unavailable.

Out of scope:

- Full particle authoring or advanced simulation.

Required tests:

- Particle render smoke test or `RenderHonestyValidation` particle cases.
- Visual gate if a sample renders particles.

Done:

- Enabled particles are visible and testable, or particles are explicitly unsupported.

---

## 17. R11 - GPU-Driven Optional Advanced

**Priority:** P2  
**Size:** L  
**Purpose:** Add advanced GPU-driven rendering only after the production path is green.

Scope:

- Frustum cull compute.
- Stream compaction.
- Indirect draw.
- Advanced descriptor/bindless work needed by the GPU-driven path.

Required tests:

- Backend validation for the selected backend.
- Visual gate proving submitted draw counts affect output.

Done:

- GPU-driven path changes real submitted work and can be disabled back to the baseline.

---

## 18. R12 - Final ModelViewer Validation

**Priority:** P0  
**Size:** S  
**Purpose:** Final acceptance for this render-first program.

Scope:

- Release build of ModelViewer.
- Fixed fixture glTF.
- At least one reproducible backend, priority DX12 or Vulkan.
- Record backend, driver, resolution, fixture, command, and artifacts.
- Run relevant Release CTest suite.

Required commands:

```powershell
cmake --build build --config Release --target ModelViewer
ctest --test-dir build -C Release --output-on-failure
.\build\Samples\Release\ModelViewer.exe <fixed-fixture-gltf>
```

Done:

- ModelViewer passes the visual gate.
- Render path is documented from glTF asset to GPU upload, material, pipeline, RenderGraph, render proxy, pass chain, and final image.
- Spark final review has no blockers.

---

## 19. Per-SP Phase Plan Template

Each phase implementation plan must include:

- Current R-SP and source lines from this document.
- Goal.
- Scope.
- Out of scope.
- Expected files/modules.
- Risks.
- Tests to add or update.
- Build and validation commands.
- Visual gate requirement.
- Spark plan review result.
- Spark code review result.
- Phase-log entry.
- Commit message.

---

## 20. Conflict Rule

If any older document conflicts with this plan during the render-first program, this plan wins. If a conflict affects implementation safety, stop and reconcile the document before writing code.
