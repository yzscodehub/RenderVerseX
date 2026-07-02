# UE5-Style Engine Framework — Top-Level Target Architecture

**Date:** 2026-05-30
**Branch context:** `engine-remediation`
**Framing:** Ideal target architecture (clean-slate), with **UE5 as the north star**. This spec describes what the top-level framework *should* be, not how the current half-migrated code gets there.
**Relationship to existing specs:**
- Supersedes the *target* sections of `2026-05-14-ue-style-actor-component-system-design.md` by lifting it from "Actor/Component object model" to "whole top-level framework".
- The convergence path from today's code is owned by `2026-05-30-ecs-ue-migration-completion-design.md` (the 5 increments). This spec deliberately does **not** re-derive that migration.

---

## 1. Design Philosophy

> **Copy UE5's architectural skeleton, but replace its two heaviest organs — garbage collection and UHT code generation — with C++20-native mechanisms.**

UE5's framework draws its power from two ideas worth copying wholesale:

1. A **unified object base** (`UObject`) from which reflection, serialization, factory spawning, and editor introspection all grow.
2. A **hard game-thread / render-thread isolation** via persistent render proxies (`FPrimitiveSceneProxy`).

But UE implements those ideas with garbage collection (mark-and-sweep) and UHT (an external compile-time code generator). For a C++20 engine that values determinism and keeps the RHI gameplay-agnostic, both are liabilities. This design keeps the *ideas* and swaps the *mechanisms*.

---

## 2. The Pivotal Decision — Object Base Layer

The entire top-level framework is a function of one decision: whether there is a unified object base, and how much of `UObject` it replicates.

| Option | What it is | Cost | Payoff |
|---|---|---|---|
| **A. Full `UObject` clone** | Reflection codegen (a UHT equivalent) + GC + CDO + `FArchive` | Enormous; effectively building half of UE. Contradicts the project's "no macro-heavy reflection" stance. | Maximum fidelity; automatic serialization/replication/editor. |
| **B. Lightweight `Object` base (CHOSEN)** | Unified base + runtime **class registry** (parent link / factory / property table) + **property-driven** serialization; **no GC**, replaced by deterministic ownership + generational handles. | Moderate; a one-time Object/Class/Archive layer (order of a few hundred lines). | UE's *architectural* dividends (uniform introspection, reflection-driven serialization, spawn-by-name, editor inspector) without the two heaviest mechanisms. |
| **C. No base** | Raw classes + an external type registry. | Zero ceremony. | Reflection / serialization / editor degrade into bespoke per-type code — exactly today's `SerializePrefabData() → std::string` trap. |

**Decision: Option B**, approved. Two deliberate divergences from UE define this layer:

1. **No GC — deterministic ownership + generational handles.** UE uses GC to make "pointer to a destroyed object" safe. RVX uses `unique_ptr` ownership (World → Level → Actor → Component) and `WeakObjectPtr<T>` (generation-tagged, validated on dereference) for all non-owning references. Dangling-safe, zero GC pauses, predictable behavior — the right trade for a real-time renderer.
2. **No UHT — runtime class registration.** A lightweight `RVX_CLASS(Type, Base)`-style registration (static initialization, not an external codegen tool) records: class name, parent, factory function, and property list. That is enough to support spawn-by-name, is-a checks, and property iteration — and property iteration is what buys prefab / scene save / undo / editor inspector for free.

The rest of this spec is built on Option B.

---

## 3. Layered Architecture

```text
┌────────────────────────────────────────────────────────────┐
│  L7  Gameplay↔Render boundary : PrimitiveComponent → Proxy   │  thread-isolation seam
│      Scene(render-side, persistent proxies) → SceneRenderer  │  (RHI never sees Actor)
│      → RenderGraph / RHI                                     │
├────────────────────────────────────────────────────────────┤
│  L6  Tick framework : TickFunction(group + prerequisites)    │  PrePhysics / DuringPhysics
│      → TickManager(World) → JobGraph (parallel)              │  / PostPhysics / PreRender
├────────────────────────────────────────────────────────────┤
│  L5  Subsystems : Engine/World scope, reflection auto-create │  missing dep / cycle = abort
│      + topological dependency ordering (fail-fast)           │
├────────────────────────────────────────────────────────────┤
│  L4  Containers : Engine ─owns─ World ─owns─ Level ─owns─ Actor│  single ownership chain
├────────────────────────────────────────────────────────────┤
│  L3  Object model : Actor / ActorComponent / SceneComponent /│  single model, zero
│       PrimitiveComponent   (no SceneEntity)                  │  duplication; one transform
├────────────────────────────────────────────────────────────┤
│  L2  Lifetime & ownership : ownership tree + pending-kill    │  deferred destroy, no GC
├────────────────────────────────────────────────────────────┤
│  L1  Object foundation : Object + Class registry +           │  ← Option B
│       WeakObjectPtr + Archive                                │
└────────────────────────────────────────────────────────────┘
```

---

## 4. Per-Layer Design

### L1 — Object foundation (`Core/Object`)

- **`Object`** — unified base. Provides `GetClass()`, weak-referenceability, and `Serialize(Archive&)`.
- **`Class`** — runtime metadata (name / parent / factory / property table). A global **`ClassRegistry`** can enumerate all registered classes; this is the substrate for L5 subsystem auto-instantiation and editor class listing.
- **`WeakObjectPtr<T>` / `ObjectHandle`** — generational handle replacing UE's GC-safe references. All non-owning references between objects go through it; a dereference validates the generation, so a stale reference resolves to null rather than crashing.
- **`Archive`** — serialization abstraction driven by reflected properties. One implementation backs prefab, scene save/load, undo, and (later) network snapshots.

### L2 — Lifetime & ownership

- **Single ownership chain** via `unique_ptr`, top-down. No `shared_ptr` object ownership (today's `shared_ptr<SceneEntity>` in `SceneManager` is removed).
- **Deferred destruction**: `Destroy()` marks pending-kill; actual teardown is flushed at a frame boundary — UE-equivalent semantics, but deterministic and without a GC sweep. Reuse the re-entrancy-safe dispatch-depth pattern already present in `Scene/Private/Actor.cpp`.

### L3 — Object model (the largest correction to the current code)

```text
Actor : Object
  ├─ vector<unique_ptr<ActorComponent>>   // the only component container
  └─ SceneComponent* RootComponent        // the only transform source of truth
ActorComponent : Object
SceneComponent : ActorComponent           // relative/world transform + attachment
PrimitiveComponent : SceneComponent        // bounds + render proxy + spatial registration
```

- **`SceneEntity` does not exist.** Identity is the Actor's object handle; the spatial index keys on `PrimitiveComponent` (via a proxy). One identity, one transform, one component container — directly flattening today's "dual everything" (`Scene/Include/Scene/SceneEntity.h` carries duplicate identity/transform/bounds plus two component containers).
- Transform dirty-propagation is **batched** and must **not** perform an RTTI `dynamic_cast` per node on the propagation path (today's hazard in `Scene/Private/SceneComponent.cpp`).

### L4 — Containers: Engine / World / Level

- **`Engine`** — top-level singleton; owns `EngineSubsystem`s, owns `World`s, drives the master loop.
- **`World`** — owns `Level`s, owns `WorldSubsystem`s, owns the render-side `Scene` (the `FScene` equivalent — a persistent render-proxy container, distinct from the gameplay `Scene/` module) + physics scene + spatial index; drives actor tick.
- **`Level`** — actor container / streaming unit. **v1 may be a single implicit Level**, but the seam is designed in (UE-style streaming grows from here).
- *(Optional)* a `GameInstance`-equivalent scope for cross-level session state — **deferred (YAGNI)** until level streaming or save sessions need it.

### L5 — Subsystem framework

- **`Subsystem`** base with scoped variants `EngineSubsystem` / `WorldSubsystem` (a GameInstance scope can be added later).
- **Reflection-driven auto-instantiation**: at World start, the `ClassRegistry` enumerates `WorldSubsystem` subclasses and instantiates them — eliminating today's manual `AddSubsystem<T>()` wiring. This is a direct dividend of the L1 base.
- **Fail-fast dependencies**: topological ordering; a **missing required dependency or a cycle aborts initialization with a clear error**. Today's "Falling back to registration order" (`Core/Include/Core/Subsystem/SubsystemCollection.h`) — i.e., starting up degraded — is a bug under this design and is removed.

### L6 — Tick framework

- **`TickFunction { bEnabled; TickGroup group; prerequisites[] }`**, registered with the World's `TickManager`.
- **Tick groups**: `PrePhysics → DuringPhysics (parallel with the physics step) → PostPhysics → PostUpdateWork → PreRender (extraction)`.
- Runs on the existing **`JobGraph`** (the equivalent of UE's task-graph parallel tick).
- **`BeginPlay` fires once, explicitly, at World begin** — not the current per-frame lazy invocation (`Scene/Private/SceneManager.cpp`).
- A fixed-timestep accumulator feeds physics for determinism.

### L7 — Gameplay ↔ Render boundary

- **`PrimitiveComponent::CreateRenderProxy()` → `PrimitiveRenderProxy`**, owned by the render-side `Scene`. Game-thread transform/visibility changes **enqueue incremental updates** to proxies, instead of full per-frame re-extraction (today's `Render/Private/Renderer/RenderSceneCollector.cpp`).
- **`SceneRenderer` consumes proxies only and never touches Actor/Component** — preserving the hard "RHI / RenderGraph stay gameplay-agnostic" constraint.
- The thread seam is a **command queue**: v1 may apply synchronously (single-threaded); it can later become a true game/render thread split with double-buffering **without changing the gameplay-facing API**. The interface is the UE interface even while the implementation starts synchronous — this is how "ideal target" stays landing-friendly.

---

## 5. Deliberate Divergences From UE (YAGNI / intentional)

| UE has | This design does | Rationale |
|---|---|---|
| Garbage collection | Deterministic ownership + generational handles | Real-time engine needs predictable, pause-free teardown |
| UHT code generation | Runtime class registration | Avoid an external codegen toolchain |
| Blueprint VM | Nothing (Lua/Sol2 already exists) | No duplicate scripting runtime |
| GameMode / Controller / Pawn | Only the Actor seam, not the implementations | That is the gameplay layer, above the engine-framework top level |
| Replication / networking in the object layer | Reflection hooks only | Reflected properties suffice later; not designed now |

---

## 6. Convergence Direction From Current Code (informative only)

`SceneEntity` collapses into `Actor`; dual containers → one; dual spatial identity → `PrimitiveComponent` proxies; full-extraction `RenderSceneCollector` → persistent render proxies. The execution sequencing for this is owned by `2026-05-30-engine-program-plan-v2.md` (SP0-Object → SP1a–d → SP-Proxy-v1); the older `2026-05-30-ecs-ue-migration-completion-design.md` increments are early stepping-stones folded into SP1, not a binding target.

---

## 7. Out of Scope

- The step-by-step migration from the current code (owned by the ECS migration completion spec).
- Gameplay framework (GameMode/Controller/Pawn), Blueprint-equivalent VM, networking/replication implementation.
- Material system, pipeline/PSO, asset system, render-pass wiring — separate sub-projects in the master plan.
- Exact public interface shapes of L1 types and the L7 command-queue contract (deferred to per-layer drill-down specs; see Open Decisions).

---

## 8. Acceptance Criteria (what "this design is realized" means)

- A single `Object` base with a runtime `Class` registry; spawn-by-name and is-a checks work.
- `WeakObjectPtr<T>` dereference is dangling-safe via generation checks; there is no garbage collector.
- Exactly one object model — `Actor` / `ActorComponent` / `SceneComponent` / `PrimitiveComponent`; no `SceneEntity`, no duplicate identity/transform/bounds, one component container, one transform source of truth.
- `Engine → World → Level → Actor` is a single `unique_ptr` ownership chain; non-owning references use handles; destruction is deferred to a frame boundary.
- `WorldSubsystem`s are auto-instantiated via reflection; dependency resolution is fail-fast (missing dependency or cycle aborts initialization).
- A tick framework with groups + prerequisites runs on the job graph; `BeginPlay` fires once explicitly.
- `PrimitiveComponent` creates a render proxy owned by the render `Scene`; `SceneRenderer` consumes proxies only; RHI/RenderGraph never reference Actor/Component; the gameplay→render seam is a command queue (synchronous in v1, threadable later).
- Serialization, prefab, and undo are driven by reflected properties through one `Archive` mechanism.

---

## 9. Open Decisions (deferred to per-layer drill-down)

- Exact public shapes of `Object` / `Class` / `WeakObjectPtr` / `Archive`, and the form of the registration API (macro vs. fluent static registration).
- Property-reflection representation (typed property descriptors vs. a visitor/`Serialize` pass).
- The L7 render-proxy command-queue contract, and the precise v1-synchronous → threaded upgrade path.
- Whether `Level` and a `GameInstance`-scope subsystem land in v1 or later.
- How the spatial index keys on `PrimitiveComponent` proxies (relates to the audit's P0.8 spatial-unification work).
