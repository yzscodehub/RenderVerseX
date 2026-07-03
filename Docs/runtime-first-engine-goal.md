# RenderVerseX Runtime-First Engine Goal

## Target

RenderVerseX should converge into a production-grade C++20 realtime rendering and game-engine foundation with a Runtime-first delivery path. The first stable milestone is not a complete commercial editor or every high-end rendering feature. It is a clean, testable, diagnosable engine core that can initialize without the Editor, load cooked or packaged content, build render-facing scene snapshots, execute RenderGraph-driven frames, and present its capabilities honestly through reports, samples, and validation artifacts.

## Non-Negotiables

- Runtime is the primary product path. Editor remains a thin client over Runtime diagnostics and must not be required for core correctness.
- Module boundaries are enforceable. Feature modules expose CPU simulation, configuration, components, and snapshot contracts; Render owns GPU/RHI execution.
- RenderGraph is the main GPU work orchestration path. Standalone or feature-owned GPU calls must either move behind Render-owned passes or be isolated as legacy budget with a removal plan.
- RHI capabilities are explicit. Supported, fallback, skipped, and unsupported states must be visible in diagnostics and tests.
- Resource loading is shipping-oriented. Runtime cannot silently fall back to source assets, and cooked/package artifacts must report structured failure reasons.
- Components converge on ActorComponent for production paths. Legacy Component remains only for compatibility and tests.
- Visual quality grows in low-tier, cross-backend slices first. Advanced effects are capability-gated until they are real.
- Samples are the public proof surface while Editor is deferred. Every showcase should have smoke execution, screenshot/report output, and eventually visual regression coverage.
- Stubs, placeholders, and deferred features must not report success. They either work, fail with a structured reason, or are explicitly marked unsupported.

## First Stable Milestone

The first stable Runtime-first milestone is reached when:

- Architecture baseline and module boundary gates are green on the main debug build.
- Runtime can initialize, load cooked/package resources, create a World, extract render-facing snapshots, and render a frame without Editor.
- Particle, Water, and Terrain public APIs expose no Render/RHI types, and their GPU work is either Render-owned or isolated behind shrinking legacy budgets.
- Post-process and lighting basics have Runtime paths, effect status reports, and sample coverage.
- Resource policy proves source-denied Runtime behavior and package/cooked artifact loading with missing/hash-mismatch diagnostics.
- Samples/Showcase targets share one CLI/report contract and can be used as visual regression entry points.

## Current Execution Order

1. Shrink Feature to Render/RHI legacy edges, starting with Particle, then Water, then Terrain.
2. Complete Samples showcase reports and smoke coverage.
3. Turn visual effects into low-tier Runtime implementations with honest fallback status.
4. Strengthen resource runtime/package fixtures and GPU residency diagnostics.
5. Classify remaining stubs into implemented, unsupported-with-diagnostic, or Editor-deferred.
