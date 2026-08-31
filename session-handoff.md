<!-- harness-handoff: gfx-24 -->

# Session Handoff

GFX24A3 is complete through `0aaddeaf`. High now uses a ceil-half SSR trace
with depth/normal/confidence-guided full-resolution resolve; Ultra and
non-High tiers retain full-resolution SSR. Bloom no longer repeats a 13-tap
kernel for each already-filtered pyramid level, current-frame TAA inputs use
pixel-aligned loads while history remains bilinear, and the transient pool
reserves physical resources/views for both renderer frame slots before
measurement. Sample report schema 35 carries exact SSR resolution evidence;
schema 34 remains legacy.

The final product digest
`a3fbd9c8d3fc0b492fd1776d7d740733735955b4c732720e1d078173e94be90f`
passes Architecture 65/65, Unit 2072/2072, Integration 756/756, real DX12
Native 61/61, PR Smoke 3/3, GFX19 DX12/Vulkan reflection qualification 12/12,
and Shipping 5/5. The post-commit Candidate receipt is
`.agent-harness/receipts/RequiredSuite.96116d630e9d485693940cb638f758ac.json`;
its 60+300 report records 11.141120 ms Graphics qualification p95,
10.852352 ms RenderGraph envelope p95, 44 stable transient textures, and
689563800 stable transient bytes. No resource or footprint grew after warmup.

Keep `gfx-24` active. Next implement GFX24A4 calibration: retain five
consecutive reports for the fixed RTX 4070 Ti / driver / OS identity, choose
the median-p95 run, emit `RenderQualityPerformanceBaselineRecord V1`, and ask
the user for explicit approval. No performance baseline or visual Golden is
approved. GFX24B/C and Linux hardware Vulkan RT/macOS Metal evidence remain
pending.

Preserve the existing user-owned changes in `AGENTS.md`, `CLAUDE.md`,
`Docs/README.md`, `Docs/engine-capabilities.json`, `ViewData.h`,
`GPUSceneRaster.hlsli`, and the deletion of `BACKEND_ANALYSIS.md`. Do not edit
or stage `.agent-harness/`. Do not push or modify master.
