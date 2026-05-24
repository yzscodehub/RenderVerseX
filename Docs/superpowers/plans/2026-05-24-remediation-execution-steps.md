# RenderVerseX Remediation Execution Steps Implementation Plan

> **Status:** Tracked remediation roadmap. Some Phase 1 / platform-build items have already landed on `engine-remediation`, including platform-specific shared presets and CI coverage. Treat this document as the execution backlog and rationale; use current repo files as the exact source of truth for already-implemented CI / preset details.

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Turn the current audit findings into a staged remediation program where Debug builds, CI, platform builds, resource ownership, RHI synchronization, and upper-layer rendering features become independently verifiable.

**Architecture:** Execute low-cost correctness and visibility fixes first, because later lifecycle and synchronization changes need trustworthy Debug assertions and CI gates. Then close platform/build gaps, then fix Core/RHI/Resource ownership, and only then enable higher-risk RenderGraph async/aliasing and Editor/feature wiring.

**Tech Stack:** C++20, CMake 3.21+ presets, vcpkg manifest mode, standalone validation executables, CTest labels, DX11/DX12/Vulkan/Metal/OpenGL RHI backends.

---

## Execution Rules

- Work one phase at a time. Do not start Phase 3 until Phase 1 and Phase 2 gates pass on the relevant platform.
- Each task gets its own commit.
- Every behavior change starts with a focused validation test or compile/configuration check.
- Source-grep tests are allowed only with CTest label `lint`; they do not count as behavior coverage.
- GPU tests remain separate from PR CI unless a self-hosted GPU runner exists; they must be run manually or nightly before backend releases.
- Audit docs under `Docs/superpowers/` are planning context; current code, CMake presets, and workflow files remain the source of truth for implemented behavior.

## Phase 1 - Make Debug And CI Truthful

This phase is first because it is low-cost and changes whether later bugs are visible.

### Task 1.1: Promote `RVX_DEBUG` And Portable Debug Break To Core

**Files:**
- Modify: `CMakeLists.txt`
- Modify: `Core/CMakeLists.txt`
- Modify: `Core/Include/Core/Assert.h`
- Modify: `Debug/CMakeLists.txt`
- Create: `Tests/CoreDebugConfigValidation/main.cpp`
- Modify: `Tests/CMakeLists.txt`

- [ ] **Step 1: Add a public options target**

In root `CMakeLists.txt`, after the C++ standard block, add an interface target:

```cmake
add_library(RVX_Options INTERFACE)
add_library(RVX::Options ALIAS RVX_Options)

target_compile_features(RVX_Options INTERFACE cxx_std_20)
target_compile_definitions(RVX_Options INTERFACE
    $<$<CONFIG:Debug>:RVX_DEBUG=1>
)
```

- [ ] **Step 2: Link Core to the options target**

In `Core/CMakeLists.txt`, add `RVX::Options` to `target_link_libraries(RVX_Core PUBLIC ...)`:

```cmake
target_link_libraries(RVX_Core PUBLIC
    RVX::Options
    spdlog::spdlog
    glm::glm
)
```

- [ ] **Step 3: Make `RVX_DEBUGBREAK()` portable**

In `Core/Include/Core/Assert.h`, replace the current `RVX_DEBUGBREAK()` block with:

```cpp
#include <cstdlib>
#include <csignal>

#ifdef RVX_DEBUG
    #if defined(_MSC_VER)
        #define RVX_DEBUGBREAK() __debugbreak()
    #elif defined(__clang__) || defined(__GNUC__)
        #define RVX_DEBUGBREAK() __builtin_trap()
    #else
        #define RVX_DEBUGBREAK() std::raise(SIGTRAP)
    #endif
#else
    #define RVX_DEBUGBREAK() ((void)0)
#endif
```

- [ ] **Step 4: Stop defining `RVX_DEBUG` only inside Debug module**

In `Debug/CMakeLists.txt`, remove `$<$<CONFIG:Debug>:RVX_DEBUG=1>` and keep only profiling if desired:

```cmake
target_compile_definitions(RVX_Debug PRIVATE
    $<$<CONFIG:Debug>:RVX_ENABLE_PROFILING=1>
)
```

- [ ] **Step 5: Add a compile-time validation executable**

Create `Tests/CoreDebugConfigValidation/main.cpp`:

```cpp
#include "Core/Assert.h"
#include "Core/Log.h"

int main()
{
#if defined(RVX_EXPECT_DEBUG_CONFIG) && !defined(RVX_DEBUG)
    #error "RVX_DEBUG must be defined for Debug configuration targets"
#endif

    RVX::Log::Initialize();
    RVX_CORE_INFO("Core debug configuration validation passed");
    RVX::Log::Shutdown();
    return 0;
}
```

- [ ] **Step 6: Register the validation target**

In `Tests/CMakeLists.txt`, add:

```cmake
add_executable(CoreDebugConfigValidation
    CoreDebugConfigValidation/main.cpp
)
target_link_libraries(CoreDebugConfigValidation PRIVATE
    RVX_TestFramework
    RVX::Core
)
target_compile_definitions(CoreDebugConfigValidation PRIVATE
    $<$<CONFIG:Debug>:RVX_EXPECT_DEBUG_CONFIG=1>
)
target_compile_features(CoreDebugConfigValidation PRIVATE cxx_std_20)

add_test(NAME CoreDebugConfigValidation COMMAND CoreDebugConfigValidation)
set_tests_properties(CoreDebugConfigValidation PROPERTIES LABELS "unit" TIMEOUT 30)
```

Add `CoreDebugConfigValidation` to the existing unit `set_tests_properties(...)` group only if keeping a single grouped call.

- [ ] **Step 7: Verify**

Run:

```bash
cmake -B build_debug_assert -S . -DCMAKE_BUILD_TYPE=Debug -DRVX_ENABLE_DX11=OFF -DRVX_ENABLE_DX12=OFF -DRVX_ENABLE_VULKAN=OFF -DRVX_ENABLE_OPENGL=OFF -DRVX_BUILD_SAMPLES=OFF -DRVX_BUILD_TESTS=ON
cmake --build build_debug_assert --target CoreDebugConfigValidation -j
ctest --test-dir build_debug_assert -R CoreDebugConfigValidation --output-on-failure
```

Expected: configure succeeds, target builds, CTest passes.

- [ ] **Step 8: Commit**

```bash
git add CMakeLists.txt Core/CMakeLists.txt Core/Include/Core/Assert.h Debug/CMakeLists.txt Tests/CMakeLists.txt Tests/CoreDebugConfigValidation/main.cpp
git commit -m "build: enable debug assertions through shared core options"
```

### Task 1.2: Fix CI Presets And CTest Test Directory

Current branch note: the implemented shared presets use platform-specific names such as `win_x64_debug`, `linux_x64_debug`, and `mac_arm64_debug`. Preserve this task's intent, not the older generic `debug` / `release` preset names shown in early examples.

**Files:**
- Modify: `CMakePresets.json`
- Modify: `.github/workflows/ci.yml`

- [ ] **Step 1: Remove personal vcpkg path from shared presets**

In `CMakePresets.json`, delete:

```json
"environment": {
  "VCPKG_ROOT": "<local-vcpkg-root>"
},
```

The shared preset should keep:

```json
"CMAKE_TOOLCHAIN_FILE": {
  "value": "$env{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake",
  "type": "FILEPATH"
}
```

Local developer paths belong in `CMakeUserPresets.json`, not the repo preset.

- [ ] **Step 2: Add CTest presets**

Append a `testPresets` array in `CMakePresets.json`:

```json
"testPresets": [
  {
    "name": "release-unit-lint",
    "configurePreset": "release",
    "configuration": "Release",
    "output": {
      "outputOnFailure": true
    },
    "filter": {
      "include": {
        "label": "unit|lint"
      }
    }
  },
  {
    "name": "debug-unit-lint",
    "configurePreset": "debug",
    "configuration": "Debug",
    "output": {
      "outputOnFailure": true
    },
    "filter": {
      "include": {
        "label": "unit|lint"
      }
    }
  }
]
```

- [ ] **Step 3: Use the CTest preset on Windows CI**

In `.github/workflows/ci.yml`, replace:

```yaml
run: ctest --test-dir build -L "unit|lint" --output-on-failure
```

with:

```yaml
run: ctest --preset release-unit-lint
```

- [ ] **Step 4: Verify**

Run on Windows:

```bash
cmake --preset release
cmake --build --preset release
ctest --preset release-unit-lint
```

Expected: tests are discovered from `build/Release`, not `build`.

- [ ] **Step 5: Commit**

```bash
git add CMakePresets.json .github/workflows/ci.yml
git commit -m "ci: use portable presets and correct ctest directory"
```

### Task 1.3: Reclassify Remaining Source-Grep Validation

**Files:**
- Modify: `Tests/CMakeLists.txt`
- Modify: `Tests/RenderPassBindingValidation/main.cpp`

- [ ] **Step 1: Add honesty banner to `RenderPassBindingValidation`**

At the top of `Tests/RenderPassBindingValidation/main.cpp`, after includes, add:

```cpp
// =============================================================================
// SOURCE-PATTERN LINT - NOT A BEHAVIORAL TEST.
// These checks grep source files for expected binding patterns. They prove that
// strings are present in source, not that rendering behavior is correct.
// CTest label: "lint".
// =============================================================================
```

- [ ] **Step 2: Move it from unit to lint**

In `Tests/CMakeLists.txt`, remove `RenderPassBindingValidation` from the unit group and add it to the lint group:

```cmake
add_test(NAME RenderPassBindingValidation COMMAND RenderPassBindingValidation)
set_tests_properties(RenderPassBindingValidation PROPERTIES LABELS "lint" TIMEOUT 60)
```

Ensure the unit `set_tests_properties(...)` list no longer includes `RenderPassBindingValidation`.

- [ ] **Step 3: Verify labels**

Run:

```bash
ctest --test-dir build -N -L unit
ctest --test-dir build -N -L lint
```

Expected: `RenderPassBindingValidation` appears only under `lint`.

- [ ] **Step 4: Commit**

```bash
git add Tests/CMakeLists.txt Tests/RenderPassBindingValidation/main.cpp
git commit -m "test: mark render pass source-pattern validation as lint"
```

## Phase 2 - Make Platform Builds Honest

This phase makes the advertised platform/backend matrix match build reality.

### Task 2.1: Add A Linux ShaderCompiler Backend

**Files:**
- Modify: `vcpkg.json`
- Modify: `ShaderCompiler/CMakeLists.txt`
- Modify: `ShaderCompiler/Private/ShaderCompiler.cpp`
- Create: `ShaderCompiler/Private/GlslangShaderCompiler.cpp`
- Modify: `ShaderCompiler/Private/DXCCompiler_Apple.cpp`
- Create: `Tests/ShaderCompilerValidation/main.cpp`
- Modify: `Tests/CMakeLists.txt`

- [ ] **Step 1: Extend glslang dependency to Linux**

In `vcpkg.json`, change:

```json
{
  "name": "glslang",
  "platform": "osx"
}
```

to:

```json
{
  "name": "glslang",
  "platform": "linux | osx"
}
```

- [ ] **Step 2: Move Apple glslang compiler into a platform-neutral file**

Create `ShaderCompiler/Private/GlslangShaderCompiler.cpp` by moving the reusable compiler implementation from `DXCCompiler_Apple.cpp`. Keep the factory function name explicit:

```cpp
namespace RVX
{
    std::unique_ptr<IShaderCompiler> CreateGlslangShaderCompiler();
} // namespace RVX
```

Leave `DXCCompiler_Apple.cpp` either deleted from the build or reduced to no build participation. Do not keep duplicate class definitions.

- [ ] **Step 3: Update compiler factory**

In `ShaderCompiler/Private/ShaderCompiler.cpp`, select the backend by platform:

```cpp
#include "ShaderCompiler/ShaderCompiler.h"

namespace RVX
{
#if defined(_WIN32)
    std::unique_ptr<IShaderCompiler> CreateDXCShaderCompiler();
#else
    std::unique_ptr<IShaderCompiler> CreateGlslangShaderCompiler();
#endif

    std::unique_ptr<IShaderCompiler> CreateShaderCompiler()
    {
#if defined(_WIN32)
        return CreateDXCShaderCompiler();
#else
        return CreateGlslangShaderCompiler();
#endif
    }
} // namespace RVX
```

- [ ] **Step 4: Update ShaderCompiler CMake**

In `ShaderCompiler/CMakeLists.txt`, include `Private/GlslangShaderCompiler.cpp` for Linux and Apple:

```cmake
if(WIN32)
    target_sources(RVX_ShaderCompiler PRIVATE
        Private/DXCCompiler.cpp
        Private/DX11SlotMapper.cpp
        Private/SPIRVCrossTranslator.cpp
        Private/TrackingIncludeHandler.h
        Private/TrackingIncludeHandler.cpp
    )
elseif(APPLE OR UNIX)
    target_sources(RVX_ShaderCompiler PRIVATE
        Private/GlslangShaderCompiler.cpp
        Private/SPIRVCrossTranslator.cpp
    )
endif()
```

Link glslang for `APPLE OR UNIX`:

```cmake
if(APPLE OR UNIX)
    target_link_libraries(RVX_ShaderCompiler PRIVATE
        glslang::glslang
        glslang::glslang-default-resource-limits
        glslang::SPIRV
    )
endif()
```

- [ ] **Step 5: Add behavior validation**

Create `Tests/ShaderCompilerValidation/main.cpp` with a CPU-only compile of `Tests/Shaders/Triangle.hlsl` for Vulkan. Validate:

```cpp
TEST_ASSERT_TRUE(result.success);
TEST_ASSERT_TRUE(!result.bytecode.empty());
TEST_ASSERT_TRUE(result.errorMessage.empty());
```

Register it in `Tests/CMakeLists.txt` as label `unit`.

- [ ] **Step 6: Verify Linux**

Run:

```bash
cmake -B build_linux -S . -DRVX_ENABLE_DX11=OFF -DRVX_ENABLE_DX12=OFF -DRVX_ENABLE_VULKAN=ON -DRVX_ENABLE_OPENGL=ON -DRVX_BUILD_TESTS=ON
cmake --build build_linux --target ShaderCompilerValidation -j
ctest --test-dir build_linux -R ShaderCompilerValidation --output-on-failure
```

Expected: Linux builds and shader validation passes.

- [ ] **Step 7: Commit**

```bash
git add vcpkg.json ShaderCompiler/CMakeLists.txt ShaderCompiler/Private/ShaderCompiler.cpp ShaderCompiler/Private/GlslangShaderCompiler.cpp ShaderCompiler/Private/DXCCompiler_Apple.cpp Tests/CMakeLists.txt Tests/ShaderCompilerValidation/main.cpp
git commit -m "shader: add linux glslang compiler backend"
```

### Task 2.2: Fix RHI Backend Selection And Linking

**Files:**
- Modify: `RHI/Include/RHI/RHIDefinitions.h`
- Modify: `RHI/Private/RHIModule.cpp`
- Modify: `RHI/CMakeLists.txt`
- Modify: backend `CMakeLists.txt` files as needed
- Modify: `Samples/*/CMakeLists.txt`
- Modify: `Editor/CMakeLists.txt`

- [ ] **Step 1: Make `SelectBestBackend()` respect compiled backends**

Update `SelectBestBackend()` so it checks `RVX_ENABLE_DX12`, `RVX_ENABLE_DX11`, `RVX_ENABLE_VULKAN`, `RVX_ENABLE_METAL`, and `RVX_ENABLE_OPENGL` in platform order.

- [ ] **Step 2: Make `CreateRHIDevice(Auto, ...)` resolve Auto**

In `RHI/Private/RHIModule.cpp`, before the switch:

```cpp
if (backend == RHIBackendType::Auto)
{
    backend = SelectBestBackend();
}
```

If selected backend is not compiled, log and return `nullptr`.

- [ ] **Step 3: Add backend aggregation target**

In `RHI/CMakeLists.txt` or root `CMakeLists.txt`, create:

```cmake
add_library(RVX_RHIBackends INTERFACE)
add_library(RVX::RHIBackends ALIAS RVX_RHIBackends)
```

After backend subdirectories are conditionally added, link enabled backend aliases into `RVX_RHIBackends`.

- [ ] **Step 4: Link executable consumers to `RVX::RHIBackends`**

Update samples, editor executable, and validation executables that call `CreateRHIDevice()` so they link `RVX::RHIBackends`.

- [ ] **Step 5: Verify**

Run:

```bash
cmake --build build --target Triangle
cmake --build build --target RVXEditor
```

Expected: consumers that call RHI factory link without manually enumerating every backend.

- [ ] **Step 6: Commit**

```bash
git add RHI/Include/RHI/RHIDefinitions.h RHI/Private/RHIModule.cpp RHI/CMakeLists.txt CMakeLists.txt Samples Editor Tests
git commit -m "rhi: add backend aggregation and auto selection"
```

### Task 2.3: Close Vulkan Surface And Metal Build Gaps

**Files:**
- Modify: `RHI_Vulkan/Private/VulkanCommon.h`
- Modify: `RHI_Vulkan/Private/VulkanDevice.cpp`
- Modify: `RHI_Vulkan/Private/VulkanSwapChain.cpp`
- Modify: `HAL/Include/HAL/Window.h` or equivalent window abstraction
- Modify: `RHI_Metal/CMakeLists.txt`
- Modify: `RHI_Metal/Private/MetalUpload.mm`

- [ ] **Step 1: Remove unconditional Win32 Vulkan macro**

Replace unconditional `VK_USE_PLATFORM_WIN32_KHR` with platform-specific definitions. Prefer GLFW surface creation if the HAL window is GLFW-backed.

- [ ] **Step 2: Add a platform-neutral surface creation path**

Use one of these two designs and keep it consistent:

1. `RHISwapChainDesc` carries a native surface callback for Vulkan.
2. HAL exposes `CreateVulkanSurface(VkInstance, VkSurfaceKHR*)`.

Use GLFW `glfwCreateWindowSurface` for GLFW windows.

- [ ] **Step 3: Add Metal missing sources**

In `RHI_Metal/CMakeLists.txt`, add:

```cmake
Private/MetalQuery.mm
Private/MetalUpload.mm
```

- [ ] **Step 4: Fix Metal upload enum names**

In `RHI_Metal/Private/MetalUpload.mm`, replace `RHIBufferUsage::TransferSrc` with `RHIBufferUsage::CopySrc`.

- [ ] **Step 5: Verify**

Run platform-specific configure/build:

```bash
cmake -B build_linux -S . -DRVX_ENABLE_VULKAN=ON -DRVX_ENABLE_OPENGL=ON -DRVX_ENABLE_DX11=OFF -DRVX_ENABLE_DX12=OFF
cmake --build build_linux --target VulkanValidation -j
```

On macOS:

```bash
cmake --preset macos-release
cmake --build --preset macos-release
```

- [ ] **Step 6: Commit**

```bash
git add RHI_Vulkan RHI_Metal HAL RHI
git commit -m "rhi: fix vulkan surface portability and metal build sources"
```

## Phase 3 - Fix Core And Resource Lifecycle Correctness

### Task 3.1: Make `JobGraph` Non-Deadlocking And Testable

**Files:**
- Modify: `Core/Include/Core/Job/JobGraph.h`
- Modify: `Core/Private/Job/JobGraph.cpp`
- Create: `Tests/JobGraphValidation/main.cpp`
- Modify: `Tests/CMakeLists.txt`

**Required behavior:**
- `Execute()` validates graph before scheduling.
- `Execute()` returns success/failure.
- No task is submitted while holding `m_mutex`.
- `Wait()` uses `std::condition_variable`, not spin/yield.
- Disabled or uninitialized `JobSystem` cannot deadlock synchronous fallback.

**Validation cases:**
- Linear dependency graph completes in order.
- Fan-out/fan-in graph completes once.
- Cycle is rejected before wait.
- Running with uninitialized JobSystem completes synchronously without deadlock.

**Commands:**

```bash
cmake --build build --target JobGraphValidation -j
ctest --test-dir build -R JobGraphValidation --output-on-failure
```

### Task 3.2: Fix Allocator Alignment And Frame Count Assumptions

**Files:**
- Modify: `Core/Private/Memory/Allocators.cpp`
- Modify: `Core/Include/Core/Memory/Allocators.h`
- Create: `Tests/AllocatorValidation/main.cpp`
- Modify: `Tests/CMakeLists.txt`

**Required behavior:**
- `LinearAllocator` base allocation supports requested alignment.
- Alignment must be a power of two.
- Returned pointer address satisfies requested alignment.
- `FrameAllocator` buffer count matches configured frame buffering or explicitly documents and enforces main-thread use.

**Validation cases:**
- Allocate 16, 32, 64, 256 byte aligned blocks.
- Invalid alignment fails deterministically.
- `NextFrame()` does not race with allocation under documented constraints.

### Task 3.3: Centralize Resource Identity And Cache Ownership

**Files:**
- Modify: `Resource/Private/ResourceManager.cpp`
- Modify: `Resource/Private/Loader/TextureLoader.cpp`
- Modify: `Resource/Private/Loader/ModelLoader.cpp`
- Modify: `Resource/Private/Loader/HDRTextureLoader.cpp`
- Modify: `Resource/Private/Loader/AudioLoader.cpp`
- Modify: `Resource/Private/ResourceCache.cpp`
- Modify: `Resource/Include/Resource/ResourceCache.h`
- Create: `Tests/ResourceCacheValidation/main.cpp`
- Modify: `Tests/CMakeLists.txt`

**Required behavior:**
- Loaders parse and return resources only.
- `ResourceManager` alone assigns id/path/name, calls `NotifyLoaded()`, stores cache, and registers dependencies.
- Canonical path or GUID is the single resource identity.
- `ResourceCache::Evict()` never re-locks the same mutex through public methods.
- Newly inserted loading resources cannot be immediately evicted before the caller receives a stable handle.

**Validation cases:**
- Loading the same texture path twice returns one cache entry.
- `Unload(path)` removes the actual cached entry.
- Low memory limit does not free the resource before `LoadResource()` returns.
- Explicit `Evict()` does not deadlock.

### Task 3.4: Fix Resource Async Loading And Hot Reload Dispatch

**Files:**
- Modify: `Resource/Private/ResourceManager.cpp`
- Modify: `Resource/Private/HotReloadManager.cpp`
- Modify: `Resource/Private/FileWatcher.cpp`
- Modify: `Resource/Include/Resource/ResourceHandle.h`
- Create: `Tests/ResourceAsyncValidation/main.cpp`
- Create: `Tests/ResourceHotReloadValidation/main.cpp`
- Modify: `Tests/CMakeLists.txt`

**Required behavior:**
- Disk I/O/import runs outside the global load mutex.
- Concurrent requests for the same resource share one in-flight operation.
- `WaitForLoad()` does not busy-wait.
- Background file watching collects changes; main thread dispatches reload callbacks.
- Reload loads replacement first, then swaps; failure preserves old resource.

## Phase 4 - Fix RHI And RenderGraph Synchronization

### Task 4.1: Extend Barrier Model With Stage, Access, And Queue Ownership

**Files:**
- Modify: `RHI/Include/RHI/RHICommandContext.h`
- Modify: `RHI/Include/RHI/RHIDefinitions.h`
- Modify: `Render/Private/Graph/RenderGraphCompiler.cpp`
- Modify: `RHI_Vulkan/Private/VulkanCommandContext.cpp`
- Modify: `RHI_DX12/Private/DX12CommandContext.cpp`
- Modify backend conversion helpers
- Create: `Tests/RHIBarrierValidation/main.cpp`
- Modify: `Tests/CMakeLists.txt`

**Required behavior:**
- Barrier carries state, src/dst stage, src/dst access, and optional source/destination queue type.
- Vulkan generates queue family release/acquire when resource moves between graphics/compute/copy queues.
- Backends that do not support split barriers do not advertise split barrier support.

### Task 4.2: Make RenderGraph External State And Resource Lifetime Correct

**Files:**
- Modify: `Render/Private/Renderer/SceneRenderer.cpp`
- Modify: `Render/Private/Graph/RenderGraph.cpp`
- Modify: `Render/Private/Graph/RenderGraphCompiler.cpp`
- Modify: `Render/Private/Graph/RenderGraphExecutor.cpp`
- Modify: `Render/Private/Graph/FrameResourceManager.cpp`
- Create: `Tests/RenderGraphLifetimeValidation/main.cpp`
- Modify: `Tests/CMakeLists.txt`

**Required behavior:**
- Imported resources expose final state back to caller or use explicit export state.
- Depth state after skybox/transparent read is tracked correctly across frames.
- Culled pass outputs are not physically allocated unless exported or side-effecting.
- Transient resources retire only after GPU fence completion.

### Task 4.3: Gate Memory Aliasing And Async Compute Until Fully Implemented

**Files:**
- Modify: `Render/Include/Render/Graph/RenderGraph.h`
- Modify: `Render/Private/Graph/RenderGraphExecutor.cpp`
- Modify: `Render/Private/Graph/RenderGraphCompiler.cpp`
- Modify: `RHI/Include/RHI/RHICommandContext.h`
- Create: `Tests/RenderGraphAliasingValidation/main.cpp`
- Modify: `Tests/CMakeLists.txt`

**Required behavior:**
- If aliasing barrier support is absent, memory aliasing is disabled by default or fails validation.
- Async compute fallback is explicit in API/docs and logs once when requested.
- True async compute requires queue scheduler, fence waits, and queue ownership transfer from Task 4.1.

## Phase 5 - Wire Upper-Layer Systems Only After Foundations Pass

### Task 5.1: Editor Uses Engine And RHI Instead Of Standalone Shell

**Files:**
- Modify: `Editor/Private/EditorApplication.cpp`
- Modify: `Editor/Private/Panels/Viewport.cpp`
- Modify: `Editor/CMakeLists.txt`
- Modify: `Engine/Include/Engine/Engine.h`
- Modify: `Render/Include/Render/RenderSubsystem.h`

**Required behavior:**
- Editor initializes `Engine`, `WindowSubsystem`, `RenderSubsystem`, and active `World`.
- Viewport renders through RHI/RenderSubsystem.
- Menu actions that remain unsupported are disabled or hidden, not empty callbacks.

### Task 5.2: Particle Simulation Path Runs

**Files:**
- Modify: `Particle/Private/ParticleSubsystem.cpp`
- Modify: `Particle/Private/ParticleSystemInstance.cpp`
- Modify: `Particle/Private/GPU/CPUParticleSimulator.cpp`
- Modify: `Particle/Private/GPU/GPUParticleSimulator.cpp`
- Create: `Tests/ParticleSimulationValidation/main.cpp`
- Modify: `Tests/CMakeLists.txt`

**Required behavior:**
- `ParticleSystemInstance` receives CPU or GPU simulator.
- CPU simulator updates alive count in CPU-only tests.
- GPU simulator path remains capability-gated.

### Task 5.3: Terrain And Water Passes Either Render Or Are Marked Experimental

**Files:**
- Modify: `Terrain/Private/TerrainRenderer.cpp`
- Modify: `Water/Private/WaterRenderer.cpp`
- Modify: `Render/Private/Renderer/RenderPassRegistry.cpp`
- Modify relevant docs or module headers

**Required behavior:**
- Production pass registration only includes passes that declare resources and issue draw/dispatch work.
- Experimental passes are opt-in and named as experimental in logs/config.

### Task 5.4: Physics Component Creates Bodies In PhysicsWorld

**Files:**
- Modify: `Physics/Private/PhysicsWorld.cpp`
- Modify: `Scene/Private/Components/RigidBodyComponent.cpp`
- Modify: `World/Private/World.cpp`
- Create: `Tests/PhysicsIntegrationValidation/main.cpp`
- Modify: `Tests/CMakeLists.txt`

**Required behavior:**
- `RigidBodyComponent` can resolve the active physics world.
- Dynamic body moves after fixed-step simulation.
- Entity transform syncs from physics after step.

## Release Gates

### Gate A - Fast PR Gate

Run on every PR:

```bash
cmake --preset win_x64_debug
cmake --build --preset win_x64_debug
ctest --preset win_x64_debug_unit_lint
```

Linux:

```bash
cmake --preset linux_x64_debug
cmake --build --preset linux_x64_debug
ctest --preset linux_x64_debug_unit_lint
```

macOS ARM64:

```bash
cmake --preset mac_arm64_debug
cmake --build --preset mac_arm64_debug
ctest --preset mac_arm64_debug_unit_lint
```

### Gate B - Debug Correctness Gate

Run before merging Core/RHI/Resource changes:

```bash
cmake --preset linux_x64_debug
cmake --build --preset linux_x64_debug
ctest --preset linux_x64_debug_unit_lint
```

### Gate C - Backend/GPU Gate

Run manually or on self-hosted/nightly runners:

```bash
ctest --test-dir build -L gpu --output-on-failure
```

Minimum expected backend matrix:
- Windows: DX11, DX12, Vulkan
- Linux: Vulkan, OpenGL
- macOS: Metal

## Recommended Commit Order

1. `build: enable debug assertions through shared core options`
2. `ci: use portable presets and correct ctest directory`
3. `test: mark render pass source-pattern validation as lint`
4. `shader: add linux glslang compiler backend`
5. `rhi: add backend aggregation and auto selection`
6. `rhi: fix vulkan surface portability and metal build sources`
7. `core: make job graph scheduling non-deadlocking`
8. `core: fix allocator alignment and frame lifetime assumptions`
9. `resource: centralize resource identity and cache ownership`
10. `resource: make async loading and hot reload main-thread safe`
11. `rhi: add stage access and queue ownership barriers`
12. `render: fix render graph external state and lifetime tracking`
13. `render: gate async compute and aliasing until fully synchronized`
14. `editor: initialize through engine render subsystem`
15. `particle: run simulator path for particle instances`
16. `render: gate terrain and water experimental passes`
17. `physics: connect rigid body components to physics world`
