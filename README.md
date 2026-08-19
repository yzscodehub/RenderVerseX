# RenderVerseX

A cross-platform, multi-backend graphics rendering engine written in C++20.

## Overview

RenderVerseX is a modern graphics engine designed for flexibility and performance across multiple platforms. It provides a unified API for rendering across DirectX 11, DirectX 12, Vulkan, Metal, and OpenGL backends, with advanced features including:

- **Multi-Backend RHI Abstraction** - Unified rendering interface across DX11/DX12/Vulkan/Metal/OpenGL
- **Frame Graph System** - Automatic resource management and barrier insertion
- **Entity-Component System** - Flexible scene architecture with spatial queries
- **GPU-Driven Particles** - High-performance particle system
- **Terrain & Water Rendering** - Advanced heightmap terrain and realistic water simulation
- **Physics Integration** - Jolt Physics for realistic simulations
- **AI & Navigation** - RecastNavigation for pathfinding
- **Scripting System** - Lua scripting via Sol2
- **Networking** - Asio-based networking for multiplayer support
- **Audio System** - MiniAudio for cross-platform audio
- **UI Framework** - Custom UI system with widgets
- **Asset Pipeline** - Comprehensive resource management and importing

## Requirements

- **CMake** 3.21 or higher
- **C++20** compatible compiler
  - Windows: Visual Studio 2022
  - Linux: GCC 11+ or Clang 13+
  - macOS: Xcode 14+
- **vcpkg** for dependency management
- Platform-specific SDKs:
  - Windows: Windows 10 SDK (for DX12)
  - Vulkan SDK (when enabling Vulkan backend)

## CMake Presets

The project includes CMakePresets.json for simplified cross-platform configuration. Presets automatically configure appropriate rendering backends for each platform:

### Available Presets
- `debug` - Windows Debug build (DX11/DX12/Vulkan/OpenGL enabled)
- `release` - Windows Release build
- `macos-debug` - macOS Debug build (Metal enabled)
- `macos-release` - macOS Release build

### Environment Setup
Presets expect the `VCPKG_ROOT` environment variable:
```bash
# Windows
set VCPKG_ROOT=E:/WorkSpace/vcpkg

# Linux/macOS
export VCPKG_ROOT=/path/to/vcpkg
```

## Platform Support

| Platform | DX11 | DX12 | Vulkan | Metal | OpenGL |
|----------|------|------|--------|-------|--------|
| Windows  | ✓    | ✓    | ✓      | ✗     | ✓      |
| Linux    | ✗    | ✗    | ✓      | ✗     | ✓      |
| macOS    | ✗    | ✗    | ✗      | ✓     | ✗      |
| iOS      | ✗    | ✗    | ✗      | ✓     | ✗      |

## Dependencies

Core dependencies are managed via vcpkg. Key libraries include:

- **spdlog** - Logging framework
- **GLFW** - Window and input handling
- **GLM** - Math library
- **Vulkan** + **VulkanMemoryAllocator** - Vulkan backend
- **SPIRV-Cross** - Shader cross-compilation
- **DirectX Shader Compiler** - DXC for HLSL compilation
- **DirectX12 Agility SDK** + **D3D12MemoryAllocator** - DX12 backend
- **GLAD** + **OpenGL** - OpenGL backend
- **tinygltf** - glTF model loading
- **stb** - Image loading
- **asio** - Networking
- **lua** + **sol2** - Scripting
- **ImGui** + **ImGuizmo** - Editor UI
- **Jolt Physics** - Physics simulation
- **RecastNavigation** - AI pathfinding
- **MiniAudio** - Audio playback
- **tinyexr** - EXR image support

See [vcpkg.json](vcpkg.json) for the complete dependency list.

## Building

### 1. Install vcpkg

```bash
git clone https://github.com/microsoft/vcpkg.git
cd vcpkg
.\bootstrap-vcpkg.bat  # Windows
./bootstrap-vcpkg.sh   # Linux/macOS
```

### 2. Install Dependencies

```bash
# Windows
vcpkg install spdlog glfw3 glm vulkan vulkan-memory-allocator spirv-reflect spirv-cross directx-dxc directx12-agility d3d12-memory-allocator glad imgui imguizmo lua sol2 asio joltphysics recastnavigation miniaudio tinygltf stb tinyexr miniz --triplet x64-windows

# Linux
vcpkg install spdlog glfw3 glm vulkan vulkan-memory-allocator spirv-reflect spirv-cross glad asio joltphysics recastnavigation miniaudio tinygltf stb tinyexr miniz imgui imguizmo lua sol2 --triplet x64-linux

# macOS
vcpkg install spdlog glfw3 glm spirv-cross spirv-cross-glsl spirv-cross-hlsl spirv-cross-msl glslang asio joltphysics recastnavigation miniaudio tinygltf stb tinyexr miniz imgui imguizmo lua sol2 --triplet x64-osx
```

### 3. Configure and Build

#### Using CMake Presets (Recommended)
The project includes CMakePresets.json for easy cross-platform configuration:

```bash
# Configure and build using presets
cmake --preset debug      # Debug configuration
cmake --preset release    # Release configuration

# macOS presets
cmake --preset macos-debug   # Debug on macOS
cmake --preset macos-release # Release on macOS

# Build specific targets
cmake --build --preset debug --target ModelViewer
cmake --build --preset debug --target BasicRHI
cmake --build --preset debug --target RenderingShowcase
```

#### Manual Configuration
```bash
# Configure with vcpkg toolchain
cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=[vcpkg-root]/scripts/buildsystems/vcpkg.cmake

# Build Release
cmake --build build --config Release

# Build Debug
cmake --build build --config Debug

# Build specific target
cmake --build build --config Release --target RenderVerseSamples
```

### Build Options

| Option | Default | Description |
|--------|---------|-------------|
| `-DRVX_ENABLE_DX11=ON/OFF` | ON | DirectX 11 backend (Windows only) |
| `-DRVX_ENABLE_DX12=ON/OFF` | ON | DirectX 12 backend (Windows only) |
| `-DRVX_ENABLE_VULKAN=ON/OFF` | ON | Vulkan backend |
| `-DRVX_ENABLE_METAL=ON/OFF` | OFF | Metal backend (macOS/iOS only) |
| `-DRVX_ENABLE_OPENGL=ON/OFF` | OFF | OpenGL backend |
| `-DRVX_BUILD_EDITOR=OFF` | OFF | Required for the pure-ECS Runtime milestone |
| `-DRVX_BUILD_SAMPLES=ON/OFF` | ON | Build sample applications |
| `-DRVX_BUILD_TESTS=ON/OFF` | ON | Build validation tests |

## Running Tests

Tests are standalone executables (no GoogleTest framework).

```bash
# Run individual validation tests
./build/Tests/Release/RenderGraphValidation.exe
./build/Tests/Release/EcsEntityValidation.exe
./build/Tests/Release/EcsSceneOrchestrationValidation.exe
./build/Tests/Release/EcsCleanupValidation.exe

# Cross-backend validation (requires multiple backends)
./build/Tests/Release/CrossBackendValidation.exe
```

### Architecture preflight

Before architecture implementation work, run the clean M0 gate documented in
[`Docs/build-truth.md`](Docs/build-truth.md). It is stricter than an ordinary
incremental build because it rejects missing tests and writes reviewable evidence.

## Sample Applications

`RenderVerseSamples` is the Runtime product proof surface while the Editor is
disabled. Each scene uses the same pure-ECS World, resource, render-publication,
diagnostic, screenshot, and cleanup contracts.

| Sample | Description |
|--------|-------------|
| **model-rendering** | Entity, transform, camera, light, atomic model adoption, frozen render snapshot |
| **model-viewer** | Environment loading, visibility, camera control, model activation |
| **pbr-materials** | Material slots, streamed textures, IBL, fully-resident texture receipts |
| **gpu-driven** | Direct/GPU-driven scene parity and indirect submission |
| **lighting-shadows** | Light fragments, shadows, and incremental scene extraction |
| **asset-gallery** | Multi-model transactions, layout, residency, and retirement |
| **render-pipeline** | Render passes, post-processing, and snapshot consistency |
| **scene-lifecycle** | Create, destroy, reparent, generation reuse, and cleanup |
| **asset-streaming** | Async load, cancel, unload, reload, and resource retirement |
| **interior-rendering** | Complex hierarchy, multi-light, multi-material environment |
| **physics-sandbox** | Fixed-step bodies, colliders, transform sync, and cleanup |
| **animation-character** | Skeleton, pose, root motion, physics intent, skinning receipts |
| **rendering-stress** | Large entity/query/dirty-update workloads and performance evidence |

## Project Structure

```
RenderVerseX/
├── Core/                    # Core utilities, handles, math, jobs
├── ECS/                     # Generic entity/fragment/query/processor kernel
├── Scene/                   # ECS fragments, hierarchy, spatial index, snapshots
├── World/                   # One SceneEcsRuntime and ECS camera service
├── Engine/                  # Per-World ECS composition and render publication
├── Resource/                # Asset loading, streaming, residency, retirement
├── ResourceSceneAdapters/   # Prepared batches and ECS asset coordinators
├── RenderExtraction/        # Frozen ECS snapshots to render contracts
├── Render/                  # RenderGraph, passes, scene database, GPU-driven path
├── Physics*/                # Physics runtime and pure ECS bridge
├── Animation*/              # Animation runtime, evaluator, and pure ECS bridge
├── Audio/                   # Audio runtime and pure ECS bridge
├── Particle/                # Particle runtime and pure ECS bridge
├── Terrain/                 # Terrain runtime and pure ECS bridge
├── Water/                   # Water runtime and pure ECS bridge
├── RHI*/                    # Backend-neutral RHI and platform backends
├── Samples/RenderVerseSamples/
├── Tests/
└── Docs/
```

## Code Style Guidelines

### Naming Conventions

| Element | Convention | Example |
|---------|------------|---------|
| Classes/Structs | PascalCase | `RenderGraph`, `SceneEcsRuntime` |
| Interfaces | `I` prefix + PascalCase | `IRHIDevice`, `IPhysicsBackend` |
| Methods | PascalCase | `CreateBuffer()`, `GetName()` |
| Member variables | `m_` prefix + camelCase | `m_device`, `m_position` |
| Static members | `s_` prefix + camelCase | `s_nextHandle`, `s_coreLogger` |
| Local variables | camelCase | `bufferDesc`, `devicePtr` |
| Constants | `RVX_` prefix + SCREAMING_SNAKE | `RVX_INVALID_INDEX` |
| Enums | `enum class` PascalCase | `RHIBackendType::Vulkan` |

### Include Order

1. Corresponding header (for .cpp files)
2. Project headers with `"quotes"` (alphabetized)
3. External library headers with `<angle brackets>`
4. Standard library headers with `<angle brackets>`

```cpp
#include "Scene/ECS/SceneEcsRuntime.h"  // Corresponding header first
#include "Core/Log.h"           // Project headers
#include <glm/glm.hpp>          // External libraries
#include <vector>               // Standard library
```

### Namespaces

All code resides in the `RVX` namespace with explicit closing comments:

```cpp
#pragma once
#include "Core/Types.h"

namespace RVX
{
    // All code in RVX namespace
} // namespace RVX
```

### Type Definitions

Use type aliases from `Core/Types.h`:

```cpp
using int8 = std::int8_t;       using uint8  = std::uint8_t;
using int16 = std::int16_t;     using uint16 = std::uint16_t;
using int32 = std::int32_t;     using uint32 = std::uint32_t;
using int64 = std::int64_t;     using uint64 = std::uint64_t;
using float32 = float;          using float64 = double;
```

### Enums

Always use `enum class` with explicit underlying type:

```cpp
enum class RHIBackendType : uint8 { None = 0, Auto, DX11, DX12, Vulkan, Metal, OpenGL };
```

### Documentation

Doxygen-style comments:

```cpp
/** @file RenderGraph.h  @brief Frame graph and automatic resource management */

/** @brief Create a buffer  @param desc Buffer description  @return Handle */
RHIBufferRef CreateBuffer(const RHIBufferDesc& desc);
```

### Error Handling & Logging

```cpp
// Module-specific logging
RVX_CORE_INFO("Initializing engine");
RVX_RHI_ERROR("Failed to create device: {}", errorMsg);

// Assertions
RVX_ASSERT(condition);                              // Fatal, always active
RVX_ASSERT_MSG(condition, "Error: {}", value);      // Fatal with message
RVX_VERIFY(condition, "Check failed");              // Non-fatal, logs error
RVX_DEBUG_ASSERT(expensiveCheck());                 // Debug-only, stripped in Release
RVX_UNREACHABLE();                                  // Mark unreachable code
```

## Key Modules

### RHI (Rendering Hardware Interface)

All GPU operations go through the `IRHIDevice` interface, providing a unified API across all rendering backends. This allows seamless switching between DX11, DX12, Vulkan, Metal, and OpenGL.

### RenderGraph

The frame graph system handles automatic resource state tracking, lifetime management, and barrier insertion. Simply define your render passes and the graph manages everything else.

### Scene System

The Runtime has one authority path: `Engine -> World -> SceneEcsRuntime`.

- **Entities and fragments** - generation-safe handles plus typed sparse-set data
- **Processors and barriers** - declared access, deterministic groups, fixed-step phases
- **Hierarchy and spatial index** - handle-only transform resolution and AABB queries
- **Frozen snapshots** - RenderExtraction never reads a live mutable Scene
- **Retirement** - Physics, Animation, Audio, feature, Render, and Resource acknowledgements

### ShaderCompiler

HLSL to SPIR-V compilation with cross-compilation to MSL (Metal) and GLSL (OpenGL) via SPIRV-Cross. Enables write-once shaders across all backends.

## Development Guidelines

- **Always initialize member variables** - Use in-class initialization
- **Close namespaces with comments** - `} // namespace RVX`
- **Use `#pragma once`** - Not traditional include guards
- **Check backend type** - When writing backend-specific code
- **Use `RVX_INVALID_INDEX`** - For invalid handle checks
- **Inherit from `NonCopyable`/`NonMovable`** - When appropriate
- **Prefer smart pointers** - `std::unique_ptr`, `std::shared_ptr`
- **Use `Ref` type aliases** - For RHI resources: `RHIBufferRef`, `RHITextureRef`

See [AGENTS.md](AGENTS.md) for detailed development guidelines and best practices.

## License

[Specify your license here]

## Contributing

[Specify contribution guidelines here]

## Roadmap

See the [Docs/](Docs/) directory for design documents and implementation plans.

## Contact

[Provide contact information here]
