# M2 RHI Best-Practice Design Review

**Status:** Reviewed; revisions incorporated; M1/Task 20 entry gate satisfied
**Review target:** `2026-07-26-m2-tier1-rhi-proof-master-plan.md` and its
shared/DX12/Vulkan/Metal closure plans
**Review date:** 2026-07-26
**Code implementation:** Task 21/CH1 is the next approved implementation slice

## Verdict

The original M2 direction is correct: normalize shared meaning, validate it
before native calls, prove it on real DX12/Vulkan/Metal devices, and retain
DX11/OpenGL as compatibility backends.

The design is approved only after the revisions below. The original
state-snapshot proposal was not expressive enough for a production RHI because
it treated resource state as if it also described execution dependency,
memory dependency, content validity, and queue ownership. Those concepts must
be related but distinct.

## Evidence From the Current Engine

1. `RHIBufferBarrier` and `RHITextureBarrier` currently contain only
   `stateBefore`, `stateAfter`, and a range.
2. DX12, Vulkan, and OpenGL skip barriers when the two states are equal.
   Therefore the shared contract cannot express a same-state write-after-write
   or write-before-read memory dependency.
3. Vulkan derives both stage and access masks from one `RHIResourceState` and
   ignores queue-family ownership in the current barrier path.
4. RenderGraph already records access type, desired state, shader stages, and
   subresource/range information. It has enough source information to compile
   a richer dependency; the RHI barrier loses that information.
5. Transient resources retain their native allocation across pool reuse but
   the graph resets their logical state to `Undefined`.
6. DX12 descriptor tables are currently created with broad `DATA_STATIC`
   assumptions, while descriptor-set validation proves supplied entries but
   not complete table initialization.
7. The GPU-driven pipeline failure is a real shader-interface mismatch, not a
   reason to special-case DX12 input-layout creation.

## Required Design Revisions

### 1. Replace state-only barriers with scoped dependencies

The production path must carry, directly or through equivalent compact types:

- source and destination execution scope;
- source and destination memory access;
- resource usage/layout before and after;
- buffer range or texture subresource range;
- source and destination physical submission domain when ownership changes;
- discard/content-valid intent;
- barrier kind where transition, memory/UAV, and aliasing differ.

`RHIResourceState` may remain as a compatibility projection during migration,
especially for DX11/OpenGL, but it cannot remain the sole RenderGraph-to-RHI
dependency contract. Equal layouts/states do not imply that no barrier is
needed.

### 2. Separate access state from content validity

`Undefined` must not mean both “unknown native state” and “old contents may be
discarded.”

- a resource or pool lease retains its actual realized access/layout state;
- content validity is stored separately;
- a graph may request discard only when the first use does not require prior
  contents;
- Vulkan may translate discard to `VK_IMAGE_LAYOUT_UNDEFINED`;
- DX12 still receives the real before state and may use discard-style render
  behavior without inventing a native `Undefined` state.

### 3. Bound state authority to resource ownership

RenderGraph is authoritative only while compiling and executing one graph.
Across graphs:

- persistent owners store the last exported access snapshot;
- transient leases store it while pooled;
- uploads, presentation, ray-tracing builds, and other non-graph producers
  commit their final snapshot explicitly;
- queue identity uses the frozen M1 `GPUQueueDomain` mapping, not a second
  logical-queue model;
- backend trackers validate the supplied plan but do not become a global
  raw-pointer-keyed authority.

### 4. Carry a compact immutable shader interface

The RHI shader object should own a compact `RHIShaderInterface`, not the full
compiler reflection graph. The interface contains:

- canonical vertex locations and formats;
- optional D3D semantic name/index for diagnostics and DXIL validation;
- resource binding signature;
- push/root constant ranges when present;
- stable interface hash.

Human-readable names may be stripped in shipping builds, but the signature
needed for pipeline validation and cache identity remains. Explicit location
is the portable identity; HLSL semantic text is not.

### 5. Split descriptor completeness from resource-data volatility

Two independent questions must be modeled:

1. Is the submitted descriptor snapshot complete and immutable while in
   flight?
2. May the resource data referenced by that snapshot change while recorded
   work executes?

The base tier requires every binding/array element to be initialized with a
real or typed fallback resource. Partial/null binding is optional capability,
not a baseline shortcut.

The current replacement-and-retirement model is retained. On DX12, completed
tables may therefore use stable descriptors, while SRV/CBV/UAV data defaults
to correctness-safe volatility according to actual use. Do not mark
`DESCRIPTORS_VOLATILE` merely because the referenced resource changes, and do
not publish DX12 range flags as shared RHI concepts.

### 6. Add three-backend design checkpoints before freezing shared contracts

DX12 remains the first real red-to-green runtime target because it supplies the
current reproducible failure. It must not be allowed to freeze a shared
contract alone.

For each shader, descriptor, and dependency contract:

- common validation tests must pass;
- DX12 native translation must be reviewed;
- Vulkan and Metal translation/structural tests must prove the same contract
  is representable;
- real-device Vulkan and Metal closure may follow afterward.

Any backend that requires a meaning change returns the contract to shared
review and reruns already-green backend tests.

### 7. Use layered conformance rather than a new monolith

Use a shared case catalog/report library with thin runners:

1. common contract tests;
2. backend translation tests;
3. native real-device validation;
4. bounded integration.

Reuse existing backend test targets where practical. DX12 Debug Layer and
normal Vulkan/Metal validation run in required developer gates. Expensive
DX12 GPU-Based Validation runs on bounded fixtures or nightly gates.

Pass criteria are zero unexpected warnings and errors. Any allowlist entry is
keyed by backend/native ID, justified, owned, and expiring; text wildcards are
not accepted.

## Retained Decisions

- GPU-driven rigid and skinned shader entry interfaces remain separate.
- GPU-driven DX12 red-to-green is an M2 regression, not proof that the full
  renderer is cross-backend complete.
- Optimized clear values remain optional resource-creation hints and part of
  resource compatibility/hash only when present.
- Descriptor replacement never mutates in-flight native memory.
- Direct-draw fallback remains available and becomes observable.
- Real-device evidence, exact backend identity, and fail-closed capability
  reporting remain mandatory.
- Editor, bindless, renderer feature expansion, residency, and performance
  tuning remain outside M2.

## Rejected Alternatives

- one global raw-pointer resource-state database;
- treating `stateBefore == stateAfter` as unconditional no-work;
- mapping one coarse state enum directly to all Vulkan stage/access/layout
  requirements;
- marking all DX12 descriptor ranges volatile or static;
- requiring descriptor indexing/partial binding for the base tier;
- making Metal use untracked hazards by default before explicit
  synchronization is proven;
- rewriting every RHI factory to a new result type in this milestone;
- freezing the shared interface after DX12 implementation only.

## Official Practice References

- Microsoft Direct3D 12 resource barriers and Root Signature 1.1 descriptor
  volatility rules
- Microsoft Direct3D 12 GPU-Based Validation guidance
- Khronos Vulkan synchronization and synchronization2 examples/specification
- Apple Metal resource synchronization, hazard tracking, and vertex descriptor
  documentation
- Epic Games Render Dependency Graph resource, transition, and validation
  guidance

These references guide semantics; RenderVerseX does not copy any one engine's
public API shape.

## Approval Boundary

The revised documents are ready for implementation planning. Source code work
still requires the M1 exit gate in the master plan and a deliberate start of
Task 20/21. This review itself authorizes no code modification.
