# M1 Exit Remediation Implementation Plan

**Date:** 2026-07-27
**Branch:** `codex/architecture-implementation`

## Purpose

Restore the M1 production baseline on the required Windows, Linux, and macOS
CI environments before Task 21 begins. CI run 30209979731 proved the previous
platform fixes and exposed four deeper baseline defects:

1. Apple libc++ does not yet implement C++20
   `std::atomic<std::shared_ptr<T>>`.
2. Linux GLFW was built without the Wayland feature required by the HAL.
3. `RVX::Scene` contained a hidden link-time dependency on the Animation-owned
   `SkeletonComponent` implementation.
4. Windows path tests compared equivalent existing paths by spelling instead
   of filesystem identity.

CI run 30420680090 then verified that the compatibility and dependency work
advanced all platforms deeper into the build, and exposed two final closure
defects:

5. `RVX_RHI` owned the enabled-backend dispatcher while every `RHI_*` archive
   depended back on `RVX_RHI`, creating a static-library cycle that GNU and
   Apple linkers rejected.
6. Two resource diagnostic assertions still compared equivalent Windows paths
   by spelling after loader-path assertions had moved to filesystem identity.

CI run 30429579148 verified the backend-factory link closure on Windows, Linux,
and macOS and passed the complete Windows job. It also exposed a platform
capability leak in the architecture baseline:

7. A pure post-process effect-accounting contract initialized the complete
   runtime `PipelineCache`. Linux intentionally reports runtime shader
   compilation as unsupported, while the Apple compiler cannot consume the
   DX12-only fake-device contract. The baseline therefore measured an
   unrelated platform compiler capability instead of the post-process
   accounting contract it named.

CI run 30444227766 then passed the complete Windows Build Truth and Editor
compile-only gate. Linux and macOS exposed the remaining test-infrastructure
coupling rather than new runtime defects:

8. `PipelineCacheValidation` and most render-pass behavior tests constructed
   the platform compiler even though they validate cache, layout, and pass
   orchestration contracts. Linux has no runtime compiler by design, and the
   Apple compiler supports Metal/Vulkan rather than the fake DX12 device.
9. The real GPU-driven shader compilation test inferred compiler availability
   from diagnostic strings instead of a typed capability contract.
10. Two UI tests populated broad font ranges for assertions involving only a
    few glyphs, making atlas capacity depend on platform fonts.
11. Build-time GoogleTest discovery intermittently omitted an already-linked
    validation executable from the architecture inventory on macOS.

CI run 30459643303 built successfully on Linux and proved that the capability
and injection design was sound, while exposing two final validation-fixture
gaps:

12. Four pipeline-cache tests used nonstandard local variable names and
    therefore escaped the mechanical migration to the deterministic compiler
    fixture.
13. The startup-watchdog test released a blocked render initialization as soon
    as the worker announced `Started`, allowing fast schedulers to acknowledge
    startup before the timeout predicate was first evaluated.
14. Shader-importer artifact tests still constructed the platform compiler,
    so an importer behavior contract requested DX12 from the Metal compiler.
15. The lost-wake test armed its blocking wait hook before `Start()` returned;
    the worker could hold the wake mutex while startup itself tried to notify
    the executor.
16. An Actor test declared its lifecycle observer after the entity that
    referenced it, leaving destruction callbacks with a dangling pointer.
17. A custom font atlas carried packed glyph geometry but not the layout source
    that produced it, so rendering mixed custom glyph advances with default
    system-font kerning and baseline metrics.

CI run 30464801873 passed all 1,343 Linux unit/lint tests and exposed one
native-environment integration gap:

18. The native harness allowed GLFW to resolve a Vulkan loader independently
    from the RHI and did not preserve GLFW's failure diagnostic. It now binds
    the linked loader before initialization and reports the exact GLFW error.

CI run 30467297024 confirmed that explicit loader ownership alone was
insufficient:

19. The hosted Linux image exposed multiple mutable Vulkan driver candidates,
    and the loader produced no X11 WSI extension set until the native gate
    selected a concrete software ICD.

CI run 30514612288 then exposed a packaging-boundary assumption before Build
Truth started:

20. The workflow selected Lavapipe by a hard-coded distribution path, but the
    installed package did not expose a manifest at that assumed location. The
    gate must resolve the manifest from the installed package inventory, verify
    the file, and only then export the loader-selection variables.

## Scope

- provide a Render-runtime-local immutable snapshot storage compatibility
  layer;
- make the Linux GLFW Wayland feature and system dependency explicit;
- pin mutable CI runner inputs and hash all vcpkg manifest inputs;
- replace the Scene-to-Animation concrete skinning query with a Scene-owned
  provider interface;
- add a Scene-only link-closure executable and provider integration coverage;
- compare existing test files with `std::filesystem::equivalent`;
- isolate enabled-backend selection in an explicit `RHI_BackendFactory`
  composition module;
- add a backend-factory link-closure executable that requires every enabled
  backend archive without creating a native device;
- make post-process accounting coverage deterministic and independent of a
  runtime shader compiler;
- expose typed shader compiler backend/stage capability queries;
- preserve explicit compiler injection through `ShaderManager` and
  `PipelineCache` composition roots;
- use a deterministic compiler test double for pipeline-cache and render-pass
  behavior contracts while keeping production construction on the real
  platform compiler;
- keep true compiler integration coverage active where the requested target is
  supported and report a typed capability skip elsewhere;
- preserve compiler injection and typed capability checks through the shader
  asset importer;
- build UI font atlases from the exact codepoints required by each test;
- keep packed atlas glyphs and their font layout metrics as one self-contained
  runtime asset;
- bind the native Vulkan lifecycle harness to the linked loader before GLFW
  initialization;
- resolve the installed Lavapipe manifest from package inventory and pin the
  Linux native lifecycle gate to that software ICD;
- defer GoogleTest discovery until test time and evaluate architecture coverage
  from one authoritative CTest inventory snapshot.

## Non-goals

- no RHI command, resource, barrier, or backend behavior changes;
- no Task 21 case-catalog implementation;
- no Scene-to-Animation CMake dependency;
- no weakening of Build Truth or removal of sample targets;
- no broad Core-level shared-pointer abstraction for a single consumer.
- no Linux runtime shader compiler replacement, Apple shader compiler
  redesign in M1.
- no fake compiler in production construction paths and no blanket skipping of
  pipeline-cache or render-pass behavior coverage.

## Ordered execution

1. Implement and validate diagnostics snapshot compatibility.
2. Close the Linux vcpkg/Wayland dependency and CI reproducibility inputs.
3. Introduce `ISkinningPaletteProvider` and remove the concrete Scene query.
4. Add provider integration and Scene-only link-closure tests.
5. Replace the seven path-spelling assertions with semantic equivalence.
6. Run focused validation, fresh Windows Build Truth, push the exact SHA, and
   require Windows/Linux/macOS CI evidence.
7. Move enabled-backend dispatch out of `RVX_RHI`, close the remaining Windows
   path assertions, and repeat exact-SHA Build Truth plus three-platform CI.
8. Decouple the post-process accounting baseline from runtime shader
   compilation, preserve the real runtime-pipeline integration test behind an
   explicit capability declaration, and repeat three-platform CI.
9. Add the typed shader compiler capability contract and composition-root
   injection, migrate pipeline-cache/render-pass behavior tests to deterministic
   artifacts, keep true compiler tests capability-gated, minimize UI atlas
   fixtures, harden test discovery/inventory, and repeat exact-SHA Build Truth
   plus three-platform CI.
10. Close the four pipeline-cache fixture omissions, synchronize watchdog
    release with the timeout lifecycle decision, arm the lost-wake hook only
    after startup, inject the importer compiler, correct observer lifetime,
    bind atlas layout metrics to packed glyphs, stress the timing-sensitive
    contracts, and repeat exact-SHA Build Truth plus three-platform CI.
11. Bind the native lifecycle harness to the linked Vulkan loader before GLFW
    initialization, retain the GLFW error code in RHI failure diagnostics, and
    repeat exact-SHA Build Truth plus three-platform CI.
12. Install and preflight the system Vulkan loader and tools, pin both current
    and legacy Vulkan driver-selection variables to the Lavapipe ICD for the
    Linux native gate, and repeat exact-SHA Build Truth plus three-platform CI.
13. Remove the distribution-path assumption, resolve the Lavapipe manifest from
    `mesa-vulkan-drivers` package inventory, verify it exists, export both
    loader-selection variables, and repeat exact-SHA Build Truth plus
    three-platform CI.

## Exit criteria

- diagnostics publication remains immutable and race-safe;
- Linux links every enabled sample with Wayland GLFW symbols present;
- `RVX::Scene` links without `RVX::Animation`;
- zero or one skinning provider is accepted and multiple providers fail
  deterministically;
- Windows path tests accept only filesystem-equivalent existing files and
  surface comparison errors;
- `RVX_RHI` has no symbol dependency on concrete backends;
- `RVX::RHI_BackendFactory` owns the public factory surface and links every
  enabled backend through a dedicated link-closure gate;
- post-process accounting is validated on every platform without requiring
  runtime shader compilation;
- production shader compilation remains selected by the platform factory;
- pipeline-cache and render-pass behavior suites execute on every platform
  using deterministic artifacts rather than compiler-availability skips;
- true shader compiler integration tests query a typed backend/stage capability
  and skip only when that requested compilation path is unsupported;
- shader importer behavior tests use deterministic artifacts while production
  importers query the real compiler's typed capability;
- font atlas capacity is independent of unrelated glyph ranges;
- custom font atlases use their own kerning, ascent, descent, and line-gap data
  during measurement and rendering;
- the Vulkan native lifecycle harness initializes GLFW with the linked loader;
- the Linux native lifecycle gate resolves the installed Lavapipe manifest from
  package inventory and selects that deterministic software Vulkan ICD;
- test discovery is deferred until the final test environment is available and
  the architecture baseline evaluates one consistent inventory snapshot;
- watchdog validation observes the lifecycle decision rather than depending on
  worker scheduling speed;
- the exact pushed SHA passes all three required CI jobs.
