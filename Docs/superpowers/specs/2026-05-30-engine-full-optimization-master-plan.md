# RenderVerseX Engine Full Optimization & Completion — Master Plan

> **⚠️ SUPERSEDED** by `2026-05-30-engine-program-plan-v2.md`. Kept for history only — do not execute from this document.

**Date:** 2026-05-30
**Branch context:** `engine-remediation`
**Builds on:**
- `2026-05-30-engine-module-optimization-audit.md` (external consolidated audit)
- the in-session 7-domain audit (RHI/Render/Core/Scene-Physics/Resource-Tools-Shader/Anim-Particle-Audio/upper-modules)
- `2026-05-14-ue-style-actor-component-system-design.md` (ECS target architecture)
- `2026-05-30-ecs-ue-migration-completion-design.md` (ECS sub-project spec, already written)

**Status:** Program roadmap. This is the umbrella plan; each sub-project (SP) below gets its own `spec → implementation plan → implementation` cycle. It supersedes the roadmap section of the audit by reconciling it with the chosen build-out philosophy and sequencing.

---

## 1. How to use this document

This is **not** a single implementation plan. It decomposes the whole-engine improvement into ~20 sub-projects, orders them by dependency, and records each one's scope, key work items (with evidence from the audits), and acceptance. Work proceeds **one sub-project at a time**: pick the next SP, write/confirm its spec, run writing-plans, implement with failing-test-first, keep CPU tests green, merge, repeat.

All execution must also follow `2026-05-30-framework-execution-protocol.md`: before each phase, re-read the approved roadmap/spec section, write a phase implementation plan, obtain Spark subagent plan review, implement only the approved scope, run the required tests/smoke checks, obtain Spark subagent code review, fix blocking findings, and commit the phase before moving on.

---

## 2. Guiding principles

1. **Each system is built out to its complete form**, not just minimally wired (per the chosen direction). Where a system is half-built, "complete" means finishing it; where stubbed, it means real implementation.
2. **Component-as-truth** for Scene/ECS (per ECS spec); **RHI and RenderGraph stay gameplay-agnostic** (no Actor/Component awareness).
3. **No false success.** A system either works at runtime or honestly reports unsupported via capability flags — no stub that returns success.
4. **Stay green.** `unit|lint` CTest stays passing after every increment; `ModelViewer` is the standing smoke gate.
5. **Thin shared foundation first.** Only the minimum cross-cutting work that unblocks the spine is front-loaded; it is enabling work, not a rewrite.

---

## 3. The one tension, resolved

Both audits rank "wire/correctness first" (Phase 0 = *make false success impossible*) above large build-outs. The chosen direction is "build each system to complete form" along ECS→Material→Pipeline→Asset.

**Reconciliation (hybrid):** a *thin* Foundation phase (Phase 0) does only the cross-cutting enablers the spine literally depends on (Core concurrency correctness, RHI submit/fence/descriptor contract, asset GPU upload path, shader-on-Linux). Then the spine systems are each taken to complete form in your order, and the rest of the engine follows by dependency. Foundation items can be folded into the first spine sub-project that needs them if you prefer fewer phases — noted per item.

---

## 4. Sub-project catalog

Priority: **P0** correctness/false-success/lifecycle · **P1** production-readiness · **P2** optimization/advanced. "Spec" = dedicated spec filename to create under `Docs/superpowers/specs/`.

### Phase 0 — Foundation enablers (thin, unblock the spine)

**SP-A · Core concurrency & runtime correctness** — P0
- JobGraph: fix lock-held-during-Submit reentrancy hazard + busy-yield `Wait()` → collect-then-submit + condition variable (`Core/Private/Job/JobGraph.cpp:134-168`).
- Error model: introduce `Result<T,E>` (`std::expected`) at RHI create / Resource load / Shader compile boundaries; keep `bool` only for predicates.
- Debug-assert pass on the 5 high-risk sites (subsystem dep validation, allocator bounds, etc.).
- Engine fixed-timestep accumulator for deterministic physics (`Runtime/.../Time.h`, `Engine/Private/Engine.cpp`).
- Subsystem dependency failure must stop init (not fall back to registration order).
- Spec: `2026-05-30-core-correctness.md`

**SP-B · RHI contract & cross-platform honesty** — P0/P1
- Submit/wait/signal: explicit signal values, GPU-queue-wait vs CPU-wait separated, DX12/Vulkan match contract (audit P0.2).
- Vulkan query/timestamp: implement or gate via capability flags so the GPU profiler never shows fake timings (P0.3).
- Linux shader compiler (DXC/shaderc) so Vulkan/OpenGL are usable from Linux/WSL (`ShaderCompiler.cpp:19`).
- Metal honesty: mark `RHI_Metal` (0 cpp) as unimplemented in the platform matrix, or schedule implementation; standardize backend capability reporting (DX11/OpenGL/Metal must not advertise unsupported features).
- Descriptor/barrier model extension: descriptor arrays, update-after-bind, bindless table handles; stage/access masks, queue ownership, UAV/aliasing barriers (audit RHI P1). Brings **Vulkan bindless** to parity with DX12 (DX12=rich, Vulkan currently minimal).
- Spec: `2026-05-30-rhi-sync-contract.md`

**SP-C · ShaderCompiler async lifetime** — P0
- Async compile queue owns stable copies of source/entry/profile/includes/macros (audit P0.1); include-dependency tracking + cache invalidation; OpenGL hot-reload passes GLSL not SPIR-V; macOS reflection via SPIRV-Cross.
- Spec: `2026-05-30-shadercompiler-async-lifetime.md`

### Phase 1 — The spine (build each to complete form, in chosen order)

**SP1 · ECS UE-style migration completion** — P0/P1 — **spec done**
- 5 increments (transform → render extraction → spatial/picking → instantiation → cleanup); component-as-truth; remove dual paths.
- Spec: `2026-05-30-ecs-ue-migration-completion-design.md` ✅ → next: writing-plans.

**SP2 · Material system completion** — P1/P2 — *depends: SP-B, SP4 textures*
- Wire MaterialSystem into SceneRenderer (currently material classes exist: `Render/.../Material/*`).
- Material template/instance/binder → a **material table** (per-material-id GPU buffer), groundwork for **bindless materials**.
- Stable material parameter layout + PBR + IBL parameters; transparent/masked routing preserved.
- Spec: `2026-05-30-material-system.md`

**SP3 · Pipeline/PSO & render batching** — P1/P2 — *depends: SP-B*
- Harden `Render/PipelineCache` (hash/invalidation/serialization) + RHI PSO contract.
- Move SceneRenderer off per-draw descriptor churn → batching / instancing / material table / bindless (audit Render P1).
- Reverse-Z + D32F depth; pipeline state keyed on material+pass.
- Spec: `2026-05-30-pipeline-and-batching.md`

**SP4 · Asset system completion** — P0/P1 — *depends: SP-B*
- **Runtime GPU upload**: TextureResource/MeshResource create real `RHITextureRef/RHIBufferRef` (today CPU-only, `TextureResource.cpp:9-13`).
- Cache identity single-owner + hot-reload in-place replacement (no dangling pointers) (audit P0.7); unify ResourceId hash (drop `std::hash`).
- **Offline AssetPipeline**: stop false success — importers produce real artifacts or report unsupported (P0.9); AssetDatabase JSON persistence + stable GUID; minimal cook loops (texture BCn/mipmaps; shader prebake).
- EXR/HDR path wired; coalesce same-path async loads; LoadBatch async.
- Spec: `2026-05-30-asset-system.md`

### Phase 2 — Render & gameplay production path

**SP5 · RenderGraph fail-fast + async compute + aliasing** — P0 — *depends: SP-B*
- Cycles/illegal deps become validation errors (no silent insertion-order fallback); emit aliasing barriers or disable aliasing by default; real compute/copy-queue submission for async passes or mark as graphics fallback (audit P0.4).
- Spec: `2026-05-30-rendergraph-validation-and-aliasing.md`

**SP6 · Render pass wiring** — P0/P1 — *depends: SP1-4, SP5*
- Register & wire ShadowPass (PSSM cascade fit, not Identity), ClusteredLighting (into SceneRenderer), PostProcessStack (ping-pong, ToneMapping+TAA+Bloom), GBuffer/deferred decision, IBL (irradiance/prefilter/BRDF LUT). Implement-or-disable all stub passes honestly.
- Spec: `2026-05-30-render-pass-wiring.md`

**SP7 · GPU-driven culling → indirect draws** — P1/P2 — *depends: SP3, SP5*
- Frustum cull compute + stream compaction + `ExecuteIndirect`; culling output affects submitted draw counts.
- Spec: `2026-05-30-gpu-driven-rendering.md`

**SP8 · Physics ↔ Scene/World integration** — P0 — *depends: SP1, SP-A*
- Create `PhysicsSubsystem` in World; RigidBodyComponent registers bodies via `PhysicsWorld::CreateBody`; deterministic Scene→Physics→Scene sync around fixed step; settle Jolt-vs-BuiltIn; collision mask/constraints/auto-mass; sleep timer; raycast broad-phase via BVH (audit P0.6).
- Spec: `2026-05-30-physics-world-scene-integration.md`

**SP9 · Particle runtime reconnect** — P0 — *depends: SP-B, SP5*
- Restore descriptor bindings (currently commented out); indirect dispatch driven by GPU alive-count; mesh emitter; sort alive-only; CPU fallback ticks state (audit P0.5).
- Spec: `2026-05-30-particle-runtime-reconnect.md`

**SP10 · Animation completion** — P1/P2 — *depends: SP1, SP-A*
- GPU skinning pass; animation event dispatch; connect compression to evaluator; jobified pose evaluation; `AnimatorComponent` in ECS tick.
- Spec: `2026-05-30-animation-completion.md`

**SP11 · Audio completion** — P1 — *depends: SP1, SP8 (occlusion)*
- AudioComponent↔AudioSubsystem with real transform; `ma_resource_manager` caching (stop per-play re-decode); bus routing to `ma_sound_group`; physics-based occlusion; streaming thread priority; LRU clip cache.
- Spec: `2026-05-30-audio-completion.md`

### Phase 3 — World, content & tools

**SP12 · Spatial & picking** — P1 — *depends: SP1*
- Incremental BVH refit + SceneManager dirty list (stop per-frame full rebuild); implement Grid index; unify SceneManager/SpatialSubsystem to one authoritative index (audit P0.8); apply PickingConfig.
- Spec: `2026-05-30-spatial-incremental-and-unify.md`

**SP13 · Editor honesty & functionality** — P0/P1 — *depends: SP1, SP6, SP-B*
- Engine/RenderSubsystem offscreen viewport (replace standalone GLFW/ImGui/GL shell); inspector connected to real entity API; gizmo writes back transforms; real hierarchy traversal; scene save/load; undo/redo command stack; AssetBrowser GUID integration (audit P0.10).
- Spec: `2026-05-30-editor-viewport-and-tools.md`

**SP14 · Tools / asset cook build-out** — P1 — *depends: SP4, SP-C*
- Real texture (BCn/mipmaps), mesh (import/tangents/LOD/optimize), shader prebake, audio transcode; AssetDatabase persistent metadata + reimport with test that fails on success-without-output.
- Spec: folded into SP4 spec or `2026-05-30-asset-cook-pipeline.md`

**SP15 · UI direction & runtime renderer** — P1 — *depends: SP-B, SP6*
- Decide runtime-RHI-UI vs Editor-only-ImGui (avoid two incomplete UI systems); implement UIRenderer DrawRect/Text/Image on RHI; widget draw; real font metrics (FreeType/MSDF) replacing char-width estimate.
- Spec: `2026-05-30-ui-runtime-renderer.md`

**SP16 · Terrain & Water rendering** — P1/P2 — *depends: SP-B, SP5, SP6*
- Terrain: GPU heightmap/material upload (replace placeholders), LOD crack mask, per-node height range from heightmap.
- Water: declare RenderGraph resources + bind pipelines + collect components + draw; FFT compute or scope to Gerstner; fix `SampleDisplacement` recursion; underwater dynamic resolution.
- Spec: `2026-05-30-terrain-water-rendering.md`

### Phase 4 — Networking, scripting, AI build-out & quality

**SP17 · Networking** — P1/P2
- ReplicationManager routing; snapshot + interpolation/extrapolation; authority request; auth/packet validation/replay protection/rate limits; connection heartbeat/timeout.
- Spec: `2026-05-30-networking-replication.md`

**SP18 · Scripting** — P1/P2
- Sandbox: memory/instruction limits, path containment, dangerous-API whitelist; broaden bindings (Physics/Render/Audio/UI); native file-watch hot reload; ECS lifecycle-safe references.
- Spec: `2026-05-30-scripting-sandbox-and-bindings.md`

**SP19 · AI** — P1/P2
- Tiled navmesh build (Recast/Detour) + dynamic obstacles; funnel string-pull; BehaviorTree clone per agent; perception build-out — or explicitly scope current simplified versions.
- Spec: `2026-05-30-ai-navigation-and-bt.md`

**SP20 · Quality & CI program** — P1/P2 (run continuously)
- GPU-labeled tests executable in CI where possible; golden-image/screenshot cross-backend comparison; sanitizer + Release CI gates; static analysis/format gates; profiling counters (upload bandwidth, pass timings, draw/cull/particle counts, cache pressure); backend capability matrix docs; remove stale pre-CI docs.
- Spec: `2026-05-30-quality-and-ci.md`

---

## 5. Dependency graph (blocking relationships)

```text
SP-A ─┐
SP-B ─┼─> SP1 (ECS) ─> SP12 (Spatial) 
SP-C ─┘        │
SP-B ─> SP4 (Asset) ─> SP2 (Material) ─┐
SP-B ─> SP3 (Pipeline) ────────────────┼─> SP6 (Pass wiring) ─> SP13 (Editor)
SP-B ─> SP5 (RenderGraph) ─────────────┘         │
SP3 + SP5 ─> SP7 (GPU-driven)                    ├─> SP15 (UI)
SP1 + SP-A ─> SP8 (Physics) ─> SP11 (Audio occlusion)   └─> SP16 (Terrain/Water)
SP-B + SP5 ─> SP9 (Particle)
SP1 + SP-A ─> SP10 (Animation)
SP17 / SP18 / SP19  (mostly independent, after SP1)
SP20  (continuous)
```

---

## 6. Execution sequence (honoring the chosen spine)

1. **Phase 0:** SP-A, SP-B, SP-C (can be trimmed to just what SP1–SP4 need).
2. **Phase 1 (spine):** SP1 (ECS) → SP4 (Asset, because Material needs GPU textures) → SP3 (Pipeline) → SP2 (Material). *Note: dependency order puts Asset/Pipeline slightly ahead of Material; if you want strict ECS→Material→Pipeline→Asset, Material's wiring will block on Asset GPU upload — flagged for your call on review.*
3. **Phase 2:** SP5 → SP6 → SP7; in parallel SP8, SP9, SP10, SP11.
4. **Phase 3:** SP12, SP13, SP14, SP15, SP16.
5. **Phase 4:** SP17, SP18, SP19; SP20 throughout.

---

## 7. Definition of done (program)

- No runtime system reports success while stubbed; capability flags honest across backends.
- Load a glTF → ECS actor/component graph → material → pipeline → drawn with shadows + clustered lighting + tonemap; picking selects actor/component; physics steps and syncs; particles simulate; audio is positional.
- Editor renders the scene through the engine, edits real entities, supports save/load + undo/redo.
- Asset importer produces real artifacts with stable GUIDs surviving restart.
- CI: `unit|lint` green continuously; GPU/golden-image/sanitizer/Release gates added.
- RHI/RenderGraph remain gameplay-agnostic; Scene is component-as-truth with no dual paths.

---

## 8. Effort & operating note

~20 sub-projects; this is multi-month work. Drive it **one SP at a time** (spec → plan → implement → merge, stay green). The ECS spec is ready to enter writing-plans now. Re-order or rescope any SP on review — the dependency graph (§5) shows what that affects.
