# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build System & Commands

### Prerequisites
- CMake 3.21+
- C++20 compatible compiler (VS2022, GCC11+, Clang13+)
- vcpkg for dependency management

### CMake Presets Configuration
The project uses CMakePresets.json for simplified cross-platform configuration. Presets are configured for Windows and macOS with appropriate backend defaults:

#### Available Presets
- `debug` - Windows Debug build (DX11/DX12/Vulkan/OpenGL enabled)
- `release` - Windows Release build
- `macos-debug` - macOS Debug build (Metal enabled)
- `macos-release` - macOS Release build

#### Environment Setup
Presets expect VCPKG_ROOT environment variable:
```bash
# Windows
set VCPKG_ROOT=E:/WorkSpace/vcpkg

# Linux/macOS
export VCPKG_ROOT=/path/to/vcpkg
```

### Configure Project

#### Using CMake Presets (Recommended)
The project includes CMakePresets.json for easy configuration:

```bash
# Configure and build using presets
cmake --preset debug      # Debug configuration
cmake --preset release    # Release configuration

# macOS presets
cmake --preset macos-debug   # Debug on macOS
cmake --preset macos-release # Release on macOS
```

#### Manual Configuration
```bash
# Configure with vcpkg toolchain
cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=[vcpkg-root]/scripts/buildsystems/vcpkg.cmake

# Configure with specific backends
cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=[vcpkg-path]/scripts/buildsystems/vcpkg.cmake \
    -DRVX_ENABLE_DX12=ON -DRVX_ENABLE_VULKAN=ON -DRVX_BUILD_TESTS=OFF
```

### Build Commands

#### Using Build Presets
```bash
# Build using presets (automatically uses configured preset)
cmake --build --preset debug      # Build Debug configuration
cmake --build --preset release    # Build Release configuration

# Build specific targets with presets
cmake --build --preset debug --target ModelViewer
cmake --build --preset debug --target Triangle
cmake --build --preset debug --target Cube3D
```

#### Manual Build
```bash
# Build all targets
cmake --build build --config Release

# Build specific application/sample
cmake --build build --config Release --target ModelViewer
cmake --build build --config Release --target Triangle
cmake --build build --config Release --target Cube3D

# Build Debug configuration
cmake --build build --config Debug
```

### Running Tests
Tests are standalone executables (no GoogleTest framework):
```bash
# Run individual validation tests
./build/Tests/Release/RenderGraphValidation.exe
./build/Tests/Release/DX12Validation.exe
./build/Tests/Release/VulkanValidation.exe
./build/Tests/Release/SystemIntegrationTest.exe

# Cross-backend validation (requires DX12 and Vulkan)
./build/Tests/Release/CrossBackendValidation.exe
```

## Architecture Overview

### Multi-Layer Architecture
RenderVerseX is organized in distinct layers from bottom to top:

1. **Core Foundation** - Types, logging, math, job system, memory management (`Core/`)
2. **Hardware Abstraction Layer** - Window, input handling (`HAL/`)
3. **RHI Layer** - Rendering Hardware Interface abstraction (`RHI/`)
4. **Resource Layer** - Asset loading and management (`Resource/`)
5. **Runtime Layer** - Core services (time, input, camera) (`Runtime/`)
6. **World Layer** - Scene management, spatial queries (`World/`, `Scene/`, `Spatial/`)
7. **Render Layer** - High-level rendering with RenderGraph (`Render/`)
8. **Feature Modules** - Physics, AI, Audio, UI, etc.
9. **Application Layer** - Engine coordination and samples

### Key Architectural Patterns

#### RHI Abstraction Layer
All GPU operations go through the `IRHIDevice` interface, providing a unified API across DirectX 11/12, Vulkan, Metal, and OpenGL. Backend-specific code is isolated in `RHI_DX11/`, `RHI_DX12/`, etc.

```cpp
// Example: Create buffer through RHI abstraction
RHIBufferRef buffer = device->CreateBuffer(bufferDesc);
```

#### RenderGraph System
The frame graph handles automatic resource state tracking, lifetime management, and barrier insertion. Define render passes and let the graph manage everything else.

#### Entity-Component System
Scene entities use components for functionality:
- Inherit from `Component` base class
- Use `ComponentFactory` for registration
- Managed by `SceneEntity` system

#### Subsystem Architecture
Engine features implement either:
- `EngineSubsystem` - Global engine services (Input, Render, Scripting)
- `WorldSubsystem` - Per-world services (Physics, AI)

## Module Dependencies

### Core Module Dependencies
```
Engine → [Core, HAL, Runtime, World, Render, Resource]
Render → [Core, RHI, Runtime, Scene, Resource]
Scene → [Core, Geometry, Spatial]
World → [Scene, Spatial, Picking]
```

### Critical Module Interactions
- **ShaderCompiler** → Converts HLSL to SPIR-V, then to MSL/GLSL via SPIRV-Cross
- **Resource** → Manages asset lifetime and loading through `ResourceCache`
- **RenderGraph** → Manages render passes and automatic resource barriers
- **Physics** → Integrates Jolt Physics with `World` and `Scene` systems

## Code Conventions

### Namespace & Structure
- All code in `RVX` namespace with explicit closing comment
- `#pragma once` for header guards (no traditional guards)
- Comment separators between sections: `// =========================================================================`

```cpp
#pragma once
#include "Core/Types.h"

namespace RVX
{
    class MyClass
    {
    public:
        // =========================================================================
        // Construction
        // =========================================================================
        MyClass();
        ~MyClass();

        // =========================================================================
        // Public Methods
        // =========================================================================
        void DoSomething();
    };
} // namespace RVX
```

### Naming Conventions
- Classes/Structs: PascalCase (`RenderGraph`, `SceneEntity`)
- Interfaces: `I` prefix + PascalCase (`IRHIDevice`)
- Members: `m_` prefix + camelCase (`m_device`)
- Static members: `s_` prefix + camelCase (`s_nextHandle`)
- Constants: `RVX_` prefix + SCREAMING_SNAKE (`RVX_INVALID_INDEX`)

### Type System
Use Core/Types.h aliases:
```cpp
using int8 = std::int8_t;     using uint8  = std::uint8_t;
using int16 = std::int16_t;   using uint16 = std::uint16_t;
using int32 = std::int32_t;   using uint32 = std::uint32_t;
using int64 = std::int64_t;   using uint64 = std::uint64_t;
using float32 = float;        using float64 = double;
```

### Error Handling & Logging
- Module-specific logging: `RVX_CORE_INFO`, `RVX_RHI_ERROR`, etc.
- Assertions: `RVX_ASSERT`, `RVX_VERIFY`, `RVX_DEBUG_ASSERT`
- Always check for invalid handles using `RVX_INVALID_INDEX`

```cpp
RVX_CORE_INFO("Initializing engine");
RVX_RHI_ERROR("Failed to create device: {}", errorMsg);
RVX_ASSERT(handle != RVX_INVALID_INDEX);
```

### Resource Management
- Use `Ref` type aliases for RHI resources: `RHIBufferRef`, `RHITextureRef`
- Smart pointers for ownership: `std::unique_ptr`, `std::shared_ptr`
- Inherit from `NonCopyable`/`NonMovable` when appropriate

## Shader Development

### Shader Compilation Pipeline
1. Write HLSL shaders in `Render/Shaders/`, `Particle/Shaders/`, etc.
2. ShaderCompiler compiles HLSL to SPIR-V using DXC
3. SPIRV-Cross translates SPIR-V to MSL (Metal) and GLSL (OpenGL)
4. Caching system stores compiled bytecode and reflection data

### Shader File Locations
- Render shaders: `Render/Shaders/`
- Particle shaders: `Particle/Shaders/`
- Water shaders: `Water/Shaders/`
- Terrain shaders: `Terrain/Shaders/`

## Development Guidelines

### Adding New Modules
1. Create module directory with `Include/` and `Private/` subdirectories
2. Add CMakeLists.txt with proper target dependencies
3. Register with Engine in `Engine/Engine.cpp`
4. Follow naming conventions and documentation style

### Working with Subsystems
- Engine subsystems inherit from `EngineSubsystem`
- Implement required lifecycle methods: `Initialize()`, `Deinitialize()`, `Tick()`
- Use `GetName()` to identify subsystem type
- Configure with `Config` structs

### Common Pitfalls
- Always check `RHIBackendType` before backend-specific code
- Use `#include "Core/Types.h"` for basic types
- Member variables must be initialized (use in-class initialization)
- Close namespace with `} // namespace RVX` comment
- Use forward declarations to reduce compile times

### Testing & Validation
- Run backend-specific validation tests after changes
- Use `RenderGraphValidation` for frame graph issues
- Test cross-backend compatibility when changing RHI code
- Validation tests are in `Tests/` directory