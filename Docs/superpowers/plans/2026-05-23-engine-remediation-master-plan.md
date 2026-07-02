# RenderVerseX Engine Remediation — Master Plan

> **Status:** Tracked historical remediation plan. Phase 0 CI / CTest / preset work has since been implemented and hardened on `engine-remediation`; use current repository files as the exact source for commands and workflow shape. This document remains useful as audit rationale and sequencing context.

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Turn the findings of `Docs/superpowers/specs/2026-05-22-engine-audit-complete.md` into an executable remediation program — starting with a *fully detailed, code-complete Phase 0* that makes regressions visible (CI + CTest + reproducible deps + honest test labels), then decomposing the deeper functional and architectural fixes into focused follow-on plans.

**Architecture:** The audit's core thesis is "骨架强、上层空" (strong skeleton, hollow superstructure), and the reason it stayed hidden is an almost-absent verification system. Therefore Phase 0 (visibility) is sequenced *before* any functional fix — there is no point un-commenting a subsystem until a test can prove it works and a CI gate can stop it regressing. Phases 1–3 are then split one-subsystem-per-plan, matching this repo's existing `Docs/superpowers/plans/` convention.

**Tech Stack:** C++20, CMake 3.21+ presets, vcpkg manifest mode, GitHub Actions, standalone validation executables (`Tests/TestFramework`, exit code 0 = pass), DX12/Vulkan/DX11/OpenGL backends.

---

## Execution Contract

This document was the original source of truth for **Phase 0**. After Phase 0 started landing, the checked-in CMake presets and GitHub workflow became the exact source for build and CI commands. Phases 1–3 remain an index of follow-on plans that must be written (via `superpowers:writing-plans`, and where flagged, preceded by `superpowers:brainstorming`) before execution.

Gates that apply to **every** task below:

- The task's listed validation command passes locally before commit.
- Phase 0 must be fully green (CI runs, `ctest -L unit` passes) before any Phase 1+ functional fix is started — visibility precedes change.
- Each commit is small and scoped to one task (DRY, YAGNI, frequent commits).
- For tasks that change engine behavior (Phase 1+), a review subagent reviews the diff before the next task starts; fix all blocking/high findings and re-run validation.

Build environment note: developer-specific vcpkg paths belong in `CMakeUserPresets.json` or the local `VCPKG_ROOT` environment. Windows / DX verification requires a Windows host with Visual Studio; Linux / WSL should use the Linux presets and should not be treated as proof that DX11/DX12 compile.

Suggested review prompt after each Phase 1+ task:

```text
Review only the changes for this remediation task. Focus on correctness, RHI/lifetime hazards, and whether the new behavior is actually exercised by a test (not just compiled). Report blocking/high issues first with file:line. Do not suggest unrelated refactors.
```

---

## Phase 0 — Make Truth Visible (highest leverage, do first)

Maps to audit findings **B1 / B2 / B3** plus the zero-risk **C-01** documentation close-out. Everything in Phase 0 is platform-agnostic CMake / YAML / comments and is code-complete here.

### File Map (Phase 0)

Modify:
- `CMakeLists.txt` — add `enable_testing()` (Task 0.2).
- `Tests/CMakeLists.txt` — add `add_test()` + `LABELS` for every validation target (Task 0.2).
- `vcpkg.json` — add `builtin-baseline` (Task 0.1).
- `Tests/RHIDX12SourceValidation/main.cpp` — honesty banner (Task 0.3).
- `Tests/RHIDX11SourceValidation/main.cpp` — honesty banner (Task 0.3).
- `Tests/RHIVulkanSourceValidation/main.cpp` — honesty banner (Task 0.3).
- `Core/Include/Core/RefCounted.h:53-63` — ordering-contract comment (Task 0.5).

Create:
- `.github/workflows/ci.yml` — CI pipeline (Task 0.4).

---

### Task 0.1: Lock vcpkg dependency baseline (B3)

**Files:**
- Modify: `vcpkg.json`

Reproducible dependency resolution. Without `builtin-baseline`, vcpkg resolves to whatever the local registry HEAD is, so builds drift across machines/time — which is exactly what lets "it built last month" rot silently.

- [ ] **Step 1: Capture the current baseline commit**

Run (on the Windows host, with `VCPKG_ROOT` set):

```bash
git -C "$VCPKG_ROOT" rev-parse HEAD
```

Expected: a 40-char SHA, e.g. `a1b2c3d4...`. Copy it.

- [ ] **Step 2: Add `builtin-baseline` to the manifest**

Edit `vcpkg.json` — add the field at the top level (after `"description"`):

```json
{
  "name": "renderversex",
  "version": "0.1.0",
  "description": "A cross-platform rendering framework with DX11/DX12/Vulkan/Metal/OpenGL backends",
  "builtin-baseline": "<PASTE-40-CHAR-SHA-FROM-STEP-1>",
  "overrides": [
    {
      "name": "lua",
      "version": "5.4.7"
    }
  ],
```

(Leave `overrides` and `dependencies` unchanged.) Equivalent automated alternative instead of hand-editing: `vcpkg x-update-baseline --add-initial-baseline` run from the repo root.

- [ ] **Step 3: Verify the manifest still resolves**

Run:

```bash
cmake --preset release
```

Expected: configure succeeds; vcpkg log shows it resolving against the pinned baseline (no "using latest" warnings).

- [ ] **Step 4: Commit**

```bash
git add vcpkg.json
git commit -m "build: pin vcpkg builtin-baseline for reproducible dependency resolution"
```

---

### Task 0.2: Register tests with CTest (B1, part 1)

**Files:**
- Modify: `CMakeLists.txt` (enable testing at root)
- Modify: `Tests/CMakeLists.txt` (declare tests + labels)

The repo has 18 validation executables but zero `add_test()` calls, so `ctest` finds nothing and CI can never gate on them. We register each, and label them so headless CI can select the GPU-free subset.

Label scheme:
- `gpu` — creates a real device / swapchain; needs a GPU, skip on headless runners.
- `lint` — string-greps over source (behavioral coverage = none); see Task 0.3.
- `unit` — CPU-only logic; safe on any runner. **This is the CI gate set.**

- [ ] **Step 1: Enable testing at the project root**

Edit `CMakeLists.txt`, in the existing tests block (currently lines ~203–205), add `enable_testing()` before `add_subdirectory(Tests)`:

```cmake
if(RVX_BUILD_TESTS)
    enable_testing()
    add_subdirectory(Tests)
endif()
```

- [ ] **Step 2: Append CTest registrations to `Tests/CMakeLists.txt`**

Add this block at the **end** of `Tests/CMakeLists.txt` (after the shader-copy `foreach`). Each `add_test` is guarded by the same condition that guards its target so disabled backends don't break configure:

```cmake
# =============================================================================
# CTest registration (labels: unit | gpu | lint)
#   unit -> CPU-only, the CI gate set      (ctest -L unit)
#   gpu  -> needs a real device, headless-skip
#   lint -> source-pattern checks, not behavioral (see Task 0.3)
# =============================================================================

# --- unit (CPU-only) ---
add_test(NAME RenderGraphValidation         COMMAND RenderGraphValidation)
add_test(NAME GPUResourceManagerValidation  COMMAND GPUResourceManagerValidation)
add_test(NAME GPUUploadServiceValidation    COMMAND GPUUploadServiceValidation)
add_test(NAME RenderSceneValidation         COMMAND RenderSceneValidation)
add_test(NAME RenderPassBindingValidation   COMMAND RenderPassBindingValidation)
add_test(NAME MaterialSystemValidation      COMMAND MaterialSystemValidation)
add_test(NAME ActorComponentValidation      COMMAND ActorComponentValidation)
add_test(NAME SpatialComponentValidation    COMMAND SpatialComponentValidation)
add_test(NAME ResourceInstantiationValidation COMMAND ResourceInstantiationValidation)
add_test(NAME ResourceViewCacheValidation   COMMAND ResourceViewCacheValidation)
add_test(NAME SystemIntegrationTest         COMMAND SystemIntegrationTest)
set_tests_properties(
    RenderGraphValidation GPUResourceManagerValidation GPUUploadServiceValidation
    RenderSceneValidation RenderPassBindingValidation MaterialSystemValidation
    ActorComponentValidation SpatialComponentValidation ResourceInstantiationValidation
    ResourceViewCacheValidation SystemIntegrationTest
    PROPERTIES LABELS "unit" TIMEOUT 120)

# --- lint (source-pattern checks; see Task 0.3) ---
add_test(NAME RHIDX12SourceValidation   COMMAND RHIDX12SourceValidation)
add_test(NAME RHIDX11SourceValidation   COMMAND RHIDX11SourceValidation)
add_test(NAME RHIVulkanSourceValidation COMMAND RHIVulkanSourceValidation)
set_tests_properties(
    RHIDX12SourceValidation RHIDX11SourceValidation RHIVulkanSourceValidation
    PROPERTIES LABELS "lint" TIMEOUT 60)

# --- gpu (needs a real device; headless CI skips these) ---
if(RVX_ENABLE_DX12)
    add_test(NAME DX12Validation COMMAND DX12Validation)
    set_tests_properties(DX12Validation PROPERTIES LABELS "gpu" TIMEOUT 120)
endif()
if(RVX_ENABLE_VULKAN)
    add_test(NAME VulkanValidation COMMAND VulkanValidation)
    set_tests_properties(VulkanValidation PROPERTIES LABELS "gpu" TIMEOUT 120)
endif()
if(RVX_ENABLE_DX11)
    add_test(NAME DX11Validation COMMAND DX11Validation)
    set_tests_properties(DX11Validation PROPERTIES LABELS "gpu" TIMEOUT 120)
endif()
if(RVX_ENABLE_DX12 AND RVX_ENABLE_VULKAN)
    add_test(NAME CrossBackendValidation COMMAND CrossBackendValidation)
    set_tests_properties(CrossBackendValidation PROPERTIES LABELS "gpu" TIMEOUT 180)
endif()
```

- [ ] **Step 3: Configure and list tests**

Run:

```bash
cmake --preset release
ctest --test-dir build --show-only
```

Expected: CTest lists the registered tests grouped by the labels above (no "No tests were found").

- [ ] **Step 4: Run the unit gate set and confirm pass/fail is reported by exit code**

Run:

```bash
cmake --build --preset release
ctest --test-dir build -L unit --output-on-failure
```

Expected: each `unit` test runs and reports PASS/FAIL via process exit code (the `Tests/TestFramework` `main()` returns `1` on any failed case — confirmed in `Tests/RHIDX12SourceValidation/main.cpp:98-104`). **If a test labeled `unit` actually needs a device and crashes/fails headless, move it to the `gpu` group in Step 2 and re-run** — this reclassification is expected on the first pass.

- [ ] **Step 5: Commit**

```bash
git add CMakeLists.txt Tests/CMakeLists.txt
git commit -m "test: register validation executables with CTest and label by unit/gpu/lint"
```

---

### Task 0.3: Make source-grep tests honest (B2)

**Files:**
- Modify: `Tests/RHIDX12SourceValidation/main.cpp`
- Modify: `Tests/RHIDX11SourceValidation/main.cpp`
- Modify: `Tests/RHIVulkanSourceValidation/main.cpp`

These "validation" suites call `source.find("...")` over the backend `.cpp` text (see `Tests/RHIDX12SourceValidation/main.cpp:24-79`). They prove a *string is present*, not that behavior is correct, and they go stale silently on any refactor that keeps the behavior but renames a symbol. Phase 0 does not rewrite them into behavioral tests (that needs GPU CI and is tracked as a Phase 2 follow-on) — it stops them from masquerading as behavioral coverage. The `lint` label from Task 0.2 already separates them; this task adds an in-file banner so a reader is never misled.

- [ ] **Step 1: Add an honesty banner to each of the three files**

Insert this comment block immediately below the existing `#include` lines (above `using namespace RVX::Test;`) in **each** of the three `main.cpp` files:

```cpp
// =============================================================================
// SOURCE-PATTERN LINT — NOT A BEHAVIORAL TEST.
// These checks grep the backend .cpp text for expected substrings. They prove
// a pattern is *present in source*, not that the runtime behavior is correct,
// and they go stale silently when a refactor preserves behavior but renames a
// symbol. CTest label: "lint" (see Tests/CMakeLists.txt). Converting these into
// real device/behavioral tests is tracked as a Phase 2 follow-on plan.
// =============================================================================
```

- [ ] **Step 2: Build and run the lint set**

Run:

```bash
cmake --build --preset release
ctest --test-dir build -L lint --output-on-failure
```

Expected: the three lint suites still build and pass; the banner is the only change (no behavior change).

- [ ] **Step 3: Commit**

```bash
git add Tests/RHIDX12SourceValidation/main.cpp Tests/RHIDX11SourceValidation/main.cpp Tests/RHIVulkanSourceValidation/main.cpp
git commit -m "test: label source-grep validations as lint, not behavioral coverage"
```

---

### Task 0.4: GitHub Actions CI (B1, part 2)

**Files:**
- Create: `.github/workflows/ci.yml`

At the time this plan was written there was no `.github/` directory, so nothing gated merges. The intended remediation was to add Windows and Linux CI jobs running the GPU-free `unit` + `lint` sets via CTest. GPU-labeled tests are intentionally not run on headless runners.

- [ ] **Step 1: Create the workflow**

Create `.github/workflows/ci.yml`:

```yaml
name: CI

on:
  push:
    branches: [master]
  pull_request:
    branches: [master]

concurrency:
  group: ci-${{ github.ref }}
  cancel-in-progress: true

jobs:
  windows:
    name: Windows (DX11/DX12/Vulkan)
    runs-on: windows-latest
    env:
      VCPKG_ROOT: C:/vcpkg
      VCPKG_DEFAULT_BINARY_CACHE: ${{ github.workspace }}/.vcpkg-cache
    steps:
      - uses: actions/checkout@v4

      - name: Prepare vcpkg binary cache dir
        run: mkdir -p ${{ github.workspace }}/.vcpkg-cache
        shell: bash

      - name: Cache vcpkg installed packages
        uses: actions/cache@v4
        with:
          path: ${{ github.workspace }}/.vcpkg-cache
          key: vcpkg-windows-${{ hashFiles('vcpkg.json') }}
          restore-keys: vcpkg-windows-

      - name: Configure (release preset)
        run: cmake --preset release

      - name: Build
        run: cmake --build --preset release

      - name: Test (unit + lint, GPU tests skipped on headless runner)
        run: ctest --test-dir build -L "unit|lint" --output-on-failure

  linux:
    name: Linux (Vulkan/OpenGL configure)
    runs-on: ubuntu-latest
    env:
      VCPKG_ROOT: /usr/local/share/vcpkg
      VCPKG_DEFAULT_BINARY_CACHE: ${{ github.workspace }}/.vcpkg-cache
    steps:
      - uses: actions/checkout@v4

      - name: Install system deps for vcpkg ports
        run: |
          sudo apt-get update
          sudo apt-get install -y ninja-build pkg-config libx11-dev libxrandr-dev \
            libxinerama-dev libxcursor-dev libxi-dev libgl1-mesa-dev autoconf libtool

      - name: Prepare vcpkg binary cache dir
        run: mkdir -p ${{ github.workspace }}/.vcpkg-cache

      - name: Cache vcpkg installed packages
        uses: actions/cache@v4
        with:
          path: ${{ github.workspace }}/.vcpkg-cache
          key: vcpkg-linux-${{ hashFiles('vcpkg.json') }}
          restore-keys: vcpkg-linux-

      - name: Configure
        run: cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake

      - name: Build
        run: cmake --build build -j

      - name: Test (unit + lint)
        run: ctest --test-dir build -L "unit|lint" --output-on-failure
```

- [ ] **Step 2: Validate workflow syntax locally (optional but recommended)**

If `actionlint` is available:

```bash
actionlint .github/workflows/ci.yml
```

Expected: no syntax errors. (If the tool is not installed, skip — GitHub validates on push.)

- [ ] **Step 3: Commit and push to a branch, open a draft PR to observe the first run**

```bash
git add .github/workflows/ci.yml
git commit -m "ci: add GitHub Actions pipeline building + running unit/lint tests"
```

> First-run expectation: vcpkg install of the full dependency set (jolt, recast, dxc, imgui, …) is slow and the first build commonly needs iteration (a missing system lib on Linux, a preset/SDK detail on Windows). Treat the first red run as the start of Phase 0 hardening, not a failure of the task — fix forward until both jobs are green, *then* mark the task done.

---

### Task 0.5: Document the `RefCounted` memory-order contract (close C-01 false positive)

**Files:**
- Modify: `Core/Include/Core/RefCounted.h:53-63`

The audit's re-verification downgraded C-01: `AddRef` uses `relaxed` and `Release` uses `acq_rel`, which is the canonical lock-free intrusive-refcount idiom (libc++/Boost `intrusive_ptr`), not a use-after-free. This is zero-risk documentation so the next reader (or audit) does not re-flag it.

- [ ] **Step 1: Add the contract comment above the two methods**

In `Core/Include/Core/RefCounted.h`, replace the `// Reference counting` line (line 53) so the block reads:

```cpp
        // Reference counting.
        // Memory-order contract (canonical lock-free intrusive refcount):
        //  - AddRef uses relaxed: a caller incrementing the count must already
        //    hold a live reference, so no ordering w.r.t. other data is needed.
        //  - Release uses acq_rel: the acquire pairs with prior releases so the
        //    thread that observes the count hit zero sees all writes made by
        //    other owners before destroying the object (no UAF).
        // See libc++/Boost intrusive_ptr for the same pattern.
        void AddRef() const
        {
            m_refCount.fetch_add(1, std::memory_order_relaxed);
        }

        // Returns true if reference count reached zero (object should be deleted)
        bool Release() const
        {
            return m_refCount.fetch_sub(1, std::memory_order_acq_rel) == 1;
        }
```

- [ ] **Step 2: Verify it still compiles**

Run (Core is header-only here, so building any consumer suffices):

```bash
cmake --build --preset release --target RenderGraphValidation
```

Expected: builds (comment-only change).

- [ ] **Step 3: Commit**

```bash
git add Core/Include/Core/RefCounted.h
git commit -m "docs: document RefCounted memory-order contract (closes C-01 false positive)"
```

---

## Phases 1–3 — Follow-on Plan Index

These are **not** bite-sized tasks here. Per `superpowers:writing-plans` scope rules and this repo's one-plan-per-subsystem convention, each item below becomes its own focused plan (file name suggested), written only after Phase 0 is green so the new behavior lands behind a test. Items flagged **[brainstorm-first]** are large/ambiguous enough to require `superpowers:brainstorming` before a plan is written. Every item carries the verified file:line anchors and the *real* gotchas found while preparing this plan, so the sub-plan author starts from ground truth rather than the audit's optimistic estimate.

### Phase 1 — Immediate functional fixes (days)

**A2 — Prefab component-class registration** → `…-prefab-component-class-registration.md`
- Anchors: `ComponentFactory::CreateComponentByClassName` (`Scene/Private/ComponentClassFactory.cpp:24`) is called by production code at `Scene/Private/Prefab.cpp:956,982,1027`, but `RegisterComponentClass` (`ComponentClassFactory.cpp:14`; template form `Scene/Include/Scene/ComponentFactory.h:168`) is invoked **only** in `Tests/`.
- Gotchas: ~15 subclasses must be registered (`LightComponent`, `CameraComponent`, `AudioComponent`, `AnimatorComponent`, `ColliderComponent`, `DecalComponent`, `ReflectionProbeComponent`, `RigidBodyComponent`, `SkeletonComponent`, `MeshRendererComponent`, `SkyboxComponent`, `LODComponent`, `LightProbeComponent`, `SceneComponent`), and **`ParticleComponent` lives in the Particle module** (`Particle/Include/Particle/ParticleComponent.h:19`), which depends on Scene — a single central `RegisterDefaults`-style call inside Scene cannot see it. **Design decision required:** central registry populated by Engine (sees all modules) vs. per-module self-registration. Do not conflate with the *other* `ComponentFactory::RegisterDefaults` (`ComponentFactory.cpp:42`), which is a separate model-import "StaticMesh" map.
- Acceptance: new test instantiates a prefab containing each registered component type with **no** test-side registration; asserts non-null + correct type. Wire into `ctest -L unit`.

**A3 — Particle simulation wiring** → `…-particle-simulation-wiring.md`
- Anchors: commented at `Particle/Private/ParticleSubsystem.cpp:137,143` and `ParticleSystemInstance.cpp:154`.
- Gotcha 1: there is **no `SetSimulator` method** on `ParticleSystemInstance` (`ParticleSystemInstance.h` has only `GetSimulator()` at :139 and `m_simulator` at :241) — it must be added.
- Gotcha 2: the commented `m_simulator->Simulate(effectiveDeltaTime, params)` references an undefined `params`; the real signature is `Simulate(float, const SimulateParams&)` (`IParticleSimulator.h:65`) where `SimulateParams = { SimulationGPUData simulationData; float deltaTime; float totalTime; }` (`IParticleSimulator.h:26`) and must be constructed/populated.
- Gotcha 3: the GPU path's simulate-pipeline binding is itself commented (`GPUParticleSimulator.cpp:158` per audit) — start with the CPU simulator path; GPU path is a separate slice.
- Acceptance: new test — CPU simulator advances `GetAliveCount()` after `Emit` + N `Simulate` steps. `ctest -L unit`.

**A4 / A5 — RenderGraph aliasing barriers & async compute** → `…-rendergraph-aliasing-and-async.md`
- Anchors: A5 aliasing barriers computed but not emitted — `Render/Private/Graph/RenderGraphExecutor.cpp:19-27` and `RenderGraphCompiler.cpp:821`. A4 `ExecuteRenderGraphAsync` ignores the compute queue/fence and silently falls back to the sync path — `RenderGraphExecutor.cpp:137-151`.
- Decision: A4 should either truly submit to the compute queue or be renamed + explicitly marked unsupported (current API is misleading). A5 minimum-viable: assert when an aliased resource's first use is a non-`Undefined` state before doing full barrier emission.
- Acceptance: extend `RenderGraphValidation` (already CPU-only `unit`) with an aliasing-barrier emission assertion and an async-path contract test.

### Phase 2 — Foundation & correctness (weeks)

**A6 — JobSystem work-stealing rewrite** → `…-jobsystem-work-stealing.md` **[brainstorm-first]**
- Single global-lock `priority_queue` pool, no work-stealing (`Core/.../Job/ThreadPool.h:174`); `JobGraph::Wait()` spins and re-scans the ready set O(N) (`JobGraph.cpp:132-169`); `WaitAll()` from a worker thread deadlocks (C-02). Engine-wide throughput ceiling touching every parallel subsystem — design brainstorm (per-worker deque + atomic dependency counts; enkiTS / UE-Tasks style) before planning. Acceptance: throughput microbench + no-deadlock test under `ctest`.

**A1 — Physics wiring** → `…-physics-wiring.md`
- Jolt step commented (`Physics/Private/PhysicsWorld.cpp:70`); bodies never registered into the physics world ("deferred until physics world is available", `Scene/Private/Components/RigidBodyComponent.cpp:326-327`); CCD/constraints/masks all TODO (`:207,220,260,275`). End-to-end non-functional. Acceptance: falling-body integration test asserts position changes after stepping.

**Script sandbox enforcement** → `…-script-sandbox-enforcement.md`
- `instructionLimit/memoryLimit` declared but no `lua_sethook(LUA_MASKCOUNT)` / custom allocator enforces them; `GetRawState()` is an escape hatch (audit §4.6). Acceptance: a runaway script is interrupted at the instruction limit.

**Resource async hardening** → `…-resource-async-hardening.md`
- `WaitForLoad()` busy-waits ("Busy wait for now"); `Load<T>` has no type constraint; check-load-cache has a TOCTOU window (`Resource/Private/ResourceManager.cpp`). R-01 already resolved in current tree. Acceptance: CV-based wait + `static_assert` + merged critical section, with a concurrency test.

**RHI backend safety** → `…-rhi-backend-safety.md`
- Descriptor-heap bounds check (D12-04); D3D12MA failure should abort not WARN-continue (D12-05); explicit `aspectMask` (VK-02); `VK_KHR_synchronization2` presence check + fallback (VK-04); deferred-semaphore fence guard (VK-01). GPU-labeled tests / validation-layer-clean runs.

**Source tests → behavioral** → `…-source-tests-to-behavioral.md`
- Convert the three `*SourceValidation` lint suites (Task 0.3) into real device/behavioral tests; requires GPU CI runners or a software adapter (WARP for DX12, lavapipe for Vulkan). Acceptance: behavioral tests replace the `lint` label with `gpu`/`unit`.

**Prefab lifecycle fixes** → `…-prefab-lifecycle-fixes.md`
- Ordinal implicit-skip data loss (W-05); `AssignComponentName` vs `SetName` (W-06, partial); unnamed-component binding tracking (W-08). Acceptance: prefab round-trip tests.

### Phase 3 — Architectural evolution (months) — all **[brainstorm-first]**

**Actor/Component dual-track cleanup** → `…-actor-component-dual-track-cleanup.md`
- Finish the Actor/legacy-`Component` migration (dual storage/transform, compile-time-branched `AddComponent`); unify the two spatial indices (`SceneManager::m_spatialIndex` vs `SpatialSubsystem::m_index`); delete left-handed legacy `Core/Math.h` that conflicts with the glm right-handed path.

**RHI shared base + bindless** → `…-rhi-shared-base-and-bindless.md`
- Introduce a shared `RHICommandContextBase` + unified `RHIResourceState`→native enum mapping (~5 duplicated copies / 215 cases); add a bindless / descriptor-array binding type (blocks GPU-driven rendering).

**Render GPU-driven path** → `…-render-gpu-driven-path.md`
- Wire up the dead `GPUCulling` / `MeshletRenderer`; convert `OpaquePass` to instanced/indirect + a bindless material table; delete the legacy `ExecutePasses` path.

**Editor ↔ Engine integration** → `…-editor-engine-integration.md`
- `EditorApplication.cpp` is a standalone GLFW+GL+ImGui shell with zero `Engine`/`GetSubsystem`/`RenderSubsystem` references and a TODO `InitializeRHI()`; recompose it onto the engine via the RHI. Also register `DebugSubsystem` (never `AddSubsystem`'d) and give `StatsHUD`/Console a real render path.

**Networking security** → `…-networking-security.md`
- Add handshake/auth/sequence validation + server-authoritative reconciliation; the current `ClientToServer` path trusts the client and "delta" is a fake delta. No encryption today.

**Particle / Terrain / Water completion** → `…-particle-terrain-water-completion.md`
- GPU particle count-driven indirect dispatch; implement Terrain/Water render passes through RenderGraph (`Setup` currently declares no resources, `Execute` draws nothing — `TerrainRenderer.cpp:47` / `WaterRenderer.cpp:49`) instead of bypassing the graph.

---

## Self-Review (writing-plans checklist)

- **Spec coverage:** Phase 0 fully covers audit B1 (Tasks 0.2 + 0.4), B2 (Task 0.2 labels + Task 0.3 banner; behavioral conversion deferred to a named Phase 2 follow-on), B3 (Task 0.1), and the C-01 close-out (Task 0.5). All A-class and architectural findings (A1–A6, themes 1–4) appear in the Phase 1–3 index with anchors. No audit P0 is dropped.
- **Placeholder scan:** Phase 0 tasks contain literal CMake/YAML/C++ and exact commands with expected output — no "TBD"/"add error handling"/"write tests for the above". The single intentional fill-in is the vcpkg SHA in Task 0.1, captured by the command in its Step 1 (not a placeholder for unknown content). Phases 1–3 are deliberately an *index of plans to write*, not bite-sized tasks, so the no-fake-code rule is honored rather than bluffed.
- **Type/identifier consistency:** label names (`unit`/`gpu`/`lint`) are used identically across Tasks 0.2/0.3/0.4; target names match `Tests/CMakeLists.txt`; `SimulateParams`/`SetSimulator`/`GetAliveCount` references match the verified headers.

## Execution Handoff

Phase 0 is ready to execute task-by-task. Two options:

1. **Subagent-Driven (recommended)** — dispatch a fresh subagent per Phase 0 task, review between tasks. Use `superpowers:subagent-driven-development`.
2. **Inline Execution** — execute Phase 0 here with checkpoints. Use `superpowers:executing-plans`.

Phases 1–3: write each linked sub-plan with `superpowers:writing-plans` (and `superpowers:brainstorming` first for the flagged items) once Phase 0 is green.
