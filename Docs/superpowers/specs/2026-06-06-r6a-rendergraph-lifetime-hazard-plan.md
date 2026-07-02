# R6a RenderGraph Lifetime and Hazard Validation Plan

**Date:** 2026-06-06  
**Parent stage:** R6 - RenderGraph Hardening  
**Sub-stage:** R6a - Resource lifetime and read/write hazard validation  
**Plan source:** `Docs/superpowers/specs/2026-05-30-render-program-plan-v1.md`, section 12.

---

## 1. Stage Goal

Make RenderGraph reject resource lifetime hazards that can currently compile as
valid, especially transient resources being read before any producing write and
transient exports that were never written.

R6 is size L, so this sub-stage deliberately targets the correctness gap that is
closest to "false success" while preserving the current graph execution model.

---

## 2. Current Code Findings

- `RenderGraphValidation` already covers invalid texture/buffer handles,
  incompatible pass state, empty usage warnings, subresource/range barrier
  preservation, explicit async fallback, and aliasing-disabled visibility.
- `CompileRenderGraph()` now fails invalid handles/state and avoids execution
  order fallback, but transient reads can still be accepted even when no prior
  pass produced the resource.
- `SetExportState()` can mark a transient resource for export even if no
  non-culled pass writes it.
- Imported resources have an external initial state and must remain valid to
  read before a later write.
- Detailed per-subresource/per-range initialization is larger than this
  sub-stage; R6a can add whole-resource lifetime validation first without
  changing barrier generation.

---

## 3. Approved Scope

1. Add visible compile statistics for lifetime hazards.
   - `readBeforeWriteHazardCount`
   - `uninitializedExportCount`
   - Keep `validationErrorCount` and `compileValid` as the hard gate.

2. Add a RenderGraph lifetime validation pass.
   - Run after usage classification and culling reachability are known.
   - Treat imported resources as initialized from their imported initial state.
   - Treat transient resources as initialized only after a needed pass writes
     them.
   - `Read` or `ReadWrite` of an uninitialized transient texture/buffer is a
     compile error.
   - Exporting a transient texture/buffer that was never initialized by a
     needed write is a compile error.
   - Exporting an unwritten transient resource is a compile error even when no
     pass reads it; `SetExportState()` is a request for externally visible
     contents, so the resource must be produced by the graph.
   - Culled passes should not poison the graph with hazards because they do not
     execute.

3. Ensure invalid lifetime graphs do not execute.
   - Existing `ExecuteRenderGraph()` skips when `compileValid == false`; add a
     focused validation case proving this behavior for the new hazard.

4. Extend `RenderGraphValidation`.
   - Transient texture read-before-write fails compile.
   - Transient buffer read-before-write fails compile.
   - Transient texture/buffer `ReadWrite` before initialization fails compile.
   - Transient export without writer fails compile.
   - Imported read-before-later-write remains valid.
   - Culled uninitialized transient read does not invalidate the graph.
   - Invalid lifetime graph does not call pass execute.
   - Lifetime hazard stats reset on a later clean `Compile()` / `Clear()` path.
   - Existing imported read-before-later-write tests should be annotated or
     paired with a transient contrast so the behavior distinction stays obvious.

---

## 4. Out of Scope

- Full cycle construction APIs or external dependency graph API.
- Re-enabling memory aliasing or emitting native aliasing barriers.
- Per-subresource/per-buffer-range initialized-region tracking.
- Real async compute/copy scheduler or multi-queue submission.
- RenderGraph visual pass completion.
- Visual golden gate; R7 owns baseline creation.
- RenderProxy, material, asset, or ModelViewer work.

---

## 5. Expected Files

- `Render/Include/Render/Graph/RenderGraph.h`
- `Render/Private/Graph/RenderGraphCompiler.cpp`
- `Tests/RenderGraphValidation/main.cpp`
- `Docs/superpowers/specs/phase-log.md`

---

## 6. Implementation Steps

1. Add compile stats fields for lifetime validation.
2. Implement a small lifetime validation helper in
   `RenderGraphCompiler.cpp`.
3. Call the helper after pass reachability/culling is known and before
   dependency sorting/barrier generation.
4. If lifetime validation reports errors, set `compileValid=false`, clear
   `executionOrder`, and return before resource creation.
5. Add focused `RenderGraphValidation` tests.
6. Run required validation.
7. Run Spark code review.
8. Update `phase-log.md`.
9. Commit R6a.

---

## 7. Validation Commands

```powershell
cmake --build build/win_x64_debug --config Debug --target RenderGraphValidation RenderHonestyValidation
build\win_x64_debug\Tests\Debug\RenderGraphValidation.exe
build\win_x64_debug\Tests\Debug\RenderHonestyValidation.exe
ctest --test-dir build/win_x64_debug -C Debug --output-on-failure -R "RenderGraphValidation|RenderHonestyValidation"
git diff --check
```

Optional regression set before commit:

```powershell
cmake --build build/win_x64_debug --config Debug --target RenderGraphValidation RenderHonestyValidation RenderPassValidation RenderSceneValidation MaterialSystemValidation
ctest --test-dir build/win_x64_debug -C Debug --output-on-failure -R "RenderGraphValidation|RenderHonestyValidation|RenderPassValidation|RenderSceneValidation|MaterialSystemValidation"
```

---

## 8. Done Criteria

- Transient read-before-write hazards are compile errors with visible stats.
- Transient exports without a producing write are compile errors with visible
  stats.
- Imported resource read-before-write remains valid.
- Culled hazards do not fail graphs that never execute the hazardous pass.
- Invalid lifetime graphs do not execute callbacks.
- Spark plan review and code review both pass.
- R6a has a dedicated commit before any R6b/R7/R8 work begins.
