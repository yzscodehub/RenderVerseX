# ECS UE-Style Migration Completion Design

**Date:** 2026-05-30
**Branch context:** `engine-remediation`
**Depends on:** `2026-05-14-ue-style-actor-component-system-design.md` (the approved target architecture)
**Status:** Approved direction — incremental convergence. This spec covers *finishing* the in-progress migration, not redesigning it.

---

## 1. Summary

The repository already committed to a UE-style Actor/Component object model and implemented most of the core types. The migration, however, was started across Phases 2–5 in parallel and **none of those phases were closed out**. The result is dual paths coexisting everywhere — most dangerously two sources of truth for transform.

This spec defines the work to **converge each duplicated concern onto a single component-as-truth source**, incrementally, keeping `ModelViewer` rendering after every step. It explicitly does **not** introduce archetype/sparse-set (data-oriented) ECS — that was a rejected non-goal in the base design and would contradict the in-progress work.

Material system, pipeline design, and asset system are **separate sub-projects** and out of scope here.

---

## 2. Current State (verified)

| Phase | State | Evidence |
|---|---|---|
| 1. Core types | ✅ Complete | `SceneEntity : public Actor` (`Scene/Include/Scene/SceneEntity.h:53`); `Component : public ActorComponent` (`Scene/Include/Scene/Component.h:38`); `SceneComponent`, `PrimitiveComponent` exist |
| 2. Transform → root SceneComponent | ⚠️ Dual truth | `SceneComponent` owns transform (`SceneComponent.h:72-77`) **and** `SceneEntity` still stores `m_position/m_rotation/m_scale` (`SceneEntity.h:268-270`) plus `m_compatRootComponent` (`SceneEntity.h:292`) |
| 3. Renderable → PrimitiveComponent | ⚠️ Dual path | `StaticMeshComponent : PrimitiveComponent` (`StaticMeshComponent.h:22`) coexists with `MeshRendererComponent : Component` (`MeshRendererComponent.h:39`); `RenderSceneCollector` runs primitive iteration (`RenderSceneCollector.cpp:70`) **and** legacy tree walk (`:155`) |
| 4. Spatial / picking | ⚠️ Dual identity | `SceneManager::RegisterPrimitive` + `m_registeredPrimitives` exist (`SceneManager.h:185,317`) but `SceneEntity` is still `ISpatialEntity` and `SceneManager::AddEntity`/`CollectSpatialEntities` remain (`SceneManager.h:160,257`) |
| 5. Model instantiation | ⚠️ Dual entry | `InstantiateActor()→Actor*` declared (`ModelResource.h:141`) but `InstantiateActorNode` still returns `SceneEntity*` (`:145`); primary path is `Instantiate()→SceneEntity*` (`:137`) |
| 6. Cleanup old names | ❌ Not started | All compatibility surface present |

**Open decisions from the base design are already settled by the code** and are kept as-is:
`SceneEntity : public Actor` and `Component : public ActorComponent` (source compatibility); primitive registration lives in `SceneManager`; tick registration stays in `World`/`SceneManager` until Job-system integration is designed separately.

---

## 3. Design Principle

**Component-as-truth, one source per concern.** For each duplicated concern, the component becomes the only owner of the data; the legacy `SceneEntity`/`Component` surface is reduced to *forwarding* shims, then removed in cleanup. Each convergence is a small, independently shippable commit with a failing-test-first and a `ModelViewer` smoke gate.

**Why incremental (Approach A) over big-bang (B) / fix-only (C):** B breaks `ModelViewer` mid-flight and is unreviewable; C does not actually complete the migration the way the work requires. A keeps the engine green at every step and matches both audits' "wire and keep CPU tests green" guidance.

---

## 4. Increments

Each increment lists: target single source of truth, the dual path it removes, primary files, the test, and the smoke gate. They are ordered by dependency; later increments assume earlier ones are merged.

### Increment 1 — Transform convergence (do first; de-risks everything)

- **Single truth:** root `SceneComponent` transform.
- **Remove:** `SceneEntity::m_position/m_rotation/m_scale` duplicate storage.
- **Change:** `SceneEntity` get/set position/rotation/scale, `GetLocalMatrix`/`GetWorldMatrix`, and dirty propagation forward to `m_compatRootComponent` (auto-created if absent). `Actor::GetWorldMatrix` reads root component.
- **Files:** `Scene/Private/SceneEntity.cpp`, `Scene/Include/Scene/SceneEntity.h`, `Scene/Private/SceneComponent.cpp`.
- **Test:** parent/child world-transform propagation; `SceneEntity::SetPosition` is observable via root `SceneComponent::GetWorldLocation`; dirty propagation updates children (extend `ActorComponentValidation`).
- **Gate:** `ModelViewer` renders DamagedHelmet at correct transform.

### Increment 2 — Render extraction convergence

- **Single truth:** registered `PrimitiveComponent` list.
- **Remove:** legacy `SceneEntity` tree-walk in `RenderSceneCollector` (`RenderSceneCollector.cpp:155`).
- **Change:** collector iterates `SceneManager::GetRegisteredPrimitives()` and calls `PrimitiveComponent::CollectRenderData` only. `MeshRendererComponent` becomes an adapter that registers as / delegates to `StaticMeshComponent`, or its call sites migrate to `StaticMeshComponent`.
- **Files:** `Render/Private/Renderer/RenderSceneCollector.cpp`, `Scene/Private/Components/StaticMeshComponent.cpp`, `Scene/Private/Components/MeshRendererComponent.cpp`.
- **Test:** `StaticMeshComponent` produces the same `RenderObject` fields (transform, bounds, mesh id, submesh material ids, flags, owner) as `MeshRendererComponent` (extend `RenderSceneValidation`/`RenderPassBindingValidation`).
- **Gate:** `ModelViewer` renders DamagedHelmet unchanged; transparent/material routing unchanged.

### Increment 3 — Spatial & picking convergence

- **Single truth:** `PrimitiveComponent` registered with the spatial index.
- **Remove:** `SceneEntity` as `ISpatialEntity` for new registrations (kept compat-only if any caller still needs it, then dropped in cleanup); converge `SceneManager`/`SpatialSubsystem` onto one authoritative index (relates to external audit P0.8).
- **Change:** spatial proxy keyed on `PrimitiveComponent*` + owner `Actor::Handle`; `HitResult` returns `Actor*` + `PrimitiveComponent*`; scene mutations mark the queried index dirty.
- **Files:** `Scene/Private/SceneManager.cpp`, `World/Private/SpatialSubsystem.cpp`, `World/Private/PickingService.cpp`, `Spatial/...` registration glue.
- **Test:** spatial query returns expected primitive + actor; picking returns both; newly created/moved entities are observed (extend `SpatialComponentValidation`).
- **Gate:** `ModelViewer` picking selects the correct primitive/actor.

### Increment 4 — Model instantiation convergence

- **Single truth:** `Actor` + root `SceneComponent` + `StaticMeshComponent` tree.
- **Remove:** `InstantiateActorNode` returning `SceneEntity*`; legacy `Instantiate()` becomes a forwarder.
- **Change:** `ModelResource::InstantiateActor` builds the actor/component graph (root scene component per node, child scene components attached, static-mesh components per mesh primitive, per-submesh material ids); `Instantiate()` calls it and returns a compatibility pointer.
- **Files:** `Resource/Private/Types/ModelResource.cpp`, `Resource/Include/Resource/Types/ModelResource.h`.
- **Test:** instantiation produces an actor tree whose primitives extract identical `RenderObject` data; reuse `ResourceInstantiationValidation`.
- **Gate:** `ModelViewer` loads DamagedHelmet via the actor path.

### Increment 5 — Cleanup

- **Remove:** compatibility-only transform/hierarchy storage from `SceneEntity`; deprecate then delete unused legacy APIs; migrate remaining call sites to `Actor`/`ActorComponent`.
- **Files:** `Scene/...`, plus any remaining `SceneEntity*` consumers.
- **Test:** full `unit|lint` CTest subset green; no remaining duplicate-storage members.
- **Gate:** all samples build and render.

---

## 5. Testing Strategy

- **Failing-test-first** for each increment, then implement to green.
- **Regression set kept green throughout:** `ActorComponentValidation`, `SpatialComponentValidation`, `RenderSceneValidation`, `RenderPassBindingValidation`, `MaterialSystemValidation`, `ResourceInstantiationValidation`.
- **Baseline command:** `ctest --test-dir build/linux_x64_debug -L "unit|lint" --output-on-failure -j 4` (currently 229/229 passing per the 2026-05-30 audit).
- **Smoke gate:** `ModelViewer` renders DamagedHelmet after every increment (manual/screenshot until a golden-image gate exists).

---

## 6. Risks & Mitigations

| Risk | Mitigation |
|---|---|
| Transform divergence during step 1 | Make root `SceneComponent` the source of truth *before* deleting `SceneEntity` fields; forward, do not store. |
| `ModelViewer` breaks mid-migration | Each increment is independently shippable and gated on the smoke test; never remove a legacy path before its replacement is verified equivalent. |
| Spatial subsystem tightly coupled to `SceneEntity*` | Introduce a primitive/spatial proxy compatibility layer in increment 3; migrate query output gradually. |
| Two object models linger permanently | Increment 5 is explicit, scheduled cleanup; no new features added to compatibility classes (forwarding only). |

---

## 7. Out of Scope

- Data-oriented / archetype / sparse-set ECS (rejected non-goal).
- Material system, pipeline design, asset system (separate sub-projects, planned after ECS).
- Prefab file-format rewrite (keep working; add component-class-name/serialization hooks only if needed).
- RHI/RenderGraph awareness of actors/components (must remain actor-agnostic).

---

## 8. Acceptance Criteria

- Root `SceneComponent` is the sole transform truth; `SceneEntity` transform fields removed.
- `RenderSceneCollector` extracts solely from registered primitives; no `SceneEntity` tree walk.
- Spatial index and picking are keyed on `PrimitiveComponent`; `HitResult` carries actor + component.
- `ModelResource` instantiates an actor/component graph; legacy `Instantiate()` forwards.
- Legacy `SceneEntity`/`Component` surface is forwarding-only and slated for removal; `unit|lint` CTest green.
- RHI and RenderGraph remain actor-agnostic.
