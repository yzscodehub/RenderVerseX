# RenderVerseX Runtime-First Engine Goal

## Target

RenderVerseX should converge into a production-grade C++20 realtime rendering and game-engine foundation with a Runtime-first delivery path. The first stable milestone is not a complete commercial editor or every high-end rendering feature. It is a clean, testable, diagnosable engine core that can initialize without the Editor, load cooked or packaged content, build render-facing scene snapshots, execute RenderGraph-driven frames, and present its capabilities honestly through reports, samples, and validation artifacts.

## Non-Negotiables

- Runtime is the product path for this milestone. Editor is explicitly disabled and is not part of build or acceptance.
- Module boundaries are enforceable. Feature modules expose value configuration, ECS bridges, and snapshot contracts; Render owns GPU/RHI execution.
- RenderGraph is the main GPU work orchestration path. Standalone or feature-owned GPU calls must either move behind Render-owned passes or be isolated as legacy budget with a removal plan.
- RHI capabilities are explicit. Supported, fallback, skipped, and unsupported states must be visible in diagnostics and tests.
- Resource loading is shipping-oriented. Runtime cannot silently fall back to source assets, and cooked/package artifacts must report structured failure reasons.
- Runtime scene authority is exclusively `Engine -> World -> SceneEcsRuntime`; entities use generation-safe handles, data-only fragments, queries, processors, and command barriers.
- Actor, SceneEntity, SceneManager, ActorComponent, Component, compatibility wrappers, and dual-written scene state do not exist in the production Runtime.
- Visual quality grows in low-tier, cross-backend slices first. Advanced effects are capability-gated until they are real.
- Samples are the public proof surface while Editor is deferred. Every showcase should have smoke execution, screenshot/report output, and eventually visual regression coverage.
- Stubs, placeholders, and deferred features must not report success. They either work, fail with a structured reason, or are explicitly marked unsupported.

## First Stable Milestone

The first stable Runtime-first milestone is reached when:

- Architecture baseline and module boundary gates are green on the main debug build.
- Runtime can initialize, load cooked/package resources, create a pure ECS World, freeze a Scene snapshot, and render a frame without Editor.
- Particle, Water, and Terrain public APIs expose no Render/RHI types, and their GPU work is either Render-owned or isolated behind shrinking legacy budgets.
- Post-process and lighting basics have Runtime paths, effect status reports, and sample coverage.
- Resource policy proves source-denied Runtime behavior and package/cooked artifact loading with missing/hash-mismatch diagnostics.
- The 13 `RenderVerseSamples` scenes share one CLI/report contract and are the visual and lifecycle acceptance surface.

## Current Execution Order

1. Keep every production subsystem on `EntityRef`, fragment queries, value commands, and frozen snapshots.
2. Run the 13-sample DX12/Vulkan Direct/GPU-driven correctness and cleanup matrix.
3. Parallelize only processor groups whose declared access sets prove no conflict.
4. Strengthen cooked/package fixtures, residency proofs, and retirement diagnostics.
5. Keep Editor explicitly deferred until it is redesigned directly against the ECS contracts.
