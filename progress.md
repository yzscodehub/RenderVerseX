# Agent Work Progress

`feature_list.json` is the authoritative current state. This file is a bounded
decision and outcome log, not a second status ledger or a command transcript.
Keep at most the ten most useful recent entries.

## Recent entries

### 2026-08-22 — harness-001 — principle-aligned repository harness

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

### 2026-08-24 — gfx-11 — real temporal anti-aliasing

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

### 2026-08-24 - gfx-12 - physical exposure and pre-exposure

- Decision: resolve one presentation-qualified exposure state per view; Manual
  EV100 and histogram auto exposure share the same pre-exposure contract across
  lighting, bloom, TAA, reflections, and tone mapping.
- Outcome: checkpoint `68907c48` adds the physical exposure contract, GPU
  histogram/adaptation path, immutable presentation-qualified history,
  pre-exposed HDR producers/consumers, deterministic Manual/Auto Samples, and
  Vulkan acquired-image abort retirement with real native coverage.
- Verification: Build Truth passed Architecture 60/60, Unit 1709/1709,
  Integration 703/703, and DX12 Native 59/59. The DX12/Vulkan Forward+/Deferred
  exposure matrix passed 24/24, PR Smoke passed 3/3, and Shipping passed 5/5.
- Evidence: `.agent-harness/receipts/Architecture.ef50c07d15ae481ca7d777efc9281afb.json`
  binds commit `68907c48` and product digest
  `86a44994efa03b7a1d73b537eb08e28ded994928f063788bfe48151656d864ee`.
- Assets: the ignored local model/HDR pool is catalogued for later cooked-only
  PBR, animation, reflection, transparency, IBL, and exposure Samples; it is not
  part of GFX12 and will not be bulk committed.
- Follow-up: GFX13 should replace SSAO with GTAO while preserving direct,
  emissive, sky, and translucent energy and reusing the established temporal
  identity and exposure contracts.

### 2026-08-26 - gfx-13 - ground-truth ambient occlusion

- Decision: replace the legacy scene-color SSAO path with one typed GTAO field
  shared by Forward+ and Deferred, using geometry-aware bilateral filtering and
  presentation-qualified temporal history.
- Outcome: checkpoint `9725fe5f` adds the multi-pass GTAO feature, canonical
  depth/normal/velocity inputs, indirect-only lighting application, strict
  execution receipts, and DX12/Vulkan qualification coverage; legacy SSAO was
  removed without fallback.
- Verification: Build Truth passed Architecture 60/60, Unit 1732/1732,
  Integration 711/711, and DX12 Native 59/59. GTAO passed 12/12, PR Smoke 3/3,
  Shipping 5/5, focused tests 276/276, and a supplemental 1280x720 capture
  passed structural image/report validation.
- Evidence: `.agent-harness/receipts/Architecture.5ad9b059771847ba99357c97b5e62e74.json`
  binds commit `9725fe5f` and product digest
  `abd979249994806e152cf9c08191825d13f7ddafdb368cc5c25981c03a9d9697`.
- Follow-up: GFX14 adds ToonLit and outlines. Moving-camera AO flicker,
  Golden/ROI/SSIM, and multi-frame perceptual qualification remain for GFX24.

### 2026-08-26 - gfx-14 / gfx-15 - stylized and translucent surfaces

- Outcome: checkpoint `cf641c87` adds shared Forward+/Deferred ToonLit and
  typed screen-space outlines; checkpoint `c5ff6217` adds stable sorted
  translucency and ThinTransmission without a legacy reader or fallback.
- Verification: the GFX15 regression source fingerprint passed Architecture
  61/61, Unit 1760/1760, Integration 718/718, DX12/Vulkan qualification 12/12,
  and Shipping 5/5 before the checkpoint commit.
- Ledger: completed renderer checkpoints remain durable in Git; stale mutable
  report references were pruned from the current-work queue instead of being
  rewritten as historical evidence.
- Follow-up: GFX16 implements deterministic CPU billboard packets,
  Render-owned rotating instance buffers, cooked shaders, and a neutral
  cooked-only `particle-billboards` Sample. Fire and smoke stay in GFX17.

### 2026-08-27 - gfx-16 - deterministic CPU particle billboards

- Decision: publish one immutable, generation-qualified CPU packet and keep
  sorting, rotating upload buffers, descriptors, pipelines, and draw batching
  entirely Render-owned.
- Outcome: checkpoint `684a02e9` adds stable world-space billboard packets,
  Alpha/Premultiplied/Additive batching, cooked shaders, fail-closed unsupported
  modes, and real DX12/Vulkan RGBA16F native pixel evidence.
- Verification: the final commit passed focused tests 236/236, the strict
  GFX16 native suite 4/4, Architecture 61/61, Unit 1768/1768, Integration
  718/718, DX12 Native 59/59, and Shipping 5/5.
- Evidence: `.agent-harness/receipts/Architecture.80a021e95e4c40b283343c0bee26dd67.json`
  binds commit `684a02e9` and product digest
  `e29f86e696730438a05a8e14d5e5cbb52ad594b41135606e36ca6b0ebcb4e2b2`.
- Follow-up: GFX17 adds deterministic flipbook fire/smoke, soft particles,
  typed distortion composition, reactive masks, and optional low-frequency
  dynamic lights without widening Sample access to RHI or Render internals.

### 2026-08-27 - gfx-17 - deterministic fire, smoke, and distortion

- Outcome: checkpoint `b08d128e` adds strict cooked particle-effect artifacts,
  deterministic fire/smoke packets, soft-depth fading, typed distortion and
  reactive-mask graph outputs, backend-neutral cooked-only Samples, and exact
  execution receipts. Checkpoint `c7bd1e76` separately normalizes two inherited
  mixed-line-ending files so the aggregate `master...HEAD` PR diff is clean.
- Verification: focused targets built and focused tests passed 349/349; full
  Build Truth passed Architecture 61/61, Unit 1782/1782, Integration 725/725,
  and DX12 Native 59/59; Shipping passed 5/5; the GFX17 DX12/Vulkan ×
  Forward+/Deferred × fire/smoke qualification matrix passed 24/24.
- Evidence: `.agent-harness/receipts/Architecture.a6c26996f9874107b1dd8223d091a1c2.json`
  binds commit `c7bd1e76` and product digest
  `8a57d46992b1fba59edb7cc5c31dfdfdae6e84074078c6b3e24d7f61da2ad7a6`.
- Follow-up: GFX18 implements deterministic Gerstner water parameters and
  gameplay queries, Render-owned surface rendering, and `water-surface`.

### 2026-08-27 - gfx-18 - deterministic Gerstner water surfaces

- Outcome: checkpoint `9dcd2d45` adds a bounded canonical Gerstner contract,
  pure gameplay queries, generation-safe ECS publication, Render-owned grid and
  rotating resources, layered normals, Fresnel, depth thickness,
  Beer-Lambert absorption, foam, reactive-mask output, and the cooked-only
  backend-neutral `water-surface` Sample. Validation hardening concludes at
  `325b7ed0` with immutable v2 gate receipts and deterministic PowerShell 7
  execution.
- Verification: focused GFX18 tests passed 260/260; water qualification passed
  12/12 across DX12/Vulkan and Forward+/Deferred. Current Build Truth passed
  Architecture 61/61, Unit 1815/1815, Integration 725/725, and DX12 Native
  59/59. GFX17 revalidation passed 24/24, GFX16 native passed 4/4, and Shipping
  passed 5/5.
- Evidence: `.agent-harness/receipts/Architecture.654348beec1c4c308552aac852453cd2.json`
  binds commit `325b7ed0` and product digest
  `15c1366c91435954dc37ccb05b640f4cda3d8591d1413f55b5b366e136e0b335`.
- Follow-up: GFX19 should add local shadow atlases, reflection probes, Hi-Z SSR
  confidence, and an explicit SSR-to-probe-to-IBL fallback chain.

### 2026-08-28 - gfx-19 - local shadows, probes, and Hi-Z reflections

- Outcome: GFX19 adds a deterministic Render-owned point/spot shadow atlas,
  bounded reflection-probe selection and blending, validated Hi-Z reduction,
  explicit SSR confidence and rejection reasons, and one shared
  SSR-to-Probe-to-IBL fallback chain for Forward+ and Deferred.
- Verification: focused tests passed 303/303; the DX12/Vulkan Forward+/Deferred
  local-lighting matrix passed 12/12; the matching reflection matrix passed
  12/12; Build Truth passed Architecture 61/61, Unit 1888/1888, Integration
  737/737, and DX12 Native 59/59; Shipping passed 5/5.
- Evidence: `.agent-harness/receipts/Architecture.a6525347eb0d468babbfca94e9c0b701.json`
  binds commit `325b7ed0` and product digest
  `0d066784d70f78773a197112aaaa367324df7324b0a408868a08774666c9b5f6`.
- Follow-up: GFX20 should productionize Vulkan ray tracing with acceleration
  structures, pipelines, SBT, descriptors, barriers, dispatch, cooked shaders,
  and native hardware evidence.

### 2026-08-28 - gfx-20 - production Vulkan ray tracing RHI

- Outcome: GFX20 adds fail-closed Vulkan ray-tracing capability discovery,
  validated BLAS/TLAS sizing and submission-owned build state, strict update
  signatures, RT pipelines and aligned SBT regions, acceleration-structure
  descriptors, precise barriers, `vkCmdTraceRaysKHR`, and six-stage cooked
  SPIR-V artifact support without a source or legacy fallback.
- Verification: focused tests passed 139/139; the GFX20 native suite passed 3/3
  on an RTX 4070 Ti with deterministic sentinel `202399319` and zero Vulkan
  validation errors or warnings; Build Truth passed Architecture 61/61, Unit
  1888/1888, Integration 737/737, and DX12 Native 59/59; after rebuilding the
  independently configured host RVXCook, Shipping passed 5/5.
- Evidence: `.agent-harness/receipts/RequiredSuite.e7ac8422af7e457cbd8d48c29d506945.json`
  and `.agent-harness/receipts/BuildTruth.3a1f41b238144aff95d5a3ceb2ca4ad3.json`
  bind commit `325b7ed0` and product digest
  `40385914d728b1b3b5fd632272109cb6b39b2ce349656dd5f38560306ca9d17a`.
- Follow-up: GFX21 should share a backend-neutral RayTracingScene across DX12
  and Vulkan, add cacheable/compactable static BLAS plus rigid TLAS updates,
  masked any-hit, half-resolution RT reflections, temporal/spatial denoising,
  and exact required-versus-optional fallback evidence.

### 2026-08-28 - gfx-21 - production hybrid ray-traced shadows and reflections

- Outcome: GFX21 adds one backend-neutral, generation-qualified
  `RayTracingScene` for DX12 and Vulkan, reusable and compactable static BLAS,
  rigid TLAS updates, masked any-hit, primary directional RT shadows,
  half-resolution RT reflections, spatial/temporal denoising, and strict
  required-versus-optional fallback receipts. Required-suite selection now also
  rejects duplicate test identities.
- Verification: the exact GFX21 DX12/Vulkan native suite passed 8/8 with eight
  unique tests; focused regression passed 1128/1128; Build Truth passed
  Architecture 61/61, Unit 1910/1910, Integration 741/741, and DX12 Native
  60/60; Shipping passed 5/5; diff hygiene passed.
- Evidence: `.agent-harness/receipts/RequiredSuite.59afbd73011d47ac93f433358fe9250e.json`,
  `.agent-harness/receipts/BuildTruth.8f74bedf01cf4a7aa7f5127c0115cd41.json`,
  and `.agent-harness/receipts/Architecture.7c3ccd2ada1a4342ac5b207490e6a519.json`
  bind commit `325b7ed0` and product digest
  `fcd9f0590b5d8aa66960d943aca864dd6b0ff335a7886cc45bf64aece42edded`.
- Follow-up: GFX22 should add weighted blended OIT for eligible non-refractive
  alpha and particle materials, keep refractive glass on the sorted path, and
  fail closed when independent floating-point blending is unavailable.

### 2026-08-28 - gfx-22 - weighted blended order-independent translucency

- Outcome: GFX22 adds backend-native independent floating-point blend
  capability reporting, one immutable mesh/particle translucency plan,
  graph-owned RGBA16F accumulation plus R16F revealage, a scene-linear HDR
  composite, strict sorted refraction, and direct additive routing shared by
  Forward+ and Deferred. Mixed mesh/particle sorted fallback fails closed until
  one global ordering owner exists; no false sorted claim is emitted.
- Verification: focused regression passed 897/897; the exact DX12/Vulkan GFX22
  cooked-shader suite passed 8/8; Build Truth passed Architecture 61/61, Unit
  1944/1944, Integration 745/745, and DX12 Native 61/61; Shipping passed 5/5;
  diff hygiene passed.
- Evidence: `.agent-harness/receipts/RequiredSuite.20958236d4824c5aa4477b4f67302d10.json`,
  `.agent-harness/receipts/BuildTruth.bf2239de0f444958a83c19e52c245ff6.json`,
  and `.agent-harness/receipts/Architecture.57db44b42b8d464399031924f7d25c85.json`
  bind commit `325b7ed0` and product digest
  `06e9fa064d583782334b2efc214524a1a0868e469bd51c7399ca13e27faa574a`.
- Follow-up: macOS Metal native confirmation remains pending remote CI; GFX23
  should add froxel volumetric lighting as a separately claimed feature.

### 2026-08-29 - gfx-22a - renderer ownership and composition convergence

- Outcome: introduced one `DefaultRenderComposition` owner and one
  `DefaultRenderPassBundle`; `SceneRenderer` now orchestrates them without
  retaining built-in feature or concrete pass pointer caches. Feature adapters
  borrow the bundle and resolve pass dependencies only for the current call.
  Destruction, move assignment, pre-initialize custom passes, partial
  initialization, and duplicate installation now have balanced lifecycle
  coverage. A fail-closed Architecture checker prevents central pass
  reconstruction and mutable pass-pointer ownership from returning.
- Verification: checker self-tests passed 8/8 and focused tests passed 76/76;
  after the line-ending-only cleanup in `43c56f79`, the complete Debug Build
  Truth passed branch and working diff hygiene, Architecture 63/63, Unit
  1947/1947, Integration 745/745, and real DX12 Native 61/61 with no backend
  fallback or software adapter. Shipping passed 5/5 before the mechanical
  cleanup and will be rebound after the implementation commit.
- Evidence: Architecture receipt
  `.agent-harness/receipts/Architecture.33227f7de8944e649bffba00529469a5.json`,
  Shipping receipt
  `.agent-harness/receipts/RequiredSuite.1addaa46297a41e884287c83c33b988a.json`,
  and Build Truth receipt
  `.agent-harness/receipts/BuildTruth.e5274260b1b5457db981bfa622e8c8ac.json`
  bind commit `43c56f79` and
  product digest
  `a0c4e0119dc233471743c19ac9913c0be96b0c01000b1084c5e44e42eb0392e7`.
- Follow-up: commit the reviewed GFX22A paths, rerun Architecture, complete
  Debug Build Truth, and Shipping against the new HEAD, then transition the
  feature to passing before starting GFX23.
