<!-- harness-handoff: gfx-24 -->

# Session Handoff

GFX23 is complete. GFX24 implementation is committed in `18572c63`, with the
performance-candidate correction in `ce0846ba`. The public-only cooked
RenderQualityLab, completion-owned seven-attachment capture, ordered per-pass
timing, 60+300 performance session, report checkers, Shipping closure, and
fail-closed required suites are implemented and locally verified.

Current `ce0846ba` evidence passes GFX24 contract 62/62, Architecture 65/65,
Unit 2040/2040, Integration 751/751, real DX12 Native 61/61, Shipping 5/5,
fixed SceneLifecycle PR Smoke 3/3, and the DX12 Release candidate producers
3/3. The candidate emitted two 1280x720 seven-attachment bundles and one
2560x1440 report with 60 warmup plus 300 measured frames. Its Graphics
qualification-span p95 is 18.874368 ms on the local RTX 4070 Ti, so the 16.67
ms target is not yet met. The baseline identity remains
`gfx24-dx12-candidate-unapproved`; no Golden or performance baseline was
approved.

Keep `gfx-24` active. Next, establish the fixed-CI-GPU baseline and approved
visual Golden, then optimize `OpaqueSceneSnapshotCopy`, `TAAResolve`,
`SSRHiZTrace`, and `WeightedBlendedOITComposite` before rerunning the exact
GFX24 candidate required suite. Linux Vulkan and macOS Metal visual evidence
remain remote PR-CI obligations. The required-suite PowerShell relaunch also
needs a separate follow-up for hosts that inherit both `PATH` and `Path`; the
current runner succeeds after direct CMake prebuild but should be hardened.

Preserve the existing user-owned changes in `AGENTS.md`, `CLAUDE.md`,
`Docs/README.md`, `Docs/engine-capabilities.json`, `ViewData.h`,
`GPUSceneRaster.hlsli`, and the deletion of `BACKEND_ANALYSIS.md`. Do not edit
or stage `.agent-harness/`. Do not push or modify master.
