# Render Module Optimization Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Reduce render-thread stalls, upload overhead, and per-frame CPU churn in the Render module while preserving current RHI abstraction boundaries.

**Architecture:** Start with GPU upload batching because it is on the frame-critical path and has focused test seams. Then tighten descriptor/frame-resource ownership, move more frame resources into RenderGraph, and add metrics so later optimization work is measured instead of guessed.

**Tech Stack:** C++20, CMake, RenderVerseX RHI, standalone validation executables in `Tests/`.

---

## File Map

- `Render/Include/Render/GPUUploadService.h`: public upload service API and upload batch state.
- `Render/Private/GPUUploadService.cpp`: staged buffer/texture upload recording, batch flush, completion tracking.
- `Render/Private/GPUResourceManager.cpp`: calls upload-service flush after processing queued resources.
- `Tests/GPUUploadServiceValidation/main.cpp`: focused tests for upload-service behavior without linking the full Render target.
- `Tests/GPUResourceManagerValidation/main.cpp`: existing integration tests updated for explicit batch flush semantics.
- `Tests/CMakeLists.txt`: adds focused validation executable.
- Future tasks:
  - `Render/Include/Render/PipelineCache.h`
  - `Render/Private/PipelineCache.cpp`
  - `Render/Private/Graph/*.cpp`
  - `Render/Private/Passes/*.cpp`
  - `Render/Include/Render/Debug/GPUProfiler.h`

---

## Task List

### Task 1: Batch staged GPU uploads

**Files:**
- Modify: `Render/Include/Render/GPUUploadService.h`
- Modify: `Render/Private/GPUUploadService.cpp`
- Modify: `Render/Private/GPUResourceManager.cpp`
- Create: `Tests/GPUUploadServiceValidation/main.cpp`
- Modify: `Tests/GPUResourceManagerValidation/main.cpp`
- Modify: `Tests/CMakeLists.txt`

- [x] **Step 1: Write failing test**

Add a test that uploads two staged buffers and asserts:
- no command context is submitted before `FlushBatchUploads()`
- only one copy command context is created
- one flush submits one command context and one fence
- both upload IDs complete after the shared fence signals

- [x] **Step 2: Verify test fails**

Run:

```powershell
cmake --build build_codex --config Release --target GPUUploadServiceValidation
```

Expected: compile failure before implementation because `GPUUploadService::FlushBatchUploads()` does not exist.

- [x] **Step 3: Implement minimal batching**

Record staged buffer and texture copies into a persistent copy command context. Track uploads as pending immediately, attach a shared fence when `FlushBatchUploads()` submits the batch, and keep immediate fallback unchanged.

- [x] **Step 4: Wire manager flush**

Call `m_uploadService->FlushBatchUploads()` after `GPUResourceManager::ProcessPendingUploads()` drains its queue for the frame, and after direct `UploadImmediate()` calls so staged commands are submitted.

- [x] **Step 5: Verify**

Run:

```powershell
cmake --build build_codex --config Release --target RVX_Render
cmake --build build_codex --config Release --target GPUUploadServiceValidation
build_codex\Tests\Release\GPUUploadServiceValidation.exe
```

Expected: builds pass; upload-service validation reports all tests passed.

### Task 2: Reduce upload object churn

**Files:**
- Modify: `Render/Include/Render/GPUUploadService.h`
- Modify: `Render/Private/GPUUploadService.cpp`
- Test: `Tests/GPUUploadServiceValidation/main.cpp`

- [ ] Pool staging buffers by size class for completed uploads.
- [ ] Reuse fences across completed batches.
- [ ] Add stats for created/reused staging buffers and fences.
- [ ] Verify repeated staged uploads stop linearly increasing staging/fence creation counts.

### Task 3: Narrow resource completion scans

**Files:**
- Modify: `Render/Include/Render/GPUResourceManager.h`
- Modify: `Render/Private/GPUResourceManager.cpp`
- Test: `Tests/GPUResourceManagerValidation/main.cpp`

- [ ] Track only resources with pending upload IDs in a dedicated set.
- [ ] Update the set when uploads start, complete, or fail.
- [ ] Verify `UpdateCompletedResourceUploads()` does not scan already resident resources.

### Task 4: Descriptor and frame-resource allocator pass

**Files:**
- Modify: `Render/Include/Render/PipelineCache.h`
- Modify: `Render/Private/PipelineCache.cpp`
- Modify: `Render/Private/Passes/OpaquePass.cpp`
- Test: add focused fake-RHI descriptor validation.

- [ ] Convert per-draw descriptor churn into frame-scoped descriptor cache or dynamic offsets.
- [ ] Keep object/material constants in frame-ring storage.
- [ ] Verify two draws in one frame bind distinct constant offsets without allocating unbounded persistent descriptor sets.

### Task 5: RenderGraph ownership tightening

**Files:**
- Modify: `Render/Private/Renderer/SceneRenderer.cpp`
- Modify: `Render/Private/Graph/RenderGraph*.cpp`
- Modify: built-in pass files under `Render/Private/Passes/`

- [ ] Move transient color/depth resources fully under RenderGraph.
- [ ] Ensure passes declare reads/writes instead of creating frame resources directly.
- [ ] Add pass-culling and aliasing validation for disabled/unread resources.

### Task 6: Metrics and profiling

**Files:**
- Modify: `Render/Include/Render/GPUUploadService.h`
- Modify: `Render/Include/Render/GPUResourceManager.h`
- Modify: `Render/Private/Debug/GPUProfiler.cpp`

- [ ] Add upload queue depth, batch count, bytes per frame, staging bytes in flight.
- [ ] Add render-visible/skipped counts.
- [ ] Surface metrics through existing debug/profiler path.
