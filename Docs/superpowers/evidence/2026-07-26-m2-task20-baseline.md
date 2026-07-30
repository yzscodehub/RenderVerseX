# M2 Task 20 Baseline and Entry Evidence

**Status:** Complete; M1 entry gate satisfied and M2 red baseline locked
**Candidate source commit:** `6c93300618026ca1068acebe61d399669bf5c8e0`
**Branch:** `codex/architecture-implementation`
**Observed remote branch head:** `6c93300618026ca1068acebe61d399669bf5c8e0`
**Evidence date:** 2026-07-30
**Platforms:** Windows x64 local; GitHub Windows, Linux, and macOS ARM64

## Purpose

Lock the M1-to-M2 boundary, the first reproducible DX12 red integration case,
and the required/optional capability matrix before changing production RHI
contracts.

This record is fail-closed:

- local execution does not substitute for required cross-platform CI;
- compile-only does not count as real-backend support;
- the expected-red GPU-driven case does not make the M1 gate red because it is
  outside the M1 lifecycle scope;
- volatile native handles, absolute machine paths, and full validation logs
  remain build artifacts rather than checked-in evidence.

## M1 Entry Gate

### Source and remote identity

- Local candidate:
  `6c93300618026ca1068acebe61d399669bf5c8e0`
- Remote branch:
  `6c93300618026ca1068acebe61d399669bf5c8e0`
- Ahead/behind after push: `0/0`
- CI:
  `https://github.com/yzscodehub/RenderVerseX/actions/runs/30521739421`
- Result: **PASS**

### Local Windows Build Truth

The exact candidate completed the full Windows Build Truth after a clean build
tree was configured and built. One vcpkg `applocal.ps1` post-build invocation
failed transiently after its target had linked; the same invocation succeeded
when replayed, the complete no-change build returned zero, and the final
Build Truth run passed.

Local evidence:

- source commit:
  `6c93300618026ca1068acebe61d399669bf5c8e0`;
- CTest inventory: 1,490;
- unit/lint preset: 1,343/1,343 passed;
- M1 architecture gate: `Architecture.M1ArchitectureCut`;
- required native backend: DX12;
- adapter: `NVIDIA GeForce RTX 4070 Ti`;
- native lifecycle: present sequence 2, one accepted resize, orderly shutdown;
- branch/working/staged diff hygiene: passed.

The authoritative fresh-environment Windows result is CI #29:

- `Build truth (Windows Debug)`: passed;
- `Configure Editor compile-only`: passed;
- `Build Editor compile-only`: passed;
- build-truth evidence upload: passed.

The Editor steps are compatibility compilation only and do not extend the M2
Editor scope.

### Cross-platform evidence

CI #29 completed successfully on the exact candidate:

- Windows DX11/DX12/Vulkan Fresh Build Truth: passed;
- Linux Lavapipe ICD discovery and Vulkan preflight: passed;
- Linux Vulkan/OpenGL Build Truth: passed;
- Linux ThreadSanitizer configure, stress-target build, and stress run: passed;
- macOS ARM64 Metal Build Truth: passed;
- every required evidence upload: passed.

No backend was silently substituted and no unavailable environment was
classified as passed.

## M2 Red Integration Case

Required case ID: `ModelViewerGPUDrivenSmoke.DX12`

The bounded test definition requests and realizes DX12, enables native
validation, runs eight frames at 320x180, and requires GPU-driven culling to be
ready.

Command:

```powershell
ctest --test-dir build\win_x64_debug -C Debug `
  -R "^ModelViewerGPUDrivenSmoke$" `
  --output-on-failure `
  --output-log build\win_x64_debug\BuildTruth\M2Task20ModelViewerGPUDrivenSmoke.log
```

Observed result on the exact candidate:

- process/test outcome: failed as required for the red baseline;
- requested/realized backend: DX12;
- adapter: `NVIDIA GeForce RTX 4070 Ti`;
- frames/resolution: 8 at 320x180;
- RHI error lines: 262;
- graphics PSO creation failures: 8;
- `GPUDrivenOpaquePipeline` backend failure records: 8;
- uninitialized descriptor-range messages: 19;
- `DATA_STATIC` violation messages: 80;
- before-state mismatch messages: 69;
- optimized-clear mismatch messages: 8;
- missing `BLENDINDICES0` messages: 8;
- missing `BLENDWEIGHT0` messages: 8.

Final GPU-driven state:

```text
enabled=true
graphPassAdded=true
graphPassRecorded=true
gpuExecutionRecorded=true
graphInputDrawItemCount=2
opaqueIndirectRequested=true
opaqueIndirectEligible=false
opaqueIndirectBatches=0
opaqueIndirectDraws=0
visibleCullableDrawItemCount=1
frustumCulledDrawItemCount=0
distanceCulledDrawItemCount=1
skippedMissingGpuDataCount=0
```

These categories remain the ordered M2 contract work:

1. shader-interface and graphics-pipeline preflight;
2. descriptor completeness and referenced-data volatility;
3. scoped dependencies and lifetime state handoff;
4. optional optimized-clear compatibility;
5. DX12 native closure followed by Vulkan and Metal closure.

### Disabled control

Command:

```powershell
ctest --test-dir build\win_x64_debug -C Debug `
  -R "^ModelViewerGPUDrivenDisabledSmoke$" `
  --output-on-failure `
  --output-log build\win_x64_debug\BuildTruth\M2Task20ModelViewerGPUDrivenDisabledSmoke.log
```

Result: **PASS**, proving the existing non-GPU-driven control remains usable.

## Frozen M2 Capability Matrix

### Required Tier 1 base

| Contract | DX12 | Vulkan | Metal |
|---|---:|---:|---:|
| Concrete backend/device/adapter identity | Required | Required | Required |
| Buffer/texture creation and legal views | Required | Required | Required |
| Upload, copy, readback, mapped memory | Required | Required | Required |
| Complete descriptor/binding snapshots | Required | Required | Required |
| Graphics and compute pipeline creation | Required | Required | Required |
| Vertex-input and attachment preflight | Required | Required | Required |
| RT/depth/UAV/SRV/copy/indirect dependencies | Required | Required | Required |
| Same-layout/state memory dependency | Required | Required | Required |
| Physical-domain queue/fence progress | Required | Required | Required |
| Presentation acquire/render/present/resize/idle | Required | Required | Required |
| Deferred destruction after GPU completion | Required | Required | Required |
| Native validation message collection | Required | Required | Required |

Timestamp/query behavior is required only when the backend reports the
corresponding base capability as supported. Capability reporting and native
feature enablement must agree.

### Optional capability cases

- ray tracing;
- mesh shaders;
- bindless/descriptor indexing;
- partially bound or null descriptors;
- sparse resources;
- multiple native queues per logical queue class;
- untracked Metal hazard mode;
- DX12 enhanced-barrier optimization;
- independent async-compute performance scheduling.

An optional case may be `Unsupported` only when the device report says the
same. It may not silently emulate or switch backends.

### Compatibility backends

DX11 and OpenGL are compile/smoke compatibility targets for M2. They translate
the shared contract conservatively and explicitly report unsupported Tier 1
cases. Their result cannot satisfy a DX12/Vulkan/Metal production-support
claim.

## Task 20 Exit Checklist

- [x] Exact local candidate identified.
- [x] Exact candidate pushed with explicit authorization.
- [x] Remote branch head equals the candidate.
- [x] Windows Fresh Build Truth observed on the exact candidate.
- [x] Linux Vulkan lifecycle and TSAN evidence observed.
- [x] macOS Metal lifecycle evidence observed.
- [x] Bounded DX12 GPU-driven red artifact recorded.
- [x] Disabled/direct-draw path retained as the fallback control.
- [x] M2 required/optional capability matrix frozen.

Task 21 production work is unblocked. Its first implementation slice is CH1:
freeze shared case IDs and the versioned deterministic report schema.
