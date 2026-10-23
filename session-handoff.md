<!-- harness-handoff: gfx-24 -->

# Session Handoff

## Resume 2026-10-22

Local gates now passed: Debug BuildTruth 83350e06bfb3458f92a86607ee2d043a,
Shipping 0ecc60d488b446b897a2cdef53acfc1d (5/5), qualification
9d6b2a37b39a4696bfca23e39298538c (117/117), focused temporal 3/3 and
Vulkan transparency/reference 16/16 (gfx26-resume-vulkan.xml).
Both resume-20261022-{forward,deferred}/review.json reports passed unchanged
Tests/RenderQuality/Policies/transparency-v4-{forward-plus,deferred}.json.
Forward report SHA256 c56cce2dcda89acdfbfaeb7d4d405eb38a1bdb4ca6db727bbe47b078c40f299b;
Deferred report SHA256 f29f8f65f9104eeb3dd2d19ca07529b3a29cf81098eeacb227b05084ceaca216.
These are local candidate reviews, not approved Goldens or performance baselines.
Next: final Architecture/Close and scoped commit; then pause GFX27+.
Remaining global gfx-24 approvals/remote CI keep the coordinator active;
do not mislabel the entire graphics roadmap passing.

User reaffirmed: finish GFX26 with glass-local MAE <= 0.0005 unchanged,
commit owned changes only after all required gates pass, no push, pause GFX27+.
Architecture Verify passed: Architecture.11af743f5f584c6fa8e2bb0a1568d8e1.json.
Forward+ 1280x720 series resume-20261022-forward passed visibility and all
temporal policy checks: clear 0.0003878385, rough 0.0001682968. This is not
Golden approval. Deferred and final Debug/Shipping gates are still pending.
The latest implementation uses a stable-grid color pyramid with explicit
raster-to-color UV conversion; depth guarding remains raster aligned.
Focused test correction clears mock descriptor records between the two TAA
purpose cases so each case checks its own exposure resource. Rebuild/retest
is in progress; the prior Architecture receipt predates this test-only edit.

Latest user steering: finish GFX26 and commit its owned changes after the
required gates pass, then pause GFX27 onward. Assess the existing Samples and
model library and propose a realistic integrated scene; no new stage is
authorized for implementation yet. No push or protected-file staging.

## Active GFX26 visual-fixture refinement (2026-10-02)

Latest explicit decision: satisfy the added glass-local temporal MAE <= 0.0005
before committing. Do not defer/remove/relax it. Composition now owns one
opaque refraction history shared by both raster recipes, with fixed raster
grid / committed-producer exposure / linear-depth domains and the existing
Present transaction. This implementation is in progress, not verified.
GFX27+ stays paused; commit is authorized only after required gates pass.

Observed dependency: new per-material temporal ROIs reveal raster-jitter
movement on rejected-history pixels. Extend the existing Render scope to
TAA.cpp / TAA.hlsl stable-grid reconstruction and a real DX12/Vulkan affine
color readback test. Keep raw transparent history admission conservative;
do not raise the failed ROI thresholds. This repair still requires validation.

Observed validation dependency: the 46-case Python qualification self-test
exceeded its single 60-second CTest budget. Include the exact
Scripts/test_render_quality_qualification.py path in this coordinator's
validation responsibility: automatically discover all cases into exclusive
CTest entries and reject zero/duplicate/skipped/expected-failure outcomes.
The new GPU reference supersedes implementation-string sampling assertions;
the binding/MRT/nine-color-load checks remain. No visual threshold is widened.

User authorized improvements following the visual review. E:/g24 was removed
after SHA-256-verified archival of 37 evidence files under
.codex-validation/archives/g24-20261002; current product worktree unchanged.
Primary owns existing Sample/Render validation scope. Deliver balanced physical
lighting/exposure, equally tilted clear/frosted glass, per-material ROIs and
visual contract v4. Prior v3 candidates remain unapproved historical evidence.
Baseline Architecture 50eaaeab56ee4274a6f0181c72cdfc54 is current before edits.
Run Cooked host/assets build, focused image/contract checks, deterministic cook,
presented DX12 Forward/Deferred series, then final native/Debug/Shipping gates.
Record exact receipts before Close; no commit/push or performance approval.

## GFX26 local implementation verified (2026-10-02)

Coordinator primary; branch codex/runtime-architecture-validation,
HEAD 31b28d0c. Keep the existing scope and protected exclusions.
No commit, push, Fresh, dependency installation or protected-file edits.

Main composition is wired: sealed temporal raster, presented material /
texture / LOD history, raw admission, separate mesh reactivity, TAA and
matched linear-depth capture. GPUScene staging first-use ownership follows
the frozen RenderGraph queue policy. Optional composition diagnostics no
longer require an initialized global logger; the regression suite passes 9/9.

Current product-source digest:
e04366456e0c90e3c11aa31e131c2504b6e5bafb4ad98961198e88daf44065a7.
Exact commands, hashes and identities are in feature_list.json:
- BuildTruth.3f63cb5c2abc41e8b4134080f67a5ec1:
  Architecture 65, Unit 2126, Integration 771, DX12 Native 82, all passed.
- RequiredSuite.d39ad9f064dc4def9c85c044e10b6c6f: Shipping 5/5 passed.
- Architecture.50eaaeab56ee4274a6f0181c72cdfc54: canonical Verify passed.
- gfx26-final-vulkan.xml: Windows Vulkan native transmission / temporal 15/15.
- current-final-20261002-{forward,deferred}: latest cooked same-process
  ticks 120/360/361, seven attachments, clean debug queue and series validation.
  Tick120 matches all 14 unapproved candidate payloads byte-for-byte.

NEXT: human review and approval of both candidate Goldens and visual policies:
build/win_x64_debug/Tests/Gfx26Completion/GoldenReview/{forward,deferred}-candidate.
REVIEW.md and beauty-1280x720.png describe the generated regression fixture.
Do not auto-promote it or claim a photorealistic art showcase. After explicit
approval, use Scripts/render_quality_golden.py approve, preserve .raw LFS
policy, and run approved compare-series against the current captures.

Five prior fixed 4070 Ti 2560x1440 High runs pass both absolute budgets.
Candidate baseline:
build/gfx24_perf_calibration/PerformanceCalibration/510404e7cb584129949ec7d913a948b5/candidate/baseline-record.json.
User explicitly chose to keep it unapproved and recalibrate when idle.
Its source precedes the logger guard; it is not final-source relative
performance approval. No 10 percent regression-baseline claim.

Prior failures are retained, including the repaired Copy ownership failure,
the repaired null-logger Integration failure, and the separately intermittent
source-publication/World teardown timeout. Later successful captures do not
prove a root fix for that separate stall. Remote Linux/macOS/Metal and broader
gfx-24 qualification remain pending. Keep gfx-24 active/non-complete.
No validation process remains running after lifecycle Close.
