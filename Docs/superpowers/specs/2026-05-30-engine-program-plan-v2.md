# RenderVerseX Engine Program Plan v2 — Ideal-Target, Execution-Ready

**Date:** 2026-05-30
**Branch context:** `engine-remediation`
**Status:** Authoritative, execution-ready program plan. Decision-complete. **This document is the hard constraint: every phase draws its task from here, not from memory.**
**Supersedes:** `2026-05-30-engine-full-optimization-master-plan.md` (history only).
**Target architecture:** `2026-05-30-ue5-style-engine-framework-design.md` (L1 Object foundation → L7 render proxies).
**Execution discipline:** `2026-05-30-framework-execution-protocol.md`.
**Demoted:** `2026-05-30-ecs-ue-migration-completion-design.md` — no longer a binding target; its 5 increments are early stepping-stones folded into SP1.

---

## 0. Master Execution Rule

**Default execution is strictly serial.** Any "can be parallel" relationship is retained only as dependency-graph commentary, never as the current execution mode.

Every SP runs the same closed loop:

1. Re-confirm this SP's task from this document (`2026-05-30-engine-program-plan-v2.md`).
2. Write this SP's implementation plan: scope, out-of-scope, files touched, tests, acceptance, commit boundary.
3. `gpt-5.3-codex-spark` subagent does the **plan review**.
4. Implementation begins **only** when Spark has no blocker.
5. Run this SP's verification.
6. `gpt-5.3-codex-spark` subagent does the **code review**.
7. After Spark has no blocker, update `phase-log.md`.
8. Commit per SP (or per XL sub-phase).
9. Advance to the next SP.

Priority: **P0** correctness/false-success/lifecycle · **P1** production-readiness · **P2** optimization/advanced.
Size: **S / M / L / XL** (rough effort, not a commitment).

---

## 1. Principles

1. **Honest before deep.** No system reports success while stubbed — first capability-flag or disable; real implementation lands in its own SP.
2. **Ideal target, single object model.** One model `Actor : Object` on a unified object base; **`SceneEntity` is deleted**. RHI and RenderGraph never reference Actor/Component.
3. **Each spine system is built to complete form**, in dependency-correct, strictly serial order.
4. **Stay green + hard render gate.** CTest stays green every increment; render-producing SPs are not "done" without a real golden-image gate — "could not run, reason recorded" is **not** acceptable for render SPs.
5. **The document is the hard constraint.** Each SP pulls its task from this file via the master execution rule.

---

## 2. Per-SP Minimal Acceptance Template

Every SP's plan and its `phase-log.md` entry must fill this format:

- **Source:** this SP's section in v2.
- **Scope:** what this SP is allowed to change.
- **Out of scope:** what must explicitly NOT be done opportunistically.
- **Files/modules:** expected edit surface.
- **Tests:** which standalone/CTest tests are added/modified.
- **Build:** Debug mandatory; Release added per SP risk.
- **Render gate:** whether ModelViewer/golden must run.
- **Spark plan review:** result + blocker resolution.
- **Spark code review:** result + blocker resolution.
- **Commit:** one commit per SP or sub-phase.

---

## 3. Phases and Sub-Projects (strictly serial within and across phases)

### Phase -1 — HS Honesty Sprint (size: S)

**Goal: do not implement deep features — only eliminate false success.**

Coverage:

- **Physics:** any path not wired to `PhysicsWorld` must be capability-flagged or disabled.
- **Particle:** paths where the simulator is unbound / not simulating must not present as usable.
- **AssetPipeline importers:** `success = true` after a TODO is forbidden.
- **AssetDatabase:** JSON save/load must not succeed without real persistence.
- **Editor viewport / hierarchy / inspector:** stub functionality must not present as usable.
- **RenderGraph async / aliasing / compiler validation fallback:** an illegal graph must not fall back into "success".
- **Vulkan query/timestamp:** must not advertise support while only warning.
- **JsonArchive:** invalid JSON must not return `Parse() == true`; a stub serialize must not pretend completeness.

**Verification:** add `HonestyValidation` (or equivalent); relevant standalone tests pass; `ctest --test-dir build -C Debug --output-on-failure` must run where the environment supports it.

### Phase 0 — Foundation (strict order: SP-A → SP-B1 → SP-C)

| SP | P | Size | Scope |
|---|---|---|---|
| **SP-A** Core correctness | P0 | M | JobGraph reentrancy/busy-yield fix; `Result<T,E>` at create/load/compile boundaries; fixed-timestep accumulator; subsystem dependency **fail-fast**. |
| **SP-B1** RHI core contract | P0 | **XL** | submit/wait/signal semantics; GPU-queue-wait separated from CPU-wait; base descriptor contract; DX12/Vulkan aligned. |
| **SP-C** ShaderCompiler async lifetime | P0 | S | async queue owns stable copies; include-dependency invalidation; OpenGL hot reload uses GLSL, not SPIR-V. |

`SP-B2` (Vulkan bindless parity, descriptor arrays/update-after-bind, UAV/aliasing barriers) **stays deferred to P2/P3 — not in Foundation.**

> **Gating note:** `SP-B1` does **not** gate Object/ECS. `SP-B1` gates GPU / Asset / Render / Pipeline work only.

### Phase 1 — Object / ECS / Render Spine (strict order)

| SP | P | Size | Scope |
|---|---|---|---|
| **SP0-Object** | P0 | M | `Object`; runtime `Class` registry; `WeakObjectPtr` / generational handle; `Archive` basics. **Reuse existing `HandlePool`, `ReflectionRegistry`, `TypeRegistry`, Actor/Component factories** — consolidate, don't reinvent. No GC. |
| **SP1a** SceneEntity inventory + compatibility shrink | P0 | M | Enumerate every `SceneEntity` call surface; build the migration inventory; allow only a short-lived compat layer; **no new dependencies on SceneEntity**. |
| **SP1b** Actor/Component single model | P0 | L | One component container; one transform source of truth; remove the per-node `dynamic_cast` dirty path. |
| **SP1c** Migrate call sites | P0 | L | `ModelResource`, Prefab, `World`, `SpatialSubsystem`, `Samples/ModelViewer`, tests. |
| **SP1d** Delete SceneEntity | P0 | M | Remove SceneEntity files/includes/CMake entries; `rg SceneEntity` allowed only in history docs or explicit migration notes; ModelViewer on Actor/Object API. |
| **SP20-VisualGate-Baseline** | P0 | M | Golden-screenshot harness; fixed fixture glTF; defined output path, diff threshold, failure artifacts. Every later render-producing SP calls this mechanism. (Early sub-item of the same SP20 mechanism.) |
| **SP-Proxy-v1** | P0 | L | `PrimitiveComponent::CreateRenderProxy()`; gameplay→render command-queue seam; render-side `Scene` consumes the proxy. **Degraded DoD: only the render-proxy seam must stand; fallback material/pipeline allowed; final PBR not required.** |
| **SP4-Asset** | P0 | L | GPU texture/buffer upload; cache identity; safe hot reload; AssetDatabase JSON persistence + stable GUID; importers produce real artifacts. |
| **SP3-Pipeline** | P1 | L | PSO contract; PipelineCache hash/invalidation/serialization; batching/instancing/material-table mechanism; reverse-Z + D32F. |
| **SP2-Material** | P1 | M | MaterialSystem into SceneRenderer; per-material-id GPU table; PBR/IBL parameter layout; transparent/masked routing. |

### Phase 2 — Render & Runtime Systems (strict order)

Order: **SP5 → SP6 → SP8 → SP9 → SP10 → SP11 → SP7.**
Constraints: **SP11 after SP8** (occlusion); **SP9 after SP5.**
**SP5 / SP6 / SP7 / SP9 (render-related) must run the visual gate — no "couldn't run locally, reason recorded" pass.**

| SP | P | Size | Scope |
|---|---|---|---|
| **SP5-RenderGraph** | P0 | M | Cycles/illegal deps → validation errors; aliasing barriers or aliasing off by default; async via real compute/copy submission or marked graphics-fallback. |
| **SP6-Passes** | P0 | L | Shadow (PSSM), ClusteredLighting, PostProcess (ping-pong: ToneMapping+TAA+Bloom), IBL; implement-or-disable stubs honestly. |
| **SP8-Physics** | P0 | L | PhysicsSubsystem; RigidBodyComponent registers bodies; deterministic Scene↔Physics sync around the fixed step (un-disables Phase -1 flags). |
| **SP9-Particle** | P0 | M | Restore descriptor bindings; indirect dispatch from GPU alive-count; CPU fallback ticks state. |
| **SP10-Animation** | P1 | M | GPU skinning; event dispatch; jobified pose evaluation; AnimatorComponent in tick. |
| **SP11-Audio** | P1 | M | AudioComponent↔AudioSubsystem real transform; `ma_resource_manager` caching; physics-based occlusion. |
| **SP7-GPU-driven** | P2 | M | Frustum cull compute + stream compaction + `ExecuteIndirect`; culling affects submitted draw counts. |

### Phase 3 — World, Content & Tools (strict order)

Order: **SP12 → SP13 → SP14 → SP15 → SP16.**
**Editor (SP13) must be built on the real Object/Actor API — no legacy SceneEntity, no fake selection/gizmo paths.**

| SP | P | Size | Scope |
|---|---|---|---|
| **SP12-Spatial** | P1 | M | Incremental BVH refit + dirty list; unify SceneManager/SpatialSubsystem to one authoritative index keyed on PrimitiveComponent. |
| **SP13-Editor** | P0/P1 | L | Engine/RenderSubsystem offscreen viewport; inspector on real entity API; gizmo writeback; scene save/load; undo/redo; AssetBrowser GUID. |
| **SP14-Tools/Cook** | P1 | M | Real texture (BCn/mipmaps), mesh (tangents/LOD), shader prebake; AssetDatabase reimport with a test that fails on success-without-output. |
| **SP15-UI** | P1 | M | Decide runtime-RHI-UI vs Editor-only-ImGui; UIRenderer DrawRect/Text/Image on RHI; real font metrics. |
| **SP16-Terrain/Water** | P1/P2 | L | Terrain GPU heightmap/material upload + LOD crack mask; Water RenderGraph resources + pipelines; fix `SampleDisplacement` recursion. |

### Phase 4 — Advanced Systems & Final Quality (strict order)

Order: **SP17 → SP18 → SP19 → SP20-hardening → Final ModelViewer validation.**

| SP | P | Size | Scope |
|---|---|---|---|
| **SP17-Networking** | P1/P2 | L | ReplicationManager routing; snapshot + interpolation; auth/packet validation/replay protection/rate limits; heartbeat. |
| **SP18-Scripting** | P1/P2 | M | Sandbox (memory/instruction limits, path containment, API whitelist); broaden bindings; native file-watch hot reload. |
| **SP19-AI** | P1/P2 | L | Tiled navmesh (Recast/Detour) + dynamic obstacles; funnel string-pull; BehaviorTree clone per agent. |
| **SP20-Quality/CI hardening** | P1/P2 | M | GPU-labeled CI tests; full cross-backend golden-image gate; sanitizer + Release gates; profiling counters; capability matrix docs. |

---

## 4. Dependency Graph (commentary only — execution is strictly serial)

```text
Phase -1 (HS) ─> everything
SP-A ─> SP0-Object ─> SP1a ─> SP1b ─> SP1c ─> SP1d ─> SP20-VisualGate-Baseline ─> SP-Proxy-v1
SP-B1 ─> SP4-Asset ─> SP3-Pipeline ─> SP2-Material
(SP-B1 gates GPU/Asset/Render/Pipeline; SP-B1 does NOT gate Object/ECS)
SP-Proxy-v1 + SP2 + SP5 ─> SP6-Passes ─> SP13-Editor
SP-B1 ─> SP5-RenderGraph ; SP5 ─> SP9-Particle
SP1 + SP-A ─> SP8-Physics ─> SP11-Audio
SP1 + SP-A ─> SP10-Animation ; SP3 + SP5 ─> SP7-GPU-driven ; SP1 ─> SP12-Spatial
SP-B2 / SP17 / SP18 / SP19 : independent ; SP20 : baseline early, hardening last
```

---

## 5. Final Acceptance (after all SPs complete)

Run at minimum:

```bash
cmake --build build --config Release --target ModelViewer
ctest --test-dir build -C Release --output-on-failure
```

Then run `Samples/ModelViewer` with the fixed fixture glTF and confirm the full path end-to-end:

```text
glTF → Object Actor/Component → Asset → Pipeline → Material → RenderProxy → Passes → screenshot/golden gate
```

Program is done when:

- No runtime system reports success while stubbed; backend capabilities honest.
- `SceneEntity` does not exist; single object model on the `Object` base; RHI/RenderGraph gameplay-agnostic.
- Editor renders through the engine, edits real entities, save/load + undo/redo.
- Asset importer produces real artifacts with stable GUIDs surviving restart.
- CI: CTest green continuously + GPU/golden-image/sanitizer/Release gates.

---

## 6. Governance

- Every SP runs the master execution rule (§0): plan → `gpt-5.3-codex-spark` plan review → implement → verify → `gpt-5.3-codex-spark` code review → `phase-log.md` → commit.
- Per-phase completion notes are appended to a durable `phase-log.md`.
- The golden-image gate for render SPs is a **hard gate** (designated environment must actually run it).
- If any document and this plan conflict, stop and reconcile the documents before implementing.
- **Next step before any code:** write this revised plan into v2 (done), then have `gpt-5.3-codex-spark` run a pass-through review of the revised document, then commit.
