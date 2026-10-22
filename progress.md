# Agent Work Progress

`feature_list.json` is the authoritative current state. This file is a bounded
decision and outcome log, not a second status ledger or a command transcript.
Keep at most the ten most useful recent entries.

## Recent entries

### 2026-10-22 — GFX26 strict local temporal closure

- Glass-local MAE <= 0.0005 unchanged. Forward+ clear/rough:
  0.0003878385 / 0.0001682968; Deferred: 0.0003956583 / 0.0001889609.
- Current Debug, Shipping and qualification receipts are in feature_list.json.
  Vulkan transparency/reference 16/16 passed; 14 cooked artifacts plus source
  fixture were byte-identical across independent cooks.
- Commit GFX26 only after final verification; no push; pause GFX27+.
  Golden promotion, idle performance approval and remote CI remain outstanding.

### 2026-10-02 - GFX26 final validation and review candidates

- Main transparent temporal composition, presented material / texture / LOD
  history and matched depth-motion capture are wired and tested.
- Five fixed 4070 Ti runs meet absolute budgets. User keeps the noisy baseline
  unapproved until idle calibration. Ten visual series produced review candidates.
- Final Debug exposed diagnostics dereferencing an absent logger. Guarded all
  new composition diagnostics; complete focused suite passes 9/9.
- Current-source full Debug, Shipping 5/5, Windows Vulkan 15/15 and both
  DX12 cooked series pass; all 14 tick120 payloads match review candidates.
  Golden human review remains open.
  Failed runs retained. No threshold weakening, commit or push.

### 2026-09-26 - GFX26 scheduling blocker resolved

- Exact pipeline counts and necessary relative order replace brittle indices;
  existing graph dependency and retention assertions remain.
- Related targets rebuilt; focused suite and Architecture pass. Main admission,
  linear-depth/reactive wiring and final visual/Shipping gates remain pending.
- Prior failures preserved. No Golden, tolerance, commit or push changes.

### 2026-09-26 - GFX26 graph producer blocked checkpoint

- Added sealed-draw graph MRT producer, submission retention and independent
  raw receipt. It compiles, but is not yet called by the main composition.
- After two repairs, focused test remains 2/3: candidate execution checks pass;
  two old absolute pipeline-position assertions fail with new graph ordering.
- Stopped per agreed rule; retained all failure evidence. Architecture passes
  independently, not a full/runtime qualification. No commit/push or promotion.

### 2026-09-26 - GFX26 temporal primitive verification

- Verified frame ABI, pipeline entry isolation, additive zero-alpha counting
  and exact half-float contribution behavior on DX12/Vulkan indexed draws.
- Added all-entry shader compilation across enabled backends; focused native
  and full Debug/Architecture pass with same-source immutable receipts.
- Main composition/admission, cooked visual review and Shipping remain open.
  No Golden/threshold changes, commit, push or GFX26 completion promotion.

### 2026-09-26 - GFX26 mesh history admission prerequisite

- Presented transform history now includes exact selected mesh content identity
  and deformation mode in both RenderScene and renderer caches.
- Mesh/fallback/content changes reject old shape history; unpresented frames
  cannot advance it. Unrelated resource changes keep history valid.
- Focused and full Debug/Architecture gates passed. Real transparent temporal
  attachments and visual qualification remain pending; no completion promotion.

### 2026-09-26 - GFX26 Forward flicker root cause and regression

- Fixed 12 omitted material extension fields in constant-buffer reconstruction;
  shared full copy replaces three incomplete handwritten mappings.
- Native DX12/Vulkan probes compare all shader-declared fields and reject an
  intentionally omitted field. FXC-compatible implementation retains ABI.
- Restored full-feature cooked 1280x720 series show zero static-panel temporal
  MAE on both raster paths. Full Debug and Architecture passed; no Golden,
  Shipping/performance or complete GFX26 claim. No commit/push.

### 2026-09-26 - GFX26 pixel guard and cooked scenario boundary

- Production UV selection now has native candidate/base/both-rejected and
  fractional-mip coverage. DX11 include regression fixed; full Debug passed.
- Added narrow GFX26 scenario target and strict Cooked host registration;
  moved renderer-backed diagnostics out of scenario code without weakening them.
- Captured and structurally validated two 1280x720 cooked seven-attachment
  series. Visual review exposed Forward-only static-panel flicker; disabled
  instancing does not remove it. No Golden or GFX26 completion claim.

### 2026-09-26 - gfx-24 / GFX26 conservative depth-pyramid integration

- Wired actual canonical R32 guard mips into both recipes and ThinTransmission.
- Fixed D32 Vulkan transfer aspect exposed by native graph readback; retained
  source-copy and graph provenance checks. Ultra uses exact pixel-center guards.
- Focused/native and full Debug gates passed; immutable receipts and commands
  recorded in feature_list.json. Both Development transparency scenes passed
  with zero critical diagnostics. No Cooked-only/Golden/Shipping completion.
- Next: explicit rejected-pixel matrix, temporal producers and large cooked
  series; all protected files preserved, no commit or push.

### 2026-08-22 鈥?harness-001 鈥?principle-aligned repository harness

- Decision: retain the reference package's root-level discoverability, but
  move RenderVerseX-specific detail into `Docs/agent-harness.md` so
  `AGENTS.md` remains a router.
- Decision: keep product truth in the existing capability Manifest, CMake
  inventory, Presets, and Build Truth evidence; the new feature list controls
  only current agent work.
- Decision: separate read-only preflight from verified bootstrap, delegate the
  latter to `Scripts/run_required_suite.ps1`, and persist only a small immutable
  receipt rather than copying Build Truth output into the ledger.
- Outcome: product-source and mutable control-plane fingerprints are separate;
  updating the ledger no longer invalidates the receipt it records. Preset,
  build-tree, report, JUnit, commit, and digest mismatches fail closed.
- Verification: `RVXValidationArchitecture` built successfully and the
  fail-closed `lane.architecture` suite passed 57/57. Durable evidence is
  `.agent-harness/receipts/Architecture.48b4d4672f3e4a88b11f6c8b7853fd7b.json`;
  raw report and JUnit artifacts remain under the build tree.
- Hygiene: JSON Schema/state validation, PowerShell parsing, preflight, and
  unstaged/staged diff checks passed. Bash syntax execution was optional and
  skipped because this Windows sandbox could not start its WSL-only
  `bash.exe`; the reason is recorded in the feature.
- Next use: create one bounded feature only for non-trivial, delegated, or
  cross-session work; keep product capability truth in its existing contracts.

### 2026-08-24 鈥?gfx-11 鈥?real temporal anti-aliasing

- Decision: keep one fail-closed AA mode and advance both renderer and TAA
  history only after the exact accepted frame is presented.
- Outcome: added deterministic Halton jitter, velocity/depth/reactive-mask
  reprojection, YCoCg neighborhood clamping, sharpening, persistent color/depth
  history, immutable per-submit constants, and the backend-neutral
  `temporal-aa` Sample.
- Verification: Windows Build Truth passed Architecture 60/60, Unit 1695/1695,
  Integration 695/695, and DX12 Native 59/59. GFX11 and GFX10 DX12/Vulkan
  Forward+/Deferred matrices passed 12/12 each, PR Smoke passed 3/3, and
  Shipping passed 5/5.
- Evidence: `.agent-harness/receipts/Architecture.18815a5fbce44047aced3455515011da.json`
  binds commit `fb609f50` and the current product digest.
- Follow-up: GFX12 should implement physical exposure and pre-exposure. Real
  assets under `assets/` remain optional local/Nightly qualification inputs.
