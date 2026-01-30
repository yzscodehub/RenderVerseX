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
cmake --build --preset debug --target Triangle
cmake --build --preset debug --target Cube3D
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
cmake --build build --config Release --target ModelViewer
```

### Build Options

| Option | Default | Description |
|--------|---------|-------------|
| `-DRVX_ENABLE_DX11=ON/OFF` | ON | DirectX 11 backend (Windows only) |
| `-DRVX_ENABLE_DX12=ON/OFF` | ON | DirectX 12 backend (Windows only) |
| `-DRVX_ENABLE_VULKAN=ON/OFF` | ON | Vulkan backend |
| `-DRVX_ENABLE_METAL=ON/OFF` | OFF | Metal backend (macOS/iOS only) |
| `-DRVX_ENABLE_OPENGL=ON/OFF` | OFF | OpenGL backend |
| `-DRVX_BUILD_SAMPLES=ON/OFF` | ON | Build sample applications |
| `-DRVX_BUILD_TESTS=ON/OFF` | ON | Build validation tests |

## Running Tests

Tests are standalone executables (no GoogleTest framework).

```bash
# Run individual validation tests
./build/Tests/Release/RenderGraphValidation.exe
./build/Tests/Release/DX12Validation.exe
./build/Tests/Release/VulkanValidation.exe
./build/Tests/Release/DX11Validation.exe
./build/Tests/Release/SystemIntegrationTest.exe

# Cross-backend validation (requires multiple backends)
./build/Tests/Release/CrossBackendValidation.exe
```

## Sample Applications

| Sample | Description |
|--------|-------------|
| **Triangle** | Basic triangle rendering (getting started) |
| **Cube3D** | 3D cube with lighting and camera controls |
| **TexturedQuad** | Textured quad with shader support |
| **ModelViewer** | Full-featured glTF model viewer |
| **ComputeDemo** | GPU compute shader demonstration |

## Project Structure

```
RenderVerseX/
├── Core/              # Core utilities (Types, Log, Assert, Math, Job System)
├── Geometry/          # Geometry primitives, spatial queries
├── HAL/               # Hardware Abstraction Layer (Window, Input)
├── Spatial/           # Spatial partitioning and queries
├── Scene/             # Entity-Component system, scene management
├── Animation/         # Animation system
├── Runtime/           # Runtime subsystems (Camera, etc.)
├── Picking/           # Object picking and raycasting
├── World/             # World management (integrates Scene, Spatial, Picking)
├── Engine/            # Engine initialization and main loop
├── RHI/               # Rendering Hardware Interface abstraction
├── RHI_DX11/          # DirectX 11 backend
├── RHI_DX12/          # DirectX 12 backend
├── RHI_Vulkan/        # Vulkan backend
├── RHI_Metal/         # Metal backend (macOS/iOS)
├── RHI_OpenGL/        # OpenGL backend (Linux fallback)
├── ShaderCompiler/    # HLSL compilation and cross-compilation
├── Render/            # High-level rendering (RenderGraph, Passes, SceneRenderer)
├── Resource/          # Asset loading and resource management
├── Particle/          # GPU-driven particle system
├── AI/                # AI systems (Navigation, BehaviorTree, Perception)
├── Networking/        # Network communication and replication
├── Debug/             # Debug tools (profiling, console, stats)
├── Scripting/         # Lua scripting system with Sol2 bindings
├── Audio/             # Audio subsystem (MiniAudio)
├── Tools/             # Asset pipeline and importers
├── UI/                # UI framework with widgets
├── Physics/           # Physics subsystem (Jolt Physics)
├── Terrain/           # Heightmap terrain rendering
├── Water/             # Water simulation and effects
└── Editor/            # Editor application with ImGui

Samples/               # Sample applications
Tests/                 # Validation tests
Docs/                  # Design documents and plans
```

## Code Style Guidelines

### Naming Conventions

| Element | Convention | Example |
|---------|------------|---------|
| Classes/Structs | PascalCase | `RenderGraph`, `SceneEntity` |
| Interfaces | `I` prefix + PascalCase | `IRHIDevice`, `ISpatialEntity` |
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
#include "Scene/SceneEntity.h"  // Corresponding header first
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

Entity-Component architecture with:
- **Components** - Transform, Mesh, Material, Light, etc.
- **Spatial Queries** - AABB/Occlusion culling, raycasting
- **Picking** - Mouse selection and object interaction
- **World Management** - Multi-world support with subsystems

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
