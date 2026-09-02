<!-- harness-handoff: gfx-24 -->

# Session Handoff

GFX24 closeout implementation is committed through `a79cee88`:

- `2afce63b` hardens Golden preparation so five Series at ticks 300, 360,
  and 361 are all compared before each temporal pair is validated.
- `cbb38ddd` adds three-platform raster correctness plus Windows DX12 and
  Linux Vulkan hardware-RT CI topology.
- `7b8e11b0` replaces unbounded nested Cook transaction names with bounded
  42-character components and covers a 128-character parent plus replacement
  cook.
- `a79cee88` keeps the new validation presets internal, so the public engine
  capability contract remains unchanged.

Clean-tree evidence at `a79cee88` passes Architecture 65/65. CI policy
self-tests pass 39/39, the engine capability contract passes, DX12 hardware RT
passes 3/3, SkeletalAssetPipelineValidation passes 12/12, and the full Windows
GFX24 raster aggregate builds past the former Water fixture MAX_PATH failure.
An exact-current Shipping rerun was not produced because a new clean build
tree stalled fetching the vcpkg registry; the previous product-digest Shipping
5/5 evidence remains valid for the unchanged runtime product, while exact-tip
Shipping stays a required remote job.

Formal B2 evidence lives under
`E:/g24/build/gfx24_perf_calibration/B2Visual/7b8e11b0`. Forward+ and Deferred
each have five valid same-process Series for ticks 300/360/361. Candidate
preparation passed all 30 same-tick bundle comparisons and 10 temporal pairs
under zero-tolerance VisualPolicy V2. Both records remain `candidate`, and
`--require-approved` correctly rejects them. They bind RTX 4070 Ti, driver
`32.0.16.1062`, Windows `10.0.19045`, and selected Series ordinal 3.

Keep `gfx-24` active. Two local human/environment gates remain:

1. The user must visually review and explicitly approve both Golden candidates.
   Switch `E:/g24` back to `7b8e11b0` before running the approval tool so
   the clean approval source matches each record.
2. The rejected A4 calibration set must not be reused or spliced. Only after
   the interactive RTX 4070 Ti is demonstrably idle, switch the evidence tree
   to the current tip and rerun all five 60+300 reports in one new session.
   Prepare the median-p95 BaselineRecord and request explicit approval.

After both approvals, commit the approved Golden/policy/baseline records,
replace CI candidate calibration with approved baseline/Golden comparison,
then obtain hosted Windows/Linux/macOS raster and self-hosted DX12/Vulkan RT
evidence. Linux hardware runner absence or any missing required job keeps
GFX24 active.

Preserve the existing user-owned changes in `AGENTS.md`, `CLAUDE.md`,
`Docs/README.md`, `Docs/engine-capabilities.json`, `ViewData.h`,
`GPUSceneRaster.hlsli`, and the deletion of `BACKEND_ANALYSIS.md`. Do not edit
or stage `.agent-harness/`. Do not push or modify master.
