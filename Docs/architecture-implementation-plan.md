# RenderVerseX Architecture Implementation Plan

Date: 2026-07-02
Branch: `codex/architecture-implementation`
Base: `engine-remediation` at `6b09c4d`
Worktree: `.claude/worktrees/architecture-implementation`

## Objective

Align the current RenderVerseX implementation with the RVX-NG v1.1 north-star:
a production-grade C++20 realtime rendering/game engine framework foundation
centered on a modern RenderGraph, a multi-backend RHI, and clear data
boundaries. The near-term target is not to finish every future engine feature in
one branch; it is to make samples, editor, runtime scene, resource, shader, and
rendering workflows share verifiable contracts, honest capabilities, fallback
paths, diagnostics artifacts, and automated gates.

This plan is deliberately incremental. It does not rewrite the engine in one
step. Each phase must leave the branch buildable and must add or preserve an
automated gate before the next phase starts.

The implementation target is not "finish every future RVX-NG capability in this
branch." The target is to make the current framework move monotonically toward
the v1.1 foundation shape: RenderGraph-driven, RHI-pluggable,
scene-data-decoupled, resource-pipeline-aware, diagnostics-first, test-gated,
and shared by editor and runtime without collapsing their responsibilities.

## North-Star Alignment Tracks

The active implementation branch maps the RVX-NG v1.1 goal into eight practical
tracks. Every architecture edit should strengthen at least one of these tracks
without weakening another:

1. **Clear engine layering**: Core, RHI, Render, Scene, Resource,
   ShaderCompiler, Runtime, and Editor keep stable responsibilities. Module
   boundaries and include direction are checked by scripts and CTest coverage,
   not left as convention.
2. **Consistent multi-backend RHI**: `IRHIDevice`, backend capabilities,
   resource lifetimes, fallback behavior, and diagnostics stay consistent across
   DX11, DX12, Vulkan, Metal, and OpenGL. Unsupported paths must report why
   they are unsupported instead of pretending success.
3. **RenderGraph as the rendering hub**: modern GPU work is expressed through
   RenderGraph passes, resources, barriers, async scheduling, queue sync,
   diagnostics snapshots, and future visualization exports.
4. **Stable Scene-to-Render data flow**: authoring/gameplay state moves toward
   `Actor/Component -> Scene Snapshot -> RenderProxy -> RenderScene`, so Render
   consumes immutable render data instead of gameplay objects.
5. **Production resource and shader pipeline**: source assets, cooked artifacts,
   runtime loading, GPU residency, shader compilation/reflection, material data,
   and hot reload must converge on explicit contracts and observable failures.
6. **Systematic modern rendering features**: PBR, shadows, IBL, postprocess,
   GPU-driven rendering, instancing, ray tracing, and fallback render paths must
   be introduced behind capability gates and validation tests.
7. **Editor/runtime split with shared core**: Editor owns authoring, inspection,
   resource management, and visualization; Runtime owns lightweight loading and
   stable execution. Both share the same core scene, asset, and rendering
   contracts.
8. **Diagnostics, tests, and architecture gates**: every critical system exposes
   validation tests, contract checks, fallback diagnostics, counters, or
   artifacts that can be inspected by CI and tools.

## Implementation Success Criteria

This branch is successful when it changes the current engine in the direction
of the v1.1 foundation while preserving a buildable, testable state after every
phase. A phase is complete only when the new behavior is represented by all of
the following:

- a public or tool-facing contract, not only private implementation state;
- explicit supported, fallback, unsupported, or skipped semantics where
  platform/backend/runtime policy differences can occur;
- versioned diagnostics or artifacts for behavior that tools or CI need to
  inspect;
- automated coverage through CTest, architecture scripts, or a documented smoke
  gate;
- no new dependency edge that violates the RVX-NG layer direction.

Long-term RVX-NG capabilities remain valid goals, but they should be promoted
into implementation phases only when the required contracts, fallbacks, and
gates can land with them. This keeps the branch focused on framework maturity
instead of accumulating isolated feature demos.

## Current Baseline

The implementation branch starts from the committed `engine-remediation`
history, not from uncommitted local workspace edits. The current code already
has several important foundation pieces:

- Engine and World subsystem lifetimes through `EngineSubsystem`,
  `WorldSubsystem`, and `SubsystemCollection`.
- Modern RHI contracts for explicit barriers, descriptors, uploads, heaps,
  queries, backend capabilities, and multi-backend validation.
- RenderGraph validation and diagnostics for pass/resource hazards.
- Render proxy data and a scene bridge, with legacy collector fallback still
  present.
- Actor/ActorComponent migration scaffolding while legacy Component support
  remains.
- Resource manager, cook tooling, GPU upload/resource managers, and render
  honesty tests.

The main architecture risks to burn down are:

- Render private includes currently reach up into Engine, World, Resource, and
  Scene implementation headers.
- Render extraction is not yet a hard contract; `SceneRenderer` can still fall
  back to legacy scene collection.
- Actor/ActorComponent and legacy Component paths coexist.
- Multiple global service access patterns coexist with subsystem lifetime
  ownership.
- Resource loading has async/cook hooks but is not yet a full shipping runtime
  package/VFS/residency model.

## Phase Order

### P0: Baseline and Gates

Goal: make the migration measurable before changing architecture.

Tasks:

- Keep the worktree clean and isolated from the main workspace.
- Record this implementation plan.
- Add a minimal architecture baseline runner for representative unit tests.
- Use the baseline runner before and after each later phase.

Gate:

```powershell
.\Scripts\run_architecture_baseline.ps1 -BuildDir build\win_x64_debug -Configuration Debug
```

### P1: Module Boundary Scaffolding

Goal: make the intended dependency direction visible to the build and tests.

Tasks:

- Add `Docs/module-boundaries.json` as the source of truth for allowed and
  tolerated legacy include edges.
- Add `Scripts/check_module_boundaries.py` and CTest coverage through
  `Architecture.ModuleBoundaries`.
- Introduce `RenderContracts` as a header-only render contract boundary for
  render-only snapshot data.
- Move pure data types used across Scene/World and Render behind that boundary.
- Start removing Render private include dependence on World/Scene headers.
- Add a boundary validation check that can fail if Render reaches across layers.

Gate:

- Baseline runner passes.
- Boundary check reports no new upward Render includes.
- Legacy edges are reported explicitly and can be tightened with
  `--fail-on-legacy`.

### P2: Extract/Proxy Default Path

Goal: make Render consume immutable scene snapshots instead of gameplay objects.

Tasks:

- Make legacy render collection fallback opt-in on `SceneRenderer` so the
  default path cannot silently leave proxy extraction.
- Move scene-to-render proxy building to the dedicated `RenderExtraction`
  module.
- Keep `RenderScene` and `SceneRenderer` consuming render-only data.
- Make legacy fallback observable and keep it outside the default path.

Gate:

- Render proxy bridge tests pass.
- Normal sample/editor paths report proxy path usage and zero legacy fallback
  frames unless explicitly requested by a compatibility test.

### P3: Actor/Component Model Convergence

Goal: prevent long-term dual ownership between `ActorComponent` and legacy
`Component` containers.

Tasks:

- Add `Architecture.PhaseGates` coverage to reject new production
  `MeshRendererComponent` instantiation.
- Define `Actor` as the authoring facade and component lifetime owner.
- Keep legacy Component as compatibility only.
- Route new model/prefab instantiation through ActorComponent paths.
- Add migration tests for legacy rejection, serialization, prefab restore, and
  scene hierarchy compatibility.

Gate:

- ActorComponent and ResourceInstantiation validation pass.
- New code paths do not add legacy components unless a compatibility test
  explicitly requests it.

### P4: Service and Lifetime Unification

Goal: reduce unowned global state.

Tasks:

- Add `Architecture.PhaseGates` coverage so `Services::Get` and
  `SystemManager` do not spread outside compatibility/foundation surfaces.
- Prefer EngineSubsystem and WorldSubsystem ownership for runtime services.
- Restrict `Services`, `SystemManager`, and singleton access to documented
  compatibility or foundation use.
- Add shutdown/multi-world isolation tests.

Gate:

- Subsystem dependency validation catches missing dependencies and cycles.
- Multi-world service access remains isolated.

### P5: Resource, Cook, and Runtime Closure

Goal: make asset loading suitable for shipping builds.

Tasks:

- Replace busy-wait load waiting with condition/future-based completion.
- Add `Architecture.PhaseGates` coverage to reject `ResourceHandle` busy-wait
  regressions and keep GPU resource budget controls visible.
- Finish hot reload/file watcher behavior or make unsupported states explicit.
- Separate source assets, cooked artifacts, runtime package/VFS access, and GPU
  residency.
- Track memory and upload budgets through Resource and GPUResourceManager.

Gate:

- Cooked assets load without source asset reads in shipping-style tests.
- Missing/invalid assets produce explicit fallback diagnostics.

### P6: Quality and Capability Gates

Goal: make production quality enforceable.

Tasks:

- Add `Architecture.PhaseGates` coverage for representative capability,
  unsupported, fallback, and proxy-default honesty tests.
- Map each capability bit to a contract test.
- Keep render honesty tests for unsupported/fallback paths.
- Maintain unit, GPU, visual golden, and cross-backend lanes.
- Add performance/memory counters as non-visual regression artifacts.

Gate:

- Capability tests distinguish supported, unsupported, fallback, and skipped
  with explicit reasons.
- Visual and GPU lanes are documented with required hardware assumptions.

## Continuation Tracks After P6

The original P0-P6 sequence burns down the highest-risk architecture debt in the
current implementation. It is not the end of the north-star work. Once these
phases are green, continue in the following order unless a blocking regression
requires a narrower fix first.

### P7: Frame Diagnostics and Tooling Surface

Goal: make RenderGraph, SceneRenderer, RHI capability, GPU resource, and pass
execution diagnostics consumable by editor/profiler/frame-debugger tooling.

Tasks:

- Promote frame diagnostics from test-only structs to stable tool-facing
  snapshots with versioned fields.
- Export RenderGraph pass/resource/queue timelines in a format that can feed a
  future RenderGraphViz or FrameDebugger panel.
- Keep GPUResourceManager, GPU-driven, ray tracing, and postprocess execution
  plans visible through `SceneRendererFrameDiagnostics`.
- Add tests proving that diagnostics are populated from real runtime state, not
  from source-string checks alone.

Gate:

- Diagnostics tests exercise a constructed frame and verify pass/resource,
  fallback, capability, and budget values through public APIs.

### P8: Resource, Shader, and Runtime Package Closure

Goal: make resource and shader loading behave like a production runtime path,
not only a demo-friendly loose-file path.

Tasks:

- Separate source asset access, cooked artifact loading, runtime package/VFS
  reads, and GPU residency decisions.
- Tie shader reflection, material layout, PSO cache keys, and resource metadata
  into stable cooked/runtime contracts.
- Make missing, stale, unsupported, or uncooked assets produce structured
  diagnostics and deterministic fallback behavior.
- Add shipping-style tests that prove runtime loads do not accidentally depend
  on source assets.

Gate:

- Cooked-resource tests, shader cache tests, and GPU residency tests pass
  without source-asset reads.

### P9: Editor/Runtime Shared-Core Boundary

Goal: let Editor and Runtime share the same scene, resource, and rendering
contracts while keeping their responsibilities separate.

Tasks:

- Define explicit app-mode seams for runtime, editor, preview, and PIE-style
  worlds.
- Keep editor-only UI, inspector, gizmo, and asset-browser code outside runtime
  modules.
- Route viewport rendering through the same Scene-to-Render snapshot and
  RenderGraph path that runtime uses.
- Add tests or smoke gates proving runtime modules do not depend on editor-only
  headers or services.

Gate:

- Module-boundary checks reject editor-to-runtime leakage in both directions
  that would break the shared-core model.

### P10: Modern Rendering Capability Closure

Goal: grow PBR, shadows, IBL, postprocess, GPU-driven, instancing, and ray
tracing as a coherent capability matrix instead of isolated feature toggles.

Tasks:

- Map each rendering feature to capability bits, fallback paths, diagnostics,
  tests, and visual/golden coverage where appropriate.
- Keep unsupported high-end paths explicit and non-fatal.
- Route new passes through RenderGraph and keep resource/state ownership inside
  the graph.
- Add performance and memory counters for features that change frame cost.

Gate:

- Capability tests and render honesty tests prove supported, fallback,
  unsupported, and skipped states for each promoted feature.

### P11: Resource Hot Reload and Tooling Contract Closure

Goal: make editor hot reload a source-asset tooling feature with explicit
runtime-policy diagnostics, instead of a silent boolean that can appear enabled
in cooked or packaged runtime modes.

Tasks:

- Expose a `ResourceHotReloadDiagnostic` through `ResourceManager` so tools and
  tests can distinguish disabled, active, unsupported-policy, and uninitialized
  states.
- Reject hot reload when the active resource runtime policy requires cooked
  artifacts or runtime packages.
- Register source-loaded editor resources with `HotReloadManager` and the
  underlying `FileWatcher` when hot reload is active.
- Keep watcher statistics stable enough to feed editor tooling and validation
  tests.

Gate:

- Resource runtime policy tests prove cooked runtime hot reload is rejected
  explicitly and editor/source loads are tracked by hot reload diagnostics.
- `Architecture.PhaseGates` checks keep the hot-reload policy contract,
  diagnostics, and watcher id stability visible.

### P12: Render Proxy Snapshot Contract Closure

Goal: make the Scene-to-Render bridge publish a stable frame-snapshot contract
that tools, RenderScene, and future threaded extraction can verify directly.

Tasks:

- Version `RenderProxySnapshot` with schema, sequence, completeness, and source
  counts without leaking Scene/World types into Render contracts.
- Have `RenderProxySceneBridge` mark successful snapshots complete and rejected
  snapshots incomplete, while reporting the same metadata through its result.
- Preserve the consumed snapshot metadata inside `RenderScene` diagnostics.
- Add tests proving bridge output, bridge result, and RenderScene consumption
  agree on snapshot sequence and counts.

Gate:

- RenderScene validation proves proxy snapshots are complete, versioned,
  sequence-tracked, and consumed by RenderScene without depending on gameplay
  objects.
- `Architecture.PhaseGates` keeps the snapshot metadata contract visible.

### P13: RHI Capability Report Contract Closure

Goal: make backend capabilities consumable as a versioned diagnostic matrix,
not only as scattered boolean flags.

Tasks:

- Expose a schema-versioned `RHICapabilityReport` derived from
  `RHICapabilities`.
- Classify core backend features as supported, emulated, or unsupported with
  required capability strings and diagnostic messages.
- Preserve the existing `ValidateRHICapabilities` consistency check and surface
  its result through the report.
- Add tests proving the report classifies supported, emulated, unsupported, and
  invalid capability states.

Gate:

- RHI contract validation proves report entries map to concrete capability bits
  and carry validation failures.
- `Architecture.PhaseGates` keeps the report schema and classification contract
  visible.

### P14: RHI Device Capability Diagnostics Surface

Goal: make the versioned RHI capability matrix available from every RHI device
through a stable public diagnostics surface, so tools do not need to rebuild
backend capability state by hand.

Tasks:

- Add a default `IRHIDevice::GetCapabilityReport()` implementation that derives
  the report from `GetCapabilities()` for all backends.
- Add a stable human-readable `ExportRHICapabilityReportText()` path for logs,
  editor panels, and future profiler/frame-debugger integration.
- Add RHI contract tests proving a device-level report uses the public device
  capabilities and that text export includes schema, backend, validation,
  summary, and feature classification.

Gate:

- RHI contract validation proves the device surface returns the same
  schema-versioned capability matrix as the lower-level report builder.
- `Architecture.PhaseGates` keeps the device-level diagnostics entry point and
  tool text export visible.

### P15: Renderer Tool Diagnostics RHI Capability Snapshot

Goal: carry the device-level RHI capability matrix into renderer tool
diagnostics, so Editor, Profiler, and future FrameDebugger surfaces can explain
render feature decisions with the same backend facts used by RHI validation.

Tasks:

- Extend `SceneRendererToolDiagnosticsSnapshot` with a schema-bumped RHI
  capability report payload and availability flag.
- Capture `IRHIDevice::GetCapabilityReport()` during diagnostics refresh when
  a render context or focused test device is available.
- Export the report summary and feature entries through readable diagnostics
  text and the machine-readable diagnostics manifest.
- Add tests proving the snapshot, text export, and manifest carry schema,
  backend, validation, summary, and feature classification.

Gate:

- Render pass validation proves renderer tool diagnostics preserve the RHI
  capability matrix from the device surface.
- Render scene validation protects draw-list material mode inference from the
  material source when explicit per-submesh modes are absent, keeping masked
  ray tracing alpha resources visible to diagnostics and descriptor budgets.
- `Architecture.PhaseGates` keeps the renderer diagnostics RHI snapshot,
  manifest export, and schema bump visible.

### P16: RenderGraph Diagnostics JSON Contract Closure

Goal: make RenderGraph diagnostics directly consumable by Editor,
FrameDebugger, Profiler, and CI artifact tooling without parsing human-readable
text or Graphviz DOT.

Tasks:

- Add a stable `RenderGraph::ExportDiagnosticsJson()` and save path that emits
  schema, compile stats, memory counters, pass/resource details, execution
  order, planned queue batches, planned syncs, actual queue batches, and actual
  syncs.
- Route the RenderGraph JSON artifact through `SceneRenderer` tool diagnostics
  alongside the existing scene-renderer text, RenderGraph DOT, and RenderGraph
  text diagnostics artifacts.
- Bump the tool diagnostics schema for the new machine-readable RenderGraph
  artifact and include the saved JSON path in diagnostics manifests.
- Add tests proving RenderGraph JSON and SceneRenderer artifact export are
  stable enough for tools.

Gate:

- RenderGraph validation proves JSON export and save paths expose schema,
  pass/resource, memory, schedule, and queue-sync data.
- Render pass validation proves SceneRenderer tool artifacts include
  `.rendergraph.json` and advertise it in the manifest.
- `Architecture.PhaseGates` keeps the RenderGraph JSON API, artifact path, and
  schema bump visible.

### P17: Tool Diagnostics Artifact Summary Contract

Goal: make SceneRenderer diagnostic captures self-describing for Editor,
Profiler, FrameDebugger, and CI artifact tooling, without requiring each tool to
hard-code every manifest field.

Tasks:

- Add a schema-versioned artifact summary JSON export that lists diagnostic
  artifact ids, kind names, content types, saved flags, and paths.
- Save the artifact summary alongside scene-renderer text, RenderGraph DOT,
  RenderGraph text, RenderGraph JSON, and the diagnostics manifest.
- Bump the tool diagnostics schema and advertise the artifact summary path from
  the manifest.
- Add tests proving empty summaries, saved summaries, manifest links, and file
  contents are stable.

Gate:

- Render pass validation proves `ExportToolDiagnosticsArtifactSummaryJson()`,
  `.diagnostics-artifacts.json`, artifact counts, saved counts, and manifest
  links are stable.
- `Architecture.PhaseGates` keeps the artifact summary schema, export API,
  saved artifact path, and manifest link visible.

### P18: Tool Diagnostics Artifact Integrity Contract

Goal: make tool diagnostics artifacts verifiable by CI and external tooling, not
just discoverable by filename.

Tasks:

- Add aggregate artifact integrity fields for saved primary artifact count,
  all-saved state, and total primary artifact bytes.
- Track per-artifact existence and byte sizes for scene-renderer text,
  RenderGraph DOT, RenderGraph text, RenderGraph JSON, and the diagnostics
  manifest.
- Export the integrity data through the artifact summary JSON and fast manifest
  overview.
- Refresh integrity after final manifest writes so summary data reflects the
  final saved files.

Gate:

- Render pass validation proves saved artifact results expose all-saved state,
  file existence, per-artifact byte sizes, and aggregate byte totals.
- `Architecture.PhaseGates` keeps the artifact integrity fields, file-size
  inspection, JSON fields, and tests visible.

### P19: Tool Diagnostics Capture Identity Contract

Goal: let Editor, Profiler, FrameDebugger, and CI tooling correlate every saved
diagnostics artifact bundle back to a stable frame capture identity.

Tasks:

- Add deterministic capture metadata to saved artifact results: output
  directory, base name, frame index, diagnostics schema versions, RenderGraph
  pass count, and RenderGraph resource count.
- Export the capture metadata through both the diagnostics manifest and the
  artifact summary JSON.
- Bump the tool diagnostics and artifact summary schemas for the new capture
  identity contract.
- Add tests proving capture metadata is populated and exported for saved
  artifact bundles.

Gate:

- Render pass validation proves artifact results, manifests, and summaries carry
  base name, frame index, schema versions, and RenderGraph capture scale.
- `Architecture.PhaseGates` keeps capture metadata fields, schema bumps,
  JSON export fields, and tests visible.

### P20: Tool Diagnostics Portable Artifact Paths

Goal: make saved diagnostics bundles portable after CI archival or manual copy,
instead of requiring tools to parse machine-local absolute paths.

Tasks:

- Add bundle-relative artifact paths to `SceneRendererToolDiagnosticsArtifactResult`.
- Export `relativePath` for each artifact through the diagnostics manifest and
  artifact summary JSON.
- Bump the tool diagnostics and artifact summary schemas for the portable path
  contract.
- Add tests proving relative paths stay stable for scene-renderer text,
  RenderGraph DOT, RenderGraph text, RenderGraph JSON, manifest JSON, and the
  artifact summary JSON.

Gate:

- Render pass validation proves saved artifact results, manifests, and summaries
  expose stable relative paths.
- `Architecture.PhaseGates` keeps schema bumps, relative-path fields, JSON
  export, and tests visible.

### P21: Tool Diagnostics Artifact Content Hashes

Goal: make saved diagnostics bundles content-verifiable after CI archival,
manual copy, or external tool ingestion, instead of relying only on existence
and byte sizes.

Tasks:

- Add stable content hashes for every primary diagnostics artifact:
  scene-renderer text, RenderGraph DOT, RenderGraph text, RenderGraph JSON, and
  diagnostics manifest JSON.
- Export `contentHash` through the artifact summary JSON, which acts as the
  bundle verification index.
- Bump the tool diagnostics and artifact summary schemas for the hash contract.
- Keep the diagnostics manifest focused on bundle identity and artifact
  location, avoiding self-referential manifest hashing.
- Add tests proving hashes are populated and exported for saved artifact
  bundles.

Gate:

- Render pass validation proves saved artifact results and artifact summaries
  expose stable content hashes.
- `Architecture.PhaseGates` keeps schema bumps, content-hash fields, hash
  calculation, JSON export, and tests visible.

### P22: Tool Diagnostics Primary Artifact Bundle Hash

Goal: let CI, Editor, Profiler, and FrameDebugger tooling compare an entire
diagnostics primary artifact set with one stable value, while avoiding
self-referential manifest or summary hashing.

Tasks:

- Add a `primaryArtifactBundleHash` derived from capture identity, schema
  versions, RenderGraph capture scale, primary artifact relative paths, byte
  sizes, and content hashes.
- Export the bundle hash through the artifact summary JSON as the bundle
  verification index.
- Bump the tool diagnostics and artifact summary schemas for the aggregate hash
  contract.
- Keep the diagnostics manifest free of the aggregate hash because the manifest
  is itself one of the primary artifacts.
- Add tests proving the aggregate hash is populated and exported for saved
  artifact bundles.

Gate:

- Render pass validation proves saved artifact results and artifact summaries
  expose a stable primary artifact bundle hash.
- `Architecture.PhaseGates` keeps schema bumps, bundle-hash fields,
  calculation, JSON export, and tests visible.

### P23: Tool Diagnostics Primary Artifact Bundle Validation

Goal: let CI and external tools re-read a saved diagnostics bundle and verify
that every primary artifact still matches the recorded byte size, content hash,
and aggregate bundle hash.

Tasks:

- Add a schema-versioned public validation result with per-artifact entries,
  actual byte sizes, actual content hashes, mismatch flags, and diagnostics.
- Expose `SceneRenderer::ValidateToolDiagnosticsArtifacts()` as a pure
  validation API over a saved artifact result.
- Recompute the actual primary artifact bundle hash from files on disk and
  compare it to the recorded bundle hash.
- Add tests proving intact bundles validate and mutated artifacts fail with
  size/hash/bundle mismatches.

Gate:

- Render pass validation proves valid bundles pass validation and modified
  artifacts fail validation.
- `Architecture.PhaseGates` keeps the validation schema, public API, actual
  bundle-hash comparison, and tests visible.

### P24: Tool Diagnostics Artifact Validation JSON

Goal: make artifact validation results consumable by CI dashboards, editor
panels, profiler capture importers, and external tools without linking against
engine code.

Tasks:

- Add a stable JSON export for
  `SceneRendererToolDiagnosticsArtifactValidationResult`.
- Include aggregate validity, bundle-hash comparison, checked/valid counts,
  actual total bytes, expected/actual bundle hashes, and per-artifact
  mismatch diagnostics.
- Add a save helper for writing validation reports as sidecar CI artifacts.
- Add tests proving both valid and mutated bundles export useful validation
  JSON.

Gate:

- Render pass validation proves validation JSON export/save works for valid
  bundles and failed validation reports.
- `Architecture.PhaseGates` keeps validation JSON APIs, aggregate fields,
  per-artifact diagnostics, and tests visible.

### P25: Tool Diagnostics Validation Sidecar Artifact

Goal: make saved diagnostics bundles carry their own validation report sidecar
so CI and external tools can inspect bundle integrity without re-running engine
code.

Tasks:

- Save a deterministic `.diagnostics-validation.json` sidecar from
  `SaveToolDiagnosticsArtifacts()` after primary artifacts and hashes are
  stable.
- Keep the validation sidecar outside the primary artifact bundle hash to avoid
  self-referential validation.
- Expose the validation report as a secondary artifact in the artifact summary
  with saved state, existence, byte size, content hash, relative path, and path.
- Add tests proving the sidecar path, content, summary entry, and hash are
  stable.

Gate:

- Render pass validation proves saved artifact bundles include the validation
  sidecar and that the sidecar content matches the validation JSON export.
- `Architecture.PhaseGates` keeps schema bumps, sidecar fields, stable naming,
  summary export, and tests visible.

### P26: Tool Diagnostics Validation Capture Identity

Goal: make validation sidecars self-identifying so CI, editor panels, profiler
capture importers, and external tools can correlate a validation report with
the frame and RenderGraph snapshot it validates without separately parsing the
artifact summary or manifest.

Tasks:

- Copy capture metadata from saved artifact results into
  `SceneRendererToolDiagnosticsArtifactValidationResult`.
- Export a `capture` object in validation JSON with base name, output
  directory, frame index, schema versions, RenderGraph pass count, and
  RenderGraph resource count.
- Bump the tool diagnostics and artifact validation schemas for the expanded
  validation contract.
- Add tests proving validation results and validation JSON carry frame/base
  identity, schema versions, and RenderGraph counts.

Gate:

- Render pass validation proves the validation result and JSON sidecar are
  independently identifiable.
- `Architecture.PhaseGates` keeps schema bumps, capture fields, JSON export,
  and tests visible.

### P27: Tool Diagnostics Summary Validation Verdict

Goal: make the artifact summary the first-stop tool index for diagnostics
captures by exposing the validation sidecar verdict directly in the summary,
without requiring CI, editor panels, profiler importers, or FrameDebugger tools
to open the validation JSON before showing bundle health.

Tasks:

- Copy validation aggregate results from
  `SceneRendererToolDiagnosticsArtifactValidationResult` into the saved
  artifact result after validation runs.
- Export validation verdict fields inside the artifact summary
  `validationReport`: result availability, aggregate validity, bundle-hash
  match, checked/valid counts, actual total bytes, and expected/actual bundle
  hashes.
- Bump the tool diagnostics and artifact summary schemas for the expanded
  summary contract.
- Add tests proving saved artifact results and artifact summary JSON expose the
  validation verdict.

Gate:

- Render pass validation proves artifact summaries expose validation sidecar
  health without parsing the sidecar itself.
- `Architecture.PhaseGates` keeps schema bumps, result fields, summary export,
  and tests visible.

### P28: Tool Diagnostics Manifest Validation Sidecar Discovery

Goal: let tools that start from the diagnostics manifest discover the validation
sidecar directly, while keeping validation sidecar state and verdicts out of the
manifest to avoid self-referential primary artifact validation.

Tasks:

- Add an `artifactValidationJson` discovery node to the manifest `artifacts`
  object.
- Export only stable sidecar identity and location data from the manifest:
  kind, content type, relative path, and path.
- Keep saved/existence/hash/verdict fields in the artifact summary and
  validation JSON, not the manifest.
- Bump the tool diagnostics schema for the manifest contract change.
- Add tests proving saved manifests expose the validation sidecar discovery
  node and stable relative path.

Gate:

- Render pass validation proves manifests can locate
  `.diagnostics-validation.json` without parsing the artifact summary first.
- `Architecture.PhaseGates` keeps schema bump, manifest node export, sidecar
  kind, and relative-path tests visible.

### P29: Tool Diagnostics Manifest Artifact Type Metadata

Goal: make the diagnostics manifest useful as a standalone artifact discovery
index by describing each artifact's stable kind and content type, so tools do
not need to hard-code filename suffixes before choosing an importer.

Tasks:

- Add `kind` and `contentType` to manifest entries for scene-renderer text,
  RenderGraph DOT, RenderGraph text, RenderGraph JSON, manifest JSON, artifact
  summary JSON, and validation JSON.
- Keep mutable integrity fields such as byte size, content hash, bundle hash,
  and validation verdict in the artifact summary/validation sidecar rather than
  in the manifest.
- Bump the tool diagnostics schema for the manifest metadata contract change.
- Add tests proving saved manifests classify representative text, Graphviz,
  JSON, manifest, and summary artifacts.

Gate:

- Render pass validation proves manifests expose artifact kind and content type
  metadata without requiring the artifact summary.
- `Architecture.PhaseGates` keeps schema bump, manifest kind/contentType
  fields, and representative tests visible.

### P30: Tool Diagnostics Manifest Sidecar Schema Metadata

Goal: let tools route artifact summary and validation sidecars by schema version
directly from the manifest before opening those sidecar JSON files.

Tasks:

- Add artifact summary and artifact validation schema version fields to saved
  artifact results.
- Export sidecar schema versions in the manifest capture metadata.
- Export `schemaVersion` on the artifact summary and validation sidecar
  discovery nodes.
- Keep byte size, content hash, bundle hash, and validation verdict ownership in
  the artifact summary and validation sidecar, not in the manifest.
- Bump the tool diagnostics schema for the sidecar schema metadata contract.
- Add tests proving saved artifact results and manifests expose the sidecar
  schema versions.

Gate:

- Render pass validation proves manifests can route summary and validation
  sidecars by schema version without opening them first.
- `Architecture.PhaseGates` keeps schema bump, result fields, manifest schema
  export, and representative tests visible.

### P31: Tool Diagnostics Stable Capture ID

Goal: give every saved diagnostics capture a deterministic, portable capture id
that can correlate manifest, artifact summary, validation sidecar, CI records,
editor panels, profiler imports, and future FrameDebugger captures without
depending on machine-local output directories.

Tasks:

- Add a stable `captureId` to artifact results and validation results.
- Derive the id from portable capture identity: base name, frame index,
  diagnostics schema versions, sidecar schema versions, and RenderGraph capture
  scale. Do not include output directory or absolute paths.
- Export the id through manifest capture metadata, artifact summary capture
  metadata, and validation JSON capture metadata.
- Bump the tool diagnostics, artifact summary, and artifact validation schemas
  for the new cross-file correlation key.
- Add tests proving the id is populated and exported consistently across saved
  artifacts and validation JSON.

Gate:

- Render pass validation proves manifest, summary, and validation sidecar all
  carry the same stable capture id.
- `Architecture.PhaseGates` keeps schema bumps, capture id fields, deterministic
  id generation, JSON export, and tests visible.

### P32: Tool Diagnostics Summary Sidecar Schema Metadata

Goal: make artifact summaries self-describing enough for summary-first tools to
route sidecars by schema version without opening the manifest or validation JSON
first.

Tasks:

- Export artifact summary and artifact validation schema versions in artifact
  summary capture metadata.
- Export the validation sidecar schema version on the artifact summary
  `validationReport` node.
- Bump the tool diagnostics and artifact summary schemas for the new
  summary-first routing contract.
- Keep the validation sidecar schema stable because the validation JSON payload
  itself is unchanged.
- Add tests proving empty summaries and saved artifact summaries expose the
  sidecar schema versions.

Gate:

- Render pass validation proves summary JSON can identify both summary and
  validation sidecar schema versions without consulting the manifest.
- `Architecture.PhaseGates` keeps schema bumps, summary JSON exports, and
  representative tests visible.

### P33: Tool Diagnostics Validation Sidecar Schema Metadata

Goal: make artifact validation JSON self-describing enough for validation-first
CI and tooling flows to route related sidecars by schema version without opening
the manifest or artifact summary first.

Tasks:

- Add artifact summary and artifact validation schema versions to validation
  results.
- Copy sidecar schema versions from saved artifact results into validation
  results during artifact validation.
- Export the sidecar schema versions in validation JSON capture metadata.
- Bump the tool diagnostics and artifact validation schemas for the new
  validation-first routing contract.
- Keep the artifact summary schema stable because the summary JSON shape is
  unchanged.
- Add tests proving validation results and validation JSON expose the sidecar
  schema versions.

Gate:

- Render pass validation proves validation results and validation JSON can
  identify both summary and validation sidecar schema versions without
  consulting the manifest or artifact summary.
- `Architecture.PhaseGates` keeps schema bumps, validation result fields, JSON
  exports, and representative tests visible.

### P34: Tool Diagnostics JSON Sidecar Identity Metadata

Goal: make SceneRenderer diagnostics JSON sidecars self-identifying when copied,
archived, uploaded to CI, or opened by tools without their original filenames.

Tasks:

- Add top-level `kind` and `contentType` metadata to diagnostics manifest JSON.
- Add top-level `kind` and `contentType` metadata to artifact summary JSON.
- Add top-level `kind` and `contentType` metadata to artifact validation JSON.
- Bump the tool diagnostics, artifact summary, and artifact validation schemas
  for the new JSON sidecar identity contract.
- Add tests proving each JSON sidecar can identify its artifact kind and content
  type directly.

Gate:

- Render pass validation proves manifest, summary, and validation JSON sidecars
  carry top-level artifact identity metadata.
- `Architecture.PhaseGates` keeps schema bumps, JSON identity fields, and
  representative tests visible.

### P35: Tool Diagnostics JSON Sidecar Artifact IDs

Goal: make SceneRenderer diagnostics JSON sidecars expose their stable bundle
artifact ids directly, so tools can identify a copied or renamed JSON sidecar as
`manifestJson`, `artifactSummaryJson`, or `artifactValidationJson` without
depending on filenames or manifest lookup.

Tasks:

- Add top-level `id` metadata to diagnostics manifest JSON.
- Add top-level `id` metadata to artifact summary JSON.
- Add top-level `id` metadata to artifact validation JSON.
- Bump the tool diagnostics, artifact summary, and artifact validation schemas
  for the new artifact-id contract.
- Add tests proving each JSON sidecar exports its top-level artifact id.

Gate:

- Render pass validation proves manifest, summary, and validation JSON sidecars
  carry stable top-level artifact ids.
- `Architecture.PhaseGates` keeps schema bumps, JSON id fields, and
  representative tests visible.

### P36: RenderGraph Diagnostics JSON Artifact Identity

Goal: make RenderGraph diagnostics JSON self-identifying when opened directly by
Editor, Profiler, FrameDebugger, CI, or external tools instead of requiring
SceneRenderer manifest context or filename conventions.

Tasks:

- Add top-level `id`, `kind`, and `contentType` metadata to
  `RenderGraph::ExportDiagnosticsJson()`.
- Bump the RenderGraph diagnostics schema for the new machine-readable artifact
  identity contract.
- Keep SceneRenderer artifact summary and manifest identity for the
  RenderGraph JSON sidecar aligned with the direct RenderGraph JSON payload.
- Add tests proving direct RenderGraph JSON export and SceneRenderer artifact
  summaries expose the RenderGraph JSON artifact identity.

Gate:

- RenderGraph validation proves `.rendergraph.json` exports top-level artifact
  id, kind, content type, and schema version.
- Render pass validation proves SceneRenderer's saved artifact summary preserves
  the RenderGraph JSON kind/content metadata.
- `Architecture.PhaseGates` keeps the schema bump, JSON identity fields, and
  representative tests visible.

### P37: RenderGraph Diagnostics Schema Identity Metadata

Goal: make RenderGraph diagnostics JSON and SceneRenderer tool sidecars identify
the diagnostics schema namespace as well as the version, so tools can route a
copied `.rendergraph.json`, artifact summary, manifest, or validation report
without relying on filename conventions or ambiguous numeric versions.

Tasks:

- Add a stable RenderGraph diagnostics schema id constant.
- Export the schema id from direct RenderGraph diagnostics text and JSON.
- Carry the RenderGraph diagnostics schema id through SceneRenderer artifact
  capture metadata and validation results.
- Export the schema id in manifest, artifact summary, and validation JSON
  sidecars wherever the RenderGraph diagnostics schema version is reported.
- Bump the RenderGraph diagnostics, tool diagnostics, artifact summary, and
  artifact validation schemas for the new schema-identity contract.
- Add tests proving direct RenderGraph JSON and SceneRenderer tool sidecars
  expose the RenderGraph diagnostics schema id.

Gate:

- RenderGraph validation proves `.rendergraph.json` exports the stable schema
  id with the bumped diagnostics schema version.
- Render pass validation proves saved manifests, artifact summaries, and
  validation sidecars preserve the RenderGraph diagnostics schema id.
- `Architecture.PhaseGates` keeps the schema id constant, JSON exports, schema
  bumps, and representative tests visible.

### P38: Tool Diagnostics Validation Entry Artifact Identity

Goal: make artifact validation JSON entries self-describing enough for
validation-first CI and external tools to understand each checked artifact
without reopening the diagnostics manifest or artifact summary.

Tasks:

- Add artifact `kind` and `contentType` metadata to validation result entries.
- Add optional `schemaId` and `schemaVersion` metadata to validation entries for
  versioned machine-readable artifacts such as RenderGraph diagnostics JSON.
- Export the entry identity metadata through artifact validation JSON.
- Bump the artifact validation schema for the new entry-level identity contract.
- Add tests proving validation results and validation JSON expose the
  RenderGraph diagnostics JSON entry kind, content type, schema id, and schema
  version.

Gate:

- Render pass validation proves validation entries identify
  `renderGraphDiagnosticsJson` as `RenderGraphDiagnosticsJson` with
  `application/json` content and the RenderGraph diagnostics schema identity.
- `Architecture.PhaseGates` keeps the validation entry fields, validation JSON
  exports, schema bump, and representative tests visible.

### P39: Tool Diagnostics Validation Entry Identity Verification

Goal: make artifact validation prove that machine-readable JSON artifacts carry
the expected identity metadata in their actual file contents, instead of only
reporting the expected identity alongside byte-size and content-hash checks.

Tasks:

- Add validation-entry flags for identity and schema checks:
  `identityChecked`, `identityMatches`, `schemaChecked`, and `schemaMatches`.
- Inspect saved JSON primary artifacts for expected `id`, `kind`,
  `contentType`, `schemaId`, and `schemaVersion` metadata where applicable.
- Include identity and schema match state in artifact validation JSON.
- Mark a JSON artifact invalid when byte/hash checks pass but identity or schema
  metadata does not match.
- Bump the artifact validation schema for the new verification result fields.
- Add a negative test proving a RenderGraph diagnostics JSON artifact with a
  wrong top-level id reports an identity mismatch.

Gate:

- Render pass validation proves valid JSON artifacts report identity/schema
  matches and malformed identity metadata reports a dedicated diagnostic.
- `Architecture.PhaseGates` keeps the validation fields, JSON exports, schema
  bump, and negative test visible.

### P40: Tool Diagnostics Validation Entry Actual Identity Metadata

Goal: make artifact validation JSON explain identity mismatches directly by
reporting the actual `id`, `kind`, `contentType`, `schemaId`, and
`schemaVersion` values read from each checked machine-readable artifact.

Tasks:

- Add actual artifact identity fields to validation entries:
  `actualId`, `actualKind`, `actualContentType`, `actualSchemaId`, and
  `actualSchemaVersion`.
- Extract actual JSON artifact identity and schema metadata during artifact
  validation.
- Compare expected identity/schema values against extracted actual values.
- Export the actual values through artifact validation JSON.
- Bump the artifact validation schema for the new actual-value contract.
- Extend the identity-mismatch test to prove the mismatched actual id is
  captured and exported.

Gate:

- Render pass validation proves both matching and mismatched JSON artifact
  identities include actual values in validation results and JSON output.
- `Architecture.PhaseGates` keeps the actual-value fields, extraction helpers,
  JSON exports, schema bump, and mismatch test visible.

### P41: Tool Diagnostics Validation Entry Diagnostic Codes

Goal: make artifact validation failures machine-actionable by exposing a stable
diagnostic code in addition to the human-readable diagnostic message.

Tasks:

- Add `diagnosticCode` to validation entries, defaulting to `None`.
- Classify validation failures with stable codes such as
  `NotRecordedAsSaved`, `FileMissing`, `ByteSizeMismatch`,
  `ContentHashMismatch`, `IdentityMetadataMismatch`, and
  `SchemaMetadataMismatch`.
- Export `diagnosticCode` through artifact validation JSON.
- Bump the artifact validation schema for the new machine-readable failure
  classification contract.
- Add tests proving identity mismatches and byte-size mismatches expose stable
  diagnostic codes.

Gate:

- Render pass validation proves successful entries report `None`, identity
  mismatches report `IdentityMetadataMismatch`, and modified artifact size
  mismatches report `ByteSizeMismatch`.
- `Architecture.PhaseGates` keeps the diagnostic-code field, JSON export,
  schema bump, and representative tests visible.

### P42: Tool Diagnostics Validation Diagnostic Code Summary

Goal: make artifact validation verdicts easier for CI and tooling to triage by
exporting aggregate counts for each stable diagnostic code.

Tasks:

- Add `diagnosticCodeCounts` to artifact validation results.
- Aggregate each validation entry's `diagnosticCode`, normalizing empty codes
  to `None`.
- Export `diagnosticCodeCounts` through artifact validation JSON.
- Bump the artifact validation schema for the new summary contract.
- Add tests proving successful, identity-mismatch, and byte-size-mismatch
  validations expose the expected code distributions.

Gate:

- Render pass validation proves successful validations report `None: 5`,
  identity mismatches report `IdentityMetadataMismatch: 1`, and modified
  artifact size mismatches report `ByteSizeMismatch: 1`.
- `Architecture.PhaseGates` keeps the diagnostic-code summary field,
  aggregation helper, JSON export, schema bump, and representative tests
  visible.

### P43: Artifact Summary Validation Diagnostic Code Snapshot

Goal: let CI and tools triage artifact validation failures from the artifact
summary JSON without opening the validation sidecar first.

Tasks:

- Add `artifactValidationDiagnosticCodeCounts` to artifact save results.
- Copy validation `diagnosticCodeCounts` into artifact summary stats after
  validation completes.
- Export the copied code counts through the artifact summary
  `validationReport`.
- Export an empty `diagnosticCodeCounts` array for empty artifact summaries.
- Bump the artifact summary schema for the new validation-report snapshot.
- Add tests proving both empty and populated artifact summaries expose the
  diagnostic code count field.

Gate:

- Render pass validation proves artifact save results preserve `None: 5` and
  artifact summary JSON exposes `diagnosticCodeCounts`.
- `Architecture.PhaseGates` keeps the summary schema bump, result field,
  copy path, JSON export, and representative tests visible.

### P44: Manifest Validation Verdict Scope Guardrail

Goal: keep the diagnostics manifest focused on sidecar discovery and avoid
putting validation verdict payloads into a primary artifact that validation
itself must hash and verify.

Tasks:

- Keep validation verdicts and diagnostic-code summaries in the artifact
  summary and validation sidecar.
- Keep the diagnostics manifest limited to artifact identity, schema metadata,
  and portable sidecar paths.
- Add tests proving saved manifests do not export `validationReport` or
  `diagnosticCodeCounts`.
- Add an architecture gate so the manifest discovery-only boundary is explicit.

Gate:

- Render pass validation proves the final diagnostics manifest can discover the
  validation sidecar while staying free of validation verdict payloads.
- `Architecture.PhaseGates` keeps the manifest scope guardrail documented and
  covered by negative manifest assertions.

### P45: Tool Diagnostics Validation Verdict Codes

Goal: let CI and tools consume one stable top-level validation verdict instead
of reconstructing status from several booleans and counts.

Tasks:

- Add `verdictCode` to artifact validation results.
- Add `artifactValidationVerdictCode` to artifact save results so summaries can
  expose the same verdict.
- Compute verdict codes from aggregate state:
  `Unavailable`, `Valid`, `InvalidArtifacts`, `BundleHashMismatch`, and
  `Invalid`.
- Export `verdictCode` through artifact validation JSON and artifact summary
  `validationReport`.
- Bump the artifact validation and artifact summary schemas for the verdict
  contract.
- Add tests proving valid captures, invalid artifact entries, and bundle-only
  hash mismatches emit the expected verdict codes.

Gate:

- Render pass validation proves successful captures report `Valid`, copied
  bundle hash mismatches report `BundleHashMismatch`, and entry-level failures
  report `InvalidArtifacts`.
- `Architecture.PhaseGates` keeps the verdict field, summary copy, helper,
  JSON export, schema bumps, and representative tests visible.

### P46: Tool Diagnostics Validation Primary Failure Summary

Goal: let CI and tools jump directly from the top-level validation verdict to
the first actionable failure without scanning every validation entry.

Tasks:

- Add `primaryFailureCode` and `primaryFailureArtifactId` to artifact
  validation results.
- Add `artifactValidationPrimaryFailureCode` and
  `artifactValidationPrimaryFailureArtifactId` to artifact save results so
  summaries can expose the same primary failure.
- Compute primary failure summaries after validation entries, bundle-hash
  status, and verdicts are known.
- Export the primary failure summary through artifact validation JSON and the
  artifact summary `validationReport`.
- Bump the artifact validation and artifact summary schemas for the new
  top-level failure summary.
- Add tests proving valid captures report `None`, bundle-only mismatches report
  `BundleHashMismatch`, and entry-level failures identify
  `renderGraphDiagnosticsJson` as the primary failing artifact.

Gate:

- Render pass validation proves identity and byte-size failures expose both the
  primary failure code and primary artifact id.
- `Architecture.PhaseGates` keeps the primary failure fields, summary copy,
  computation helper, JSON export, schema bumps, and representative tests
  visible.

### P47: Tool Diagnostics Validation Primary Failure Detail

Goal: let CI, editor tooling, profiler imports, and future FrameDebugger views
show the first actionable validation failure with its portable artifact path and
human-readable failure message, without scanning every validation entry.

Tasks:

- Add `primaryFailureArtifactRelativePath` and `primaryFailureMessage` to
  artifact validation results.
- Add `artifactValidationPrimaryFailureArtifactRelativePath` and
  `artifactValidationPrimaryFailureMessage` to artifact save results so
  artifact summaries can expose the same failure detail.
- Populate the relative path and message from the first failed validation entry,
  with explicit messages for unavailable validation, bundle-hash mismatches, and
  invalid fallback verdicts.
- Export the primary failure detail through artifact validation JSON and the
  artifact summary `validationReport`.
- Bump the artifact validation and artifact summary schemas for the new
  top-level failure detail fields.
- Add tests proving valid captures have empty detail fields, unavailable
  summaries explain missing validation, bundle-only mismatches expose a stable
  message, and entry-level failures identify `Frame001.rendergraph.json` with
  the entry diagnostic message.

Gate:

- Render pass validation proves identity and byte-size failures expose both the
  primary failing artifact relative path and primary failure message.
- `Architecture.PhaseGates` keeps the detail fields, summary copy, computation
  path, JSON export, schema bumps, and representative tests visible.

### P48: Tool Diagnostics Validation Primary Failure Artifact Identity

Goal: let CI, editor tooling, profiler imports, and future FrameDebugger views
route the first failing artifact to the right parser or panel without scanning
validation entries after reading the top-level primary failure summary.

Tasks:

- Add primary failure artifact identity fields to artifact validation results:
  `primaryFailureArtifactKind`, `primaryFailureArtifactContentType`,
  `primaryFailureArtifactSchemaId`, and
  `primaryFailureArtifactSchemaVersion`.
- Add matching `artifactValidationPrimaryFailureArtifact*` fields to artifact
  save results so artifact summaries can expose the same top-level artifact
  identity.
- Populate the identity fields from the first failed validation entry and leave
  them empty/zero for unavailable validation, valid captures, bundle-hash
  mismatches, and invalid fallback verdicts.
- Export the primary failure artifact identity through artifact validation JSON
  and the artifact summary `validationReport`.
- Bump the artifact validation and artifact summary schemas for the new
  top-level artifact identity fields.
- Add tests proving valid captures have empty identity fields, bundle-only
  mismatches keep identity empty, and entry-level failures expose
  `RenderGraphDiagnosticsJson`, `application/json`,
  `RVX.RenderGraph.Diagnostics`, and schema version `3`.

Gate:

- Render pass validation proves identity and byte-size failures expose the
  primary failing artifact kind, content type, schema id, and schema version.
- `Architecture.PhaseGates` keeps the identity fields, summary copy, computation
  path, JSON export, schema bumps, and representative tests visible.

### P49: Tool Diagnostics Validation Primary Failure Entry Index

Goal: let CI, editor tooling, profiler imports, and future FrameDebugger views
jump from the top-level primary failure summary to the exact validation `entries`
array item without scanning every artifact entry.

Tasks:

- Add `primaryFailureEntryIndex` to artifact validation results, using
  `RVX_INVALID_INDEX` when the primary failure is not tied to a validation
  entry.
- Add `artifactValidationPrimaryFailureEntryIndex` to artifact save results so
  artifact summaries can expose the same zero-based entry index.
- Populate the index while finding the first failed validation entry and keep it
  unset for unavailable validation, valid captures, bundle-hash mismatches, and
  invalid fallback verdicts.
- Export the index through artifact validation JSON and the artifact summary
  `validationReport`, writing JSON `null` when no entry index exists.
- Bump the artifact validation and artifact summary schemas for the new
  top-level entry-index field.
- Add tests proving valid captures and bundle-only mismatches export `null`,
  while identity and byte-size failures expose index `3` for
  `renderGraphDiagnosticsJson`.

Gate:

- Render pass validation proves entry-level failures expose the zero-based
  primary failure entry index and non-entry failures expose no index.
- `Architecture.PhaseGates` keeps the index fields, summary copy, computation
  path, JSON null/value export, schema bumps, and representative tests visible.

### P50: Tool Diagnostics Validation Failure Count Summary

Goal: let CI, editor tooling, profiler imports, and future FrameDebugger views
understand validation scope and failure cardinality from either the validation
sidecar or the artifact summary without reopening and scanning every validation
entry.

Tasks:

- Add `failedPrimaryArtifactCount` to artifact validation results.
- Add `artifactValidationFailedPrimaryArtifactCount` to artifact save results so
  artifact summaries expose the same validation scope snapshot.
- Count failed primary artifacts while validating artifact entries, keeping
  bundle-only mismatches at zero failed entries because they are top-level hash
  failures rather than entry failures.
- Export the failed count through artifact validation JSON and the artifact
  summary `validationReport`, including zero for empty and valid reports.
- Bump the artifact validation and artifact summary schemas for the new
  validation scope field.
- Add tests proving valid captures and bundle-only mismatches expose zero
  failed primary artifacts, while identity and byte-size failures expose one.

Gate:

- Render pass validation proves successful, bundle-only, identity-mismatch, and
  byte-size-mismatch validations expose the expected failed primary artifact
  count.
- `Architecture.PhaseGates` keeps the failed-count fields, computation path,
  summary copy, JSON export, schema bumps, and representative tests visible.

### P51: Tool Diagnostics Validation Entry Index Metadata

Goal: make each artifact validation entry self-identify its stable zero-based
position, so CI, editor tooling, profiler imports, and future FrameDebugger
views can filter, sort, or deep-link validation entries while still jumping back
to the original validation `entries` array position referenced by
`primaryFailureEntryIndex`.

Tasks:

- Add `entryIndex` to artifact validation entries, using `RVX_INVALID_INDEX`
  only for uninitialized entries.
- Assign the entry index while constructing validation entries from the primary
  artifact source list.
- Export the entry index through artifact validation JSON for every entry.
- Bump the artifact validation schema for the entry-index metadata contract.
- Add tests proving successful entries preserve array-order indices and
  `renderGraphDiagnosticsJson` exports index `3` in valid, identity-mismatch,
  and byte-size-mismatch reports.

Gate:

- Render pass validation proves validation entries expose stable entry indices
  in memory and JSON.
- `Architecture.PhaseGates` keeps the entry-index field, assignment path, JSON
  export, schema bump, and representative tests visible.

### P52: Tool Diagnostics Validation Entry Primary Failure Flag

Goal: let CI, editor tooling, profiler imports, and future FrameDebugger views
highlight the first actionable validation entry directly from the entry payload,
without comparing every entry against the top-level `primaryFailureEntryIndex`
after filtering or sorting validation rows.

Tasks:

- Add `primaryFailure` to artifact validation entries.
- Reset entry primary-failure flags before computing the top-level primary
  failure summary.
- Mark the first failed validation entry as `primaryFailure = true`, leaving
  valid captures and bundle-only hash mismatches with no marked entries.
- Export the entry primary-failure flag through artifact validation JSON.
- Bump the artifact validation schema for the entry primary-failure flag.
- Add tests proving valid entries and bundle-only mismatches do not mark
  entries, while identity and byte-size failures mark
  `renderGraphDiagnosticsJson`.

Gate:

- Render pass validation proves entry-level failures expose `primaryFailure` in
  memory and JSON.
- `Architecture.PhaseGates` keeps the primary-failure flag, reset path,
  assignment path, JSON export, schema bump, and representative tests visible.

### P53: Tool Diagnostics Validation Primary Failure Entry Count

Goal: let CI, editor tooling, profiler imports, and future FrameDebugger views
verify the consistency of primary-failure entry marking from top-level
validation and artifact-summary payloads, without scanning every validation
entry to detect missing or duplicate `primaryFailure` markers.

Tasks:

- Add `primaryFailureEntryCount` to artifact validation results.
- Add `artifactValidationPrimaryFailureEntryCount` to artifact save results so
  artifact summaries expose the same consistency snapshot.
- Reset the count before computing the primary failure and set it to one when
  the first failed validation entry is marked.
- Keep valid captures, unavailable validation, bundle-only hash mismatches, and
  fallback invalid verdicts at zero marked entries.
- Export the count through artifact validation JSON and artifact summary
  `validationReport`.
- Bump the artifact validation and artifact summary schemas for the new
  marked-entry count.
- Add tests proving valid captures and bundle-only mismatches report zero,
  while identity and byte-size entry failures report one.

Gate:

- Render pass validation proves the primary-failure marked-entry count is
  exposed in memory, validation JSON, and artifact summary JSON.
- `Architecture.PhaseGates` keeps the result fields, summary copy, reset path,
  assignment path, JSON export, schema bumps, and representative tests visible.

### P54: Tool Diagnostics Validation Entry Count Summary

Goal: let CI, editor tooling, profiler imports, and future FrameDebugger views
verify the completeness of artifact validation entry payloads from top-level
validation and artifact-summary metadata, without scanning the `entries` array
just to know how many entries were generated.

Tasks:

- Add `entryCount` to artifact validation results.
- Add `artifactValidationEntryCount` to artifact save results so artifact
  summaries expose the same entry-count snapshot.
- Derive `entryCount` from the generated validation `entries` array after
  artifact validation entry construction.
- Export the count through artifact validation JSON and artifact summary
  `validationReport`, including zero for unavailable summary payloads.
- Bump the artifact validation and artifact summary schemas for the new
  entry-count metadata.
- Add tests proving valid captures, bundle-only mismatches, identity failures,
  and byte-size failures preserve and export the expected entry count.

Gate:

- Render pass validation proves the validation entry count is exposed in memory,
  validation JSON, and artifact summary JSON.
- `Architecture.PhaseGates` keeps the result fields, summary copy, computation
  path, JSON export, schema bumps, and representative tests visible.

### P55: Tool Diagnostics Validation Entry Coverage Summary

Goal: let CI, editor tooling, profiler imports, and future FrameDebugger views
detect whether artifact validation generated one entry for every checked
primary artifact through a direct metadata flag, instead of every consumer
having to compare `entryCount` and `checkedPrimaryArtifactCount` on its own.

Tasks:

- Add `entryCountMatchesCheckedPrimaryArtifactCount` to artifact validation
  results.
- Add `artifactValidationEntryCountMatchesCheckedPrimaryArtifactCount` to
  artifact save results so artifact summaries expose the same coverage state.
- Compute the flag from `entryCount == checkedPrimaryArtifactCount` after
  validation entry construction.
- Export the flag through artifact validation JSON and artifact summary
  `validationReport`, including `false` for unavailable summary payloads.
- Bump the artifact validation and artifact summary schemas for the new entry
  coverage metadata.
- Add tests proving valid captures, bundle-only mismatches, identity failures,
  and byte-size failures preserve and export complete entry coverage.

Gate:

- Render pass validation proves the validation entry coverage flag is exposed
  in memory, validation JSON, and artifact summary JSON.
- `Architecture.PhaseGates` keeps the result fields, summary copy, computation
  path, JSON export, schema bumps, and representative tests visible.

### P56: Tool Diagnostics Validation Entry Coverage Code

Goal: let CI, editor tooling, profiler imports, and future FrameDebugger views
display validation entry coverage as a stable status code, instead of every
consumer having to infer user-facing state from availability, entry counts, and
the coverage boolean.

Tasks:

- Add `entryCoverageCode` to artifact validation results, with `Unavailable`
  as the default value.
- Add `artifactValidationEntryCoverageCode` to artifact save results so
  artifact summaries expose the same coverage status.
- Compute the code as `Unavailable` when no validation result is available,
  `Complete` when `entryCount` matches `checkedPrimaryArtifactCount`, and
  `Mismatch` otherwise.
- Include entry coverage completeness in the `allPrimaryArtifactsValid`
  decision so future missing or extra validation entries cannot be treated as a
  valid artifact bundle.
- Export the code through artifact validation JSON and artifact summary
  `validationReport`.
- Bump the artifact validation and artifact summary schemas for the new entry
  coverage code.
- Add tests proving unavailable summaries export `Unavailable`, while valid
  captures, bundle-only mismatches, identity failures, and byte-size failures
  export `Complete`.

Gate:

- Render pass validation proves the validation entry coverage code is exposed in
  memory, validation JSON, and artifact summary JSON.
- `Architecture.PhaseGates` keeps the result fields, summary copy, computation
  path, JSON export, schema bumps, and representative tests visible.

### P57: Tool Diagnostics Validation Entry Coverage Message

Goal: let CI, editor tooling, profiler imports, and future FrameDebugger views
display validation entry coverage with a stable human-readable message paired
with the `entryCoverageCode`, instead of every consumer having to translate the
code and counts independently.

Tasks:

- Add `entryCoverageMessage` to artifact validation results, with
  `validation result is unavailable` as the default value.
- Add `artifactValidationEntryCoverageMessage` to artifact save results so
  artifact summaries expose the same message.
- Compute the message as unavailable, complete, or mismatch alongside
  `entryCoverageCode`.
- Export the message through artifact validation JSON and artifact summary
  `validationReport`.
- Bump the artifact validation and artifact summary schemas for the new entry
  coverage message.
- Add tests proving unavailable summaries export the unavailable message, while
  valid captures, bundle-only mismatches, identity failures, and byte-size
  failures export the complete-coverage message.

Gate:

- Render pass validation proves the validation entry coverage message is
  exposed in memory, validation JSON, and artifact summary JSON.
- `Architecture.PhaseGates` keeps the result fields, summary copy, computation
  path, JSON export, schema bumps, and representative tests visible.

### P58: Tool Diagnostics Validation Entry Coverage Helper

Goal: keep validation entry coverage code/message semantics centralized so
future coverage states can be added in one validation helper path, instead of
duplicating conditional logic inside artifact validation construction or JSON
export code.

Tasks:

- Add a helper for mapping validation entry coverage state to
  `entryCoverageCode`.
- Add a helper for mapping the same state to `entryCoverageMessage`.
- Route validation result population through those helpers.
- Preserve existing JSON output and schema versions because this is an internal
  semantic consolidation, not a payload shape change.
- Keep tests proving complete coverage codes and messages are still exported for
  valid and failure cases.

Gate:

- `Architecture.PhaseGates` keeps the helper functions, helper-backed
  assignments, mismatch explanation, and representative render pass validation
  assertions visible.

### P59: Architecture Baseline Contract Coverage

Goal: make the default architecture baseline exercise the framework contracts
that were introduced after the original baseline, so module boundaries, shared
editor/runtime core policy, RHI capability honesty, app-mode semantics, and
runtime resource policy are checked together after each architecture edit.

Tasks:

- Add `Architecture.CMakeModuleVisibility` to the baseline runner.
- Add `Architecture.EditorRuntimeBoundary` to the baseline runner.
- Add the full `RHIContractValidation` suite to the baseline runner.
- Add the full `AppModeBoundaryValidation` suite to the baseline runner.
- Add the full `ResourceRuntimePolicyValidation` suite to the baseline runner.
- Keep `Architecture.PhaseGates` coverage so these baseline contract suites
  cannot be dropped silently.

Gate:

- `Architecture.PhaseGates` keeps the promoted baseline regex entries visible.
- `run_architecture_baseline.ps1` now runs the promoted contract suites.

### P60: CMake Module Link Boundary Gate

Goal: extend module-boundary enforcement from source include directives to
CMake target links, so modules cannot bypass the dependency direction encoded
in `Docs/module-boundaries.json` by linking forbidden implementation targets.

Tasks:

- Add a CMake target-link boundary checker that scans `target_link_libraries`
  blocks.
- Reuse `Docs/module-boundaries.json` as the shared source of truth for allowed
  module edges.
- Resolve common project target spellings such as `RVX_Core`, `RVX::Core`, and
  direct module targets like `Spatial`.
- Register the checker as `Architecture.CMakeModuleLinks`.
- Add the checker to the architecture baseline runner.
- Remove stale over-broad CMake links that were not backed by source-level
  dependencies, including UI-to-Render and backend-to-ShaderCompiler links.

Gate:

- `Architecture.CMakeModuleLinks` reports no link-boundary violations.
- `Architecture.PhaseGates` keeps the checker, CTest registration, baseline
  entry, and removed over-broad links protected.

### P61: CMake Module Include-Edge Boundary Gate

Goal: extend module-boundary enforcement to CMake include directories, so
modules cannot bypass source include and target-link checks by manually adding
another module's `Include` or `Private` directory.

Tasks:

- Add a CMake include-edge checker that scans `target_include_directories`
  blocks.
- Reuse `Docs/module-boundaries.json` as the shared source of truth for allowed
  include-directory edges.
- Resolve generator expressions such as `$<BUILD_INTERFACE:...>` and common
  project target spellings.
- Register the checker as `Architecture.CMakeModuleIncludeEdges`.
- Add the checker to the architecture baseline runner.
- Remove stale `Terrain` and `Water` private include directories that reached
  into `Engine`, `World`, `Resource`, or `Geometry` without source-level
  dependency evidence.

Gate:

- `Architecture.CMakeModuleIncludeEdges` reports no include-directory boundary
  violations.
- `Architecture.PhaseGates` keeps the checker, CTest registration, baseline
  entry, and removed over-broad include directories protected.

### P62: Module Boundary Manifest Coverage Gate

Goal: make `Docs/module-boundaries.json` itself verifiable, so future modules,
project include prefixes, CMake targets, and allowed dependency edges cannot be
added outside the module-boundary system.

Tasks:

- Add a manifest checker that validates `projectIncludePrefixes` against the
  registered module table.
- Validate that module paths exist, are unique, and expose explicit allowed
  dependency edges.
- Validate allowed and legacy edges reference known modules.
- Scan CMake project targets and ensure they live under registered module paths.
- Register the checker as `Architecture.ModuleBoundaryManifest`.
- Add the checker to the architecture baseline runner.

Gate:

- `Architecture.ModuleBoundaryManifest` reports no manifest coverage failures.
- `Architecture.PhaseGates` keeps the checker, CTest registration, baseline
  entry, and core manifest validation rules protected.

### P63: Public Header Linkage Scope Gate

Goal: ensure every cross-module dependency exposed by public headers is matched
by a PUBLIC or INTERFACE CMake link, so downstream targets can consume module
APIs without depending on accidental include directories or private links.

Tasks:

- Add a public-header linkage checker that scans module `Include` headers for
  project include edges.
- Parse CMake `target_link_libraries` scopes and resolve project target aliases.
- Fail when a public header includes another module but the owning module does
  not link that dependency as PUBLIC or INTERFACE.
- Register the checker as `Architecture.PublicHeaderLinkage`.
- Add the checker to the architecture baseline runner.
- Promote `Tools -> Audio` to a public CMake dependency because
  `Tools/Importers/AudioImporter.h` exposes `Audio::AudioFormat`.
- Remove the private Audio include-directory workaround from `RVX_Tools`.

Gate:

- `Architecture.PublicHeaderLinkage` reports no public header linkage
  violations.
- `Architecture.PhaseGates` keeps the checker, CTest registration, baseline
  entry, and the `Tools -> Audio` public dependency protected.

### P64: Public Include Directory Scope Gate

Goal: ensure modules do not expose another module's include directory through
PUBLIC or INTERFACE `target_include_directories`; cross-module include paths
must flow through the dependency target that owns them.

Tasks:

- Extend the CMake include-edge checker to classify PUBLIC and INTERFACE
  include-directory edges.
- Fail when a module publicly exposes a different module's `Include` directory
  directly.
- Keep PRIVATE include-directory edges under the existing module-boundary
  allow-list, so implementation-only adapters remain explicit.
- Reuse `Architecture.CMakeModuleIncludeEdges` and the architecture baseline
  runner instead of adding a parallel test.

Gate:

- `Architecture.CMakeModuleIncludeEdges` reports no public include-directory
  exposure violations.
- `Architecture.PhaseGates` keeps the public exposure classifier, failure
  message, and baseline coverage protected.

### P65: RHI RenderGraph Baseline Capability Contract

Goal: make multi-backend RHI capability reports expose whether a backend can
satisfy the engine's RenderGraph baseline contract, and list the exact missing
requirements when it cannot.

Tasks:

- Bump the RHI capability report schema for the new baseline-readiness fields.
- Add `renderGraphBaselineSupported` and
  `renderGraphBaselineMissingRequirements` to the public capability report.
- Compute the baseline from validated capabilities, descriptor-set support,
  explicit-or-emulated resource barriers, and native-or-emulated queue
  synchronization.
- Export the baseline verdict and missing requirements through the stable text
  report used by tools and logs.
- Add RHI contract tests proving both a ready backend and a valid-but-incomplete
  backend report deterministic baseline diagnostics.

Gate:

- RHI contract validation proves RenderGraph baseline readiness and missing
  requirements are derived from public `RHICapabilities`.
- `Architecture.PhaseGates` keeps the schema bump, public fields, computation
  helper, text export, and representative test coverage visible.

### P66: Renderer Tool RHI RenderGraph Baseline Diagnostics

Goal: carry the RHI RenderGraph baseline readiness contract from `IRHIDevice`
into `SceneRenderer` tool diagnostics, so editor/profiler/frame-debugger tools
can inspect backend readiness from the same manifest they already consume for
frame and RenderGraph diagnostics.

Tasks:

- Bump the SceneRenderer tool diagnostics schema for the new RHI baseline
  payload fields.
- Export the RHI RenderGraph baseline verdict and missing requirements in
  `ExportToolDiagnosticsText()`.
- Export the same verdict and missing-requirement array in
  `ExportToolDiagnosticsManifestJson()`.
- Keep the existing captured `RHICapabilityReport` as the single source of
  truth; renderer tooling must not recompute RHI baseline rules.
- Add render pass validation coverage proving the snapshot, text export, and
  manifest JSON expose the RHI baseline readiness state.

Gate:

- Render pass validation proves renderer tool diagnostics preserve the RHI
  RenderGraph baseline verdict and missing requirements.
- `Architecture.PhaseGates` keeps the SceneRenderer tool schema bump, text
  export, manifest export, and representative test coverage visible.

### P67: Cross-Backend RHI Baseline Report Consistency

Goal: ensure real backend devices publish the same RenderGraph baseline
readiness contract that unit tests and renderer tooling consume, so DX11, DX12,
Vulkan, Metal, and OpenGL capability differences are visible through
`IRHIDevice::GetCapabilityReport()`.

Tasks:

- Add a cross-backend GPU validation test that creates every available hardware
  backend and reads `IRHIDevice::GetCapabilityReport()`.
- Verify every tested backend reports the current RHI capability schema, a
  concrete backend identity, validation success, and RenderGraph baseline
  support.
- Verify the public text export also contains the RenderGraph baseline verdict
  and missing-requirements summary.
- Keep this in the GPU/cross-backend lane, not the default architecture
  baseline, because it depends on installed backend runtimes and hardware.

Gate:

- `CrossBackendValidation.DeviceCapabilityReportRenderGraphBaselineConsistency`
  passes on available hardware GPU backends or skips when no hardware backend
  is available.
- `Architecture.PhaseGates` keeps the cross-backend test and public report/text
  export assertions visible.

### P68: RHI Capability Report Backend Identity Metadata

Goal: make RHI capability reports self-identifying with backend adapter and
driver metadata, so editor/profiler/frame-debugger captures and cross-backend
GPU failures can be attributed to a concrete GPU/driver environment instead of
only a backend API name.

Tasks:

- Bump the RHI capability report schema for backend identity metadata.
- Copy `adapterName` and `driverVersion` from public `RHICapabilities` into
  `RHICapabilityReport`.
- Export adapter and driver identity through the stable RHI capability text
  report.
- Bump SceneRenderer tool diagnostics for the new RHI identity payload fields.
- Export the same adapter and driver identity through SceneRenderer tool text
  and manifest JSON.
- Extend RHI unit, renderer diagnostics, and cross-backend GPU validation tests
  to prove report identity comes from public device capabilities.

Gate:

- RHI contract validation proves `BuildRHICapabilityReport()` and text export
  preserve adapter and driver identity.
- Render pass validation proves SceneRenderer tool diagnostics preserve the RHI
  identity metadata.
- Cross-backend validation proves real hardware backend reports match
  `IRHIDevice::GetCapabilities()`.
- `Architecture.PhaseGates` keeps the schema bumps, public fields, construction
  path, text/manifest exports, and representative tests visible.

### P69: RHI Capability Report JSON Artifact

Goal: make RHI capability reports directly consumable by tools and CI without
requiring `SceneRenderer` manifests or human-readable text parsing.

Tasks:

- Add a stable RHI capability report schema id.
- Add `ExportRHICapabilityReportJson()` as a machine-readable artifact export
  with id, kind, content type, schema id, schema version, backend identity,
  validation state, RenderGraph baseline state, summary counts, and entries.
- Add `IRHIDevice::ExportCapabilityReportJson()` as a default device-level
  diagnostics API.
- Add RHI contract validation proving the JSON payload exposes schema,
  identity, baseline, and entry data.
- Extend cross-backend GPU validation so real backend devices expose the same
  machine-readable report through the public device API.

Gate:

- RHI contract validation proves standalone JSON export is stable for tools.
- Cross-backend validation proves real hardware devices expose JSON reports
  through `IRHIDevice`.
- `Architecture.PhaseGates` keeps the schema id, JSON export API, default
  device method, payload identity fields, and representative tests visible.

### P70: Renderer Tool RHI Capability JSON Sidecar

Goal: make `SceneRenderer` saved diagnostics bundles carry the standalone RHI
capability report JSON sidecar whenever a captured RHI report is available, so
editor/profiler/frame-debugger tools can discover and validate backend
capabilities from the same artifact bundle as frame and RenderGraph diagnostics.

Tasks:

- Add `SceneRenderer::ExportToolRHICapabilityReportJson()` and
  `SaveToolRHICapabilityReportJson()` as renderer-level accessors for the
  captured `RHICapabilityReport`.
- Save a `<base>.rhi-capabilities.json` sidecar from
  `SaveToolDiagnosticsArtifacts()` when the tool diagnostics snapshot contains
  an RHI capability report.
- Include the RHI JSON sidecar in artifact result paths, byte counts, hashes,
  bundle hash, manifest discovery, artifact summary, and validation entries.
- Reuse the standalone RHI capability report schema id/version and validate the
  sidecar's id, kind, content type, schema id, and schema version.
- Keep no-RHI diagnostic captures valid by treating the RHI sidecar as a
  conditional primary artifact instead of forcing unavailable reports to fail
  offline bundles.

Gate:

- Render pass validation proves the renderer exports and saves the RHI
  capability JSON sidecar, manifests and summaries discover it, and validation
  checks its identity and schema metadata.
- `Architecture.PhaseGates` keeps the renderer export/save API, sidecar file
  name, manifest/summary/validation integration, and representative test
  coverage visible.

### P71: Shader Runtime Contract JSON Artifact

Goal: make cooked shader runtime contracts directly consumable by tools and CI,
so the resource/shader pipeline exposes stable machine-readable evidence for
shader payload identity, reflection counts, backend/stage targeting, cache keys,
and invalid-contract diagnostics.

Tasks:

- Add a stable `RVX.Resource.ShaderRuntimeContract` schema id for shader runtime
  contract artifacts.
- Add `ShaderResource::ExportRuntimeContractJson()` and
  `SaveRuntimeContractJson()` as resource-level artifact APIs independent of
  renderer diagnostics.
- Export resource identity, shader metadata, contract validity/status,
  diagnostic message, cache key, contract hash, payload hash, source hash,
  reflection resource count, and payload sizes.
- Preserve invalid contracts as first-class JSON artifacts with structured
  status names and diagnostic messages instead of requiring tools to infer
  failure from missing cache keys.
- Extend resource runtime policy validation to prove cooked shader artifacts
  and invalid in-memory shader contracts both produce stable JSON output.

Gate:

- Resource runtime policy validation proves shader runtime contract JSON exports
  schema identity, artifact identity, metadata, valid/invalid status, hashes,
  and saved-file round trips.
- `Architecture.PhaseGates` keeps the schema id, resource export/save APIs,
  payload fields, invalid-contract status export, and representative tests
  visible.

### P72: Material Shader Contract Snapshot JSON Artifact

Goal: make the material-to-shader runtime contract binding directly consumable
by editor asset inspectors, cook validation, and CI, so material resources can
explain which shader they depend on and whether that shader's runtime contract
is valid without requiring ad-hoc engine-side queries.

Tasks:

- Add a stable `RVX.Resource.MaterialShaderContractSnapshot` schema id and
  version for material shader contract snapshots.
- Add `MaterialResource::ExportShaderContractSnapshotJson()` and
  `SaveShaderContractSnapshotJson()` as resource-level diagnostics APIs.
- Export material identity, workflow/alpha mode, shader assignment/loading
  state, shader resource id, contract validity, contract hash, payload hash,
  contract key, and diagnostic message.
- Preserve invalid and missing shader states as first-class JSON artifacts so
  tools can report a material's shader dependency status deterministically.
- Extend material system validation to prove valid, invalid, and missing shader
  snapshots all produce stable JSON output, including saved-file round trips.

Gate:

- Material system validation proves material shader contract snapshot JSON
  exports schema identity, artifact identity, material identity, shader
  assignment/loading state, contract hashes, payload hashes, cache keys, and
  diagnostic messages.
- `Architecture.PhaseGates` keeps the schema id, material export/save APIs,
  payload fields, missing/invalid shader diagnostics, and representative tests
  visible.

### P73: Resource Load Diagnostic JSON Artifact

Goal: make resource load outcomes directly consumable by editor asset browsers,
runtime telemetry, cook validation, and CI, so source/cooked/package policy
failures and successful load paths are inspectable without log scraping.

Tasks:

- Add a stable `RVX.Resource.LoadDiagnostic` schema id and version for resource
  load diagnostic artifacts.
- Add `ExportResourceLoadDiagnosticJson()` and
  `SaveResourceLoadDiagnosticJson()` for standalone diagnostic serialization.
- Add `ResourceManager::ExportLastLoadDiagnosticJson()` and
  `SaveLastLoadDiagnosticJson()` convenience APIs for tools that consume the
  last synchronous or async load diagnostic.
- Export attempted/success state, domain and failure names/codes, requested and
  resolved paths, human-readable message, and source/cooked/package read flags.
- Extend resource runtime policy validation to prove denied source reads and
  successful cooked artifact loads both produce stable JSON output and saved
  artifact round trips.

Gate:

- Resource runtime policy validation proves resource load diagnostic JSON
  exports schema identity, artifact identity, domain/failure metadata, paths,
  read flags, success/failure state, and saved-file round trips.
- `Architecture.PhaseGates` keeps the schema id, standalone export/save APIs,
  ResourceManager convenience APIs, payload fields, and representative tests
  visible.

### P74: Resource Hot Reload Diagnostic JSON Artifact

Goal: make resource hot reload policy and watcher state directly consumable by
editor asset tools, runtime policy validation, and CI, so rejected cooked/package
runtime policies and active editor source-asset tracking can be diagnosed
without scraping logs or querying engine internals.

Tasks:

- Add a stable `RVX.Resource.HotReloadDiagnostic` schema id and version for
  resource hot reload diagnostic artifacts.
- Add `ExportResourceHotReloadDiagnosticJson()` and
  `SaveResourceHotReloadDiagnosticJson()` for standalone diagnostic
  serialization.
- Add `ResourceManager::ExportHotReloadDiagnosticJson()` and
  `SaveHotReloadDiagnosticJson()` convenience APIs for tools that inspect the
  current hot reload state.
- Export requested/enabled state, source-asset requirement,
  watcher-initialized state, status name/code, message, watched file count,
  registered resource count, pending reload count, and reload success/failure
  counters.
- Extend resource runtime policy validation to prove cooked-runtime rejection
  and editor source-asset tracking both produce stable JSON output and saved
  artifact round trips.

Gate:

- Resource runtime policy validation proves hot reload diagnostic JSON exports
  schema identity, artifact identity, status metadata, watcher state, tracking
  counts, enablement state, and saved-file round trips.
- `Architecture.PhaseGates` keeps the schema id, standalone export/save APIs,
  ResourceManager convenience APIs, payload fields, and representative tests
  visible.

### P75: Runtime Visible Output Baseline

Goal: make the default runtime post-process path visibly correct and aligned
with the RVX-NG postprocess best-practice target: ToneMapping remains the
runtime output anchor, ACES is the default operator, Bloom stays a no-op unless
explicitly configured, and FXAA remains a fallback/quality option rather than
the long-term default anti-aliasing strategy.

Tasks:

- Change the default runtime `SceneRenderer` post-process settings to use
  `ToneMappingOperator::ACES`.
- Preserve Bloom as a zero-intensity no-op in the default path.
- Keep FXAA disabled by default so TAA/TAAU can become the default AA mainline.
- Extend frame diagnostics with post-process fallback copy, final output
  format, and frame input availability.

Gate:

- Render pass validation proves runtime defaults use ACES, FXAA remains off by
  default, and SceneRenderer diagnostics expose fallback copy and frame input
  state.

### P76: PostProcess Shader/PSO Readiness Diagnostics

Goal: make visible post-process effects report shader/PSO readiness through
stable effect execution plans instead of only reporting a generic unsupported
state.

Tasks:

- Extend `PostProcessEffectExecutionPlan` with `pipelineReady` and
  `pipelineReadinessReason`.
- Populate readiness from the effect's supported state and unsupported reason.
- Preserve existing live fullscreen pass behavior for ToneMapping, Bloom, FXAA,
  ColorGrading, Vignette, and ChromaticAberration.

Gate:

- Render pass validation proves requested effects without resources report
  `pipelineReady == false` and a non-empty readiness reason.

### P77: PostProcess Frame Input Contract

Goal: give the post-process stack a single frame input contract for effects
that require depth, normal, velocity, jitter, history, or previous
view-projection data.

Tasks:

- Add `PostProcessFrameInputs` carrying scene color, depth, normal, velocity,
  output format, frame index, jitter, current/previous view-projection, and
  temporal reset state.
- Add `PostProcessFrameInputRequirements` and an overridable requirements hook
  on `IPostProcessPass`.
- Make `PostProcessStack::Execute()` accept frame inputs while preserving the
  old scene-color/output overload.
- Report missing frame inputs in effect execution plans and skip those effects
  without silently running partial work.

Gate:

- Render pass validation proves effects with missing depth, velocity, or
  history inputs are skipped with stable missing-input reasons.

### P78: TAA Minimal Runtime Path

Goal: promote TAA from a purely unsupported placeholder to a minimal runtime
component that owns jitter, history resources, reset state, and a safe copy
resolve fallback while reserving advanced reprojection for later work.

Tasks:

- Mark TAA supported after history/result/constant resources are created.
- Add `TAAResolveStats` for requested/supported/resolved state, history state,
  current-frame copy fallback, motion-vector availability, and jitter.
- Implement minimal resolve by copying current color into the result and history
  textures.
- Keep motion-vector reprojection and sharpening as future work rather than
  claiming full TAA quality.

Gate:

- Render pass validation proves TAA initializes, resolves through the copy
  fallback, updates history state, and reports resolve stats.

### P79: Depth and Motion Effect Input Contracts

Goal: make DOF and MotionBlur participate in the frame input contract before
their full GPU algorithms are promoted to supported runtime effects.

Tasks:

- Make DOF declare a depth input requirement.
- Make MotionBlur declare depth, velocity, and temporal history requirements.
- Keep both effects unsupported until their shader pipelines and algorithms are
  implemented.

Gate:

- Render pass validation proves DOF and MotionBlur declare the expected frame
  input requirements, while render honesty validation still prevents them from
  reporting support prematurely.

### P80: SSAO and SSR Opt-In Fallback Diagnostics

Goal: keep SSAO and SSR as opt-in advanced effects while making missing input
state diagnosable.

Tasks:

- Add SSAO compute stats for requested/supported/executed state, depth/normal
  availability, and fallback reason.
- Add SSR compute stats for requested/supported/executed state,
  color/depth/normal/roughness availability, temporal requirement, and fallback
  reason.
- Preserve unsupported status until the HiZ/AO/ray-march/resolve pipelines are
  actually implemented.

Gate:

- Render pass validation proves SSAO and SSR report missing input fallback
  diagnostics, and render honesty validation keeps them out of the supported
  set until real runtime paths exist.

## Minimal Architecture Baseline

The baseline runner intentionally avoids broad GPU/visual smoke by default. It
selects representative tests for:

- Module-boundary manifest coverage, CMake module visibility, module
  include-edge boundaries, public include-directory scope, module link
  boundaries, public-header linkage scope, and Editor/Runtime shared-core
  boundaries.
- Core debug configuration.
- RHI public contract validation and capability honesty.
- RenderGraph lifecycle, barriers, aliasing, and async fallback honesty.
- RenderScene proxy extraction and legacy fallback observability.
- Actor/Component lifecycle and legacy container rejection.
- App-mode editor/runtime contract semantics.
- Resource instantiation and World load behavior.
- Resource runtime policy and cooked/runtime package behavior.
- Render honesty checks for placeholder/stub visibility.
- System integration for Scene and Resource basics.

This set should stay fast enough to run after every architecture edit. Broader
unit, GPU, and visual lanes remain necessary before merging large phases.

## Change Discipline

- Do not fold unrelated feature work into architecture phases.
- Do not remove compatibility behavior without a test proving the replacement.
- Do not report unsupported features as success.
- Do not hide dependencies by adding include directories without a migration
  note and a follow-up gate.
- Keep phase commits small enough to review independently.
