# R1a RHI Submit/Fence Contract Implementation Plan

**Date:** 2026-05-31
**Source plan:** `Docs/superpowers/specs/2026-05-30-render-program-plan-v1.md`
**Source section:** `6. R1 - RHI Core Contract`
**Parent R-SP:** R1 RHI Core Contract
**Sub-stage:** R1a - Submit/Fence Contract
**Mode:** Strict serial execution with Spark plan review before code changes.

---

## 1. Goal

Stabilize the RHI submit/fence contract before deeper render, upload, RenderGraph, and material work depends on it.

R1a focuses only on command submission and fence semantics. It must make these outcomes explicit and testable:

- Fence signal mode is explicit: host signal, explicit queue signal, and backend default-queue signal are not conflated.
- `SubmitCommandContext(s)` with a signal fence returns the value that was actually submitted.
- Consecutive submissions to the same fence produce monotonic, unique signal values even before the GPU completes prior work.
- Backends expose whether host signal, default-queue signal, explicit queue signal, queue wait, and multi-queue batch submission are real or emulated.
- Existing command-context fence wait/signal APIs cannot silently imply cross-queue GPU synchronization when the backend does not implement it.

---

## 2. R1 Decomposition

R1 is XL and will be executed as sub-stages:

- **R1a - Submit/Fence Contract:** current sub-stage.
- **R1b - Barrier and Descriptor Base Contract:** validate null/error paths, split barrier capability honesty, descriptor layout/set creation contracts.
- **R1c - Query/Timestamp Capability Parity:** either implement query pools where feasible or keep capability-disabled with backend tests and profiler behavior.
- **R1d - RHI Capability Matrix Closure:** consolidate capability matrix, cross-backend validation, and remaining R1 phase-log closure.

Each sub-stage gets its own plan, Spark plan review, implementation, Spark code review, phase-log entry, and commit.

---

## 3. Current Code Findings

Files inspected:

- `RHI/Include/RHI/RHIDevice.h`
- `RHI/Include/RHI/RHICommandContext.h`
- `RHI/Include/RHI/RHISynchronization.h`
- `RHI/Include/RHI/RHICapabilities.h`
- `RHI_DX12/Private/DX12Device.cpp`
- `RHI_DX12/Private/DX12CommandContext.cpp`
- `RHI_DX12/Private/DX12Resources.cpp`
- `RHI_Vulkan/Private/VulkanDevice.cpp`
- `RHI_Vulkan/Private/VulkanCommandContext.cpp`
- `RHI_Vulkan/Private/VulkanResources.cpp`
- `RHI_DX11/Private/DX11Device.cpp`
- `RHI_DX11/Private/DX11CommandContext.cpp`
- `RHI_OpenGL/Private/OpenGLDevice.cpp`
- `RHI_OpenGL/Private/OpenGLCommandContext.cpp`
- `Tests/DX12Validation/main.cpp`
- `Tests/VulkanValidation/main.cpp`
- `Tests/DX11Validation/main.cpp`
- `Tests/CrossBackendValidation/main.cpp`

Observed issues:

- `IRHIDevice::SubmitCommandContext()` and `SubmitCommandContexts()` return `void`, so callers cannot know the fence value they should wait for.
- DX12 submit currently signals `completed + 1`; consecutive submissions before completion can target the same value.
- DX11 and OpenGL submit also emulate signal as `completed + 1`, with the same non-monotonic contract risk.
- Vulkan already has `VulkanFence::AllocateSignalValue()`, but the value is hidden from the caller.
- `RHIFence::Signal()` is documented as CPU signal, but DX12 implementation currently signals on the graphics queue while Vulkan uses host `vkSignalSemaphore`; the plan must not call those equivalent.
- `RHICommandContext::WaitFence()` is documented as GPU command/queue wait, but DX12 and Vulkan currently perform CPU waits, and DX11/OpenGL treat it as no-op.
- DX12 `SubmitDX12CommandContexts()` currently accepts only same-queue batches, so it must not report general multi-queue batch submit support until that is implemented and tested.
- R-HS added query capability honesty; R1a should not expand into query implementation.

---

## 4. Approved Scope

### R1a.1 - Public submit/fence API contract

Files/modules:

- `RHI/Include/RHI/RHIDevice.h`
- `RHI/Include/RHI/RHISynchronization.h`
- `RHI/Include/RHI/RHICommandContext.h`
- Backend device headers and implementations, including Metal signature-only updates
- Any local test doubles such as `NullDevice` in tests touched by signature changes

Tasks:

- Change `IRHIDevice::SubmitCommandContext()` and `SubmitCommandContexts()` to return `uint64`.
- Return `0` when no signal fence is provided or when submission fails before a fence is queued.
- Document that a non-zero return value is the fence value submitted to the backend and is safe to pass to `WaitForFence()` / `RHIFence::Wait()`.
- Clarify `RHIFence::Signal()` as backend default signal, not necessarily host-only. Backends must report its semantics through capabilities.
- Clarify `RHIFence::SignalOnQueue()` as explicit GPU queue signal when `supportsExplicitQueueFenceSignal=true`; otherwise it is unsupported or emulated.
- Clarify `RHICommandContext::WaitFence()` and `SignalFence()` as legacy/advanced queue-sync APIs whose support must be checked through capabilities.
- Update all backend implementations and local fake devices to the new return signature in the same sub-stage:
  - `RHI_DX12`
  - `RHI_Vulkan`
  - `RHI_DX11`
  - `RHI_OpenGL`
  - `RHI_Metal`
  - `Tests/RenderHonestyValidation/main.cpp`
  - `Tests/GPUUploadServiceValidation/main.cpp`
  - `Tests/GPUResourceManagerValidation/main.cpp`
  - `Tests/ResourceViewCacheValidation/main.cpp`

### R1a.2 - Capability fields for synchronization honesty

Files/modules:

- `RHI/Include/RHI/RHICapabilities.h`
- Backend device capability initialization files

Tasks:

- Add sync capability fields:
  - `supportsHostFenceSignal`
  - `supportsDefaultQueueFenceSignal`
  - `supportsExplicitQueueFenceSignal`
  - `supportsQueueFenceWait`
  - `supportsMultiQueueBatchSubmit`
  - `emulatesQueueFences`
- Add a short comment to each field describing whether it refers to host signal, default backend signal, explicit queue signal, or batching multiple queues in one `SubmitCommandContexts()` call.
- Set them per backend:
  - DX12: host signal false until implemented; default-queue signal true; explicit queue signal true; queue wait true if implemented through queue wait; multi-queue batch submit false because current implementation rejects mixed queue contexts; emulated false.
  - Vulkan: host signal true; default-queue signal true through submit; explicit queue signal true if implemented through queue submit; queue wait true if implemented safely in this sub-stage, otherwise false with an explicit unsupported path; multi-queue batch submit true only if the existing multi-queue submit path is kept honest and tests cover it; emulated false.
  - DX11: host signal false unless a real host-set fence path is implemented; default-queue signal emulated; explicit queue signal false; queue wait false; multi-queue batch submit false; emulated true.
  - OpenGL: host signal false because `glFenceSync` is a GPU sync point, not a host-set fence value; default-queue signal emulated; explicit queue signal false; queue wait false; multi-queue batch submit false; emulated true.
  - Metal: signature/capability initialization must compile if the backend is enabled; deeper Metal behavior may remain capability-disabled if not validated locally.

### R1a.3 - Backend submit value implementation

Files/modules:

- `RHI_DX12/Private/DX12Resources.h`
- `RHI_DX12/Private/DX12Resources.cpp`
- `RHI_DX12/Private/DX12CommandContext.cpp`
- `RHI_DX12/Private/DX12Device.cpp`
- `RHI_Vulkan/Private/VulkanResources.h`
- `RHI_Vulkan/Private/VulkanResources.cpp`
- `RHI_Vulkan/Private/VulkanCommandContext.cpp`
- `RHI_Vulkan/Private/VulkanDevice.cpp`
- `RHI_DX11/Private/DX11Resources.h`
- `RHI_DX11/Private/DX11Resources.cpp`
- `RHI_DX11/Private/DX11Device.cpp`
- `RHI_OpenGL/Private/OpenGLSync.h`
- `RHI_OpenGL/Private/OpenGLSync.cpp`
- `RHI_OpenGL/Private/OpenGLDevice.cpp`

Tasks:

- Add or expose backend-local fence value allocation for DX12, DX11, and OpenGL matching Vulkan's existing `AllocateSignalValue()` approach.
- Ensure repeated `SubmitCommandContext()` calls against the same fence return unique increasing values before waiting.
- Make DX12 `RHIFence::Signal()` use host fence signal semantics rather than queue signal semantics.
- If true host signaling cannot be implemented safely for DX12 in this sub-stage, keep `RHIFence::Signal()` as default-queue signal, rename/comment the semantics honestly, set `supportsHostFenceSignal=false`, and add a follow-up for host signal parity.
- Ensure Vulkan tracks explicit host signal values through its existing next-signal counter.
- For DX11/OpenGL, keep queue submission fence behavior explicitly emulated and capability-visible.
- Keep old call sites source-compatible: callers may ignore the returned value.

### R1a.4 - Command-context fence wait/signal honesty

Files/modules:

- `RHI_DX12/Private/DX12CommandContext.cpp`
- `RHI_Vulkan/Private/VulkanCommandContext.cpp`
- `RHI_DX11/Private/DX11CommandContext.cpp`
- `RHI_OpenGL/Private/OpenGLCommandContext.cpp`

Tasks:

- DX12: make command-context `WaitFence()` use queue wait semantics through the context queue when possible, not CPU wait.
- DX12: keep `SignalFence()` as queue signal and return/log visible errors for invalid queue/fence.
- Vulkan: implement true queue wait/signal only if it can be done safely in this sub-stage; otherwise set `supportsQueueFenceWait=false` and make the command-context wait path visibly unsupported instead of CPU blocking.
- DX11/OpenGL: keep no-op/emulated behavior capability-visible and log unsupported for queue wait/signal.
- Capability fields must match whichever path is actually implemented. No backend may report queue wait/signal support for a CPU wait/no-op path.

### R1a.5 - Tests

Files/modules:

- `Tests/DX12Validation/main.cpp`
- `Tests/VulkanValidation/main.cpp`
- `Tests/DX11Validation/main.cpp`
- `Tests/CrossBackendValidation/main.cpp`
- `Tests/RenderHonestyValidation/main.cpp` if a pure capability check fits better there

Tasks:

- Add backend tests proving submit returns a non-zero fence value when a signal fence is provided.
- Add repeated-submit tests proving returned values are strictly increasing before waiting.
- Add tests that `WaitForFence(fence, returnedValue)` completes for DX12, Vulkan, and DX11 where hardware backend is available.
- Add capability assertions for sync fields per backend.
- Add capability assertions proving unsupported/emulated queue wait/signal paths are not reported as real support on DX11/OpenGL and any backend where the path remains unsupported.
- Add cross-backend consistency coverage for submit-returned fence values on available hardware backends.
- Add compile coverage for fake devices updated to the new `uint64` submit signature by building `RenderHonestyValidation`, `GPUUploadServiceValidation`, `GPUResourceManagerValidation`, and `ResourceViewCacheValidation`.

---

## 5. Out of Scope

- Descriptor set binding parity.
- Barrier/split-barrier implementation beyond direct fence synchronization needs.
- Query/timestamp pool implementation.
- RenderGraph async queue scheduling.
- Upload service redesign.
- Full bindless parity.
- Metal backend work unless the current build requires signature updates.
- Deep Metal synchronization behavior beyond signature/capability honesty.
- Any render visual output changes.

---

## 6. Red Lines

- No submit path may signal a fence without returning the exact value submitted.
- No backend may use `completed + 1` as a submit value allocator when multiple submissions can be queued before completion.
- No CPU wait may be described or capability-reported as a GPU queue wait.
- No emulated DX11/OpenGL queue fence behavior may be reported as real queue synchronization.
- No DX12 mixed-queue batch submission capability may be reported until mixed-queue `SubmitCommandContexts()` is implemented and tested.
- No backend may report host fence signal support merely because it can signal through a default GPU queue.
- No existing caller may be forced to know the return value immediately; ignoring the returned `uint64` must remain valid C++.

---

## 7. Tests and Validation

Required build targets:

```powershell
cmake --build build/win_x64_debug --config Debug --target DX12Validation
cmake --build build/win_x64_debug --config Debug --target VulkanValidation
cmake --build build/win_x64_debug --config Debug --target DX11Validation
cmake --build build/win_x64_debug --config Debug --target CrossBackendValidation
cmake --build build/win_x64_debug --config Debug --target RenderHonestyValidation
cmake --build build/win_x64_debug --config Debug --target GPUUploadServiceValidation
cmake --build build/win_x64_debug --config Debug --target GPUResourceManagerValidation
cmake --build build/win_x64_debug --config Debug --target ResourceViewCacheValidation
```

Required test commands:

```powershell
build\win_x64_debug\Tests\Debug\DX12Validation.exe
build\win_x64_debug\Tests\Debug\VulkanValidation.exe
build\win_x64_debug\Tests\Debug\DX11Validation.exe
build\win_x64_debug\Tests\Debug\CrossBackendValidation.exe
build\win_x64_debug\Tests\Debug\RenderHonestyValidation.exe
build\win_x64_debug\Tests\Debug\GPUUploadServiceValidation.exe
build\win_x64_debug\Tests\Debug\GPUResourceManagerValidation.exe
build\win_x64_debug\Tests\Debug\ResourceViewCacheValidation.exe
ctest --test-dir build/win_x64_debug -C Debug --output-on-failure -R "DX12Validation|VulkanValidation|DX11Validation|CrossBackendValidation|RenderHonestyValidation|GPUUploadServiceValidation|GPUResourceManagerValidation|ResourceViewCacheValidation"
```

Visual gate:

- N/A for R1a. This sub-stage changes synchronization contract only and must not change render output.

---

## 8. Implementation Order

1. Update public RHI interfaces and comments.
2. Add sync capability fields and backend initialization.
3. Update backend submit functions to return submitted fence values.
4. Add backend-local monotonic fence value allocation where missing.
5. Make CPU signal and queue signal semantics consistent.
6. Make command-context queue wait/signal either real or capability-visible unsupported.
7. Update tests and local test doubles for new return signatures.
8. Run validation.
9. Run Spark code review.
10. Update `phase-log.md`.
11. Commit R1a.

---

## 9. Expected Commit Boundary

Commit message:

```text
fix(rhi): define submit fence value contract
```
