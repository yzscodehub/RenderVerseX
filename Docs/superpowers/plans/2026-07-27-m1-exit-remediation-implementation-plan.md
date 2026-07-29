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
  backend archive without creating a native device.

## Non-goals

- no RHI command, resource, barrier, or backend behavior changes;
- no Task 21 case-catalog implementation;
- no Scene-to-Animation CMake dependency;
- no weakening of Build Truth or removal of sample targets;
- no broad Core-level shared-pointer abstraction for a single consumer.

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
- the exact pushed SHA passes all three required CI jobs.
