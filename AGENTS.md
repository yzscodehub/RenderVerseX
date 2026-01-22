# AGENTS.md - RenderVerseX Development Guide

This document provides guidelines for AI coding agents working on RenderVerseX, a cross-platform multi-backend graphics rendering engine written in C++ 20.

## Project Overview

- **Language**: C++ 20
- **Build System**: CMake 3.21+
- **Package Manager**: vcpkg
- **Platforms**: Windows (DX11/DX12/Vulkan), Linux (Vulkan/OpenGL), macOS/iOS (Metal)

## Build Commands

### Configure and Build

```bash
# Configure with vcpkg toolchain
cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=[vcpkg-root]/scripts/buildsystems/vcpkg.cmake

# Build (Release)
cmake --build build --config Release

# Build (Debug)
cmake --build build --config Debug

# Build specific target
cmake --build build --config Release --target ModelViewer
```

### Build Options

```bash
-DRVX_ENABLE_DX11=ON/OFF      # DirectX 11 backend (Windows only)
-DRVX_ENABLE_DX12=ON/OFF      # DirectX 12 backend (Windows only)
-DRVX_ENABLE_VULKAN=ON/OFF    # Vulkan backend
-DRVX_ENABLE_METAL=ON/OFF     # Metal backend (macOS/iOS only)
-DRVX_ENABLE_OPENGL=ON/OFF    # OpenGL backend (Linux fallback)
-DRVX_BUILD_SAMPLES=ON/OFF    # Build sample applications
-DRVX_BUILD_TESTS=ON/OFF      # Build validation tests
```

## Test Commands

Tests are standalone executables, not using a test framework like GoogleTest.

```bash
# Run all validation tests (after building)
./build/Tests/Release/RenderGraphValidation.exe
./build/Tests/Release/DX12Validation.exe
./build/Tests/Release/VulkanValidation.exe
./build/Tests/Release/DX11Validation.exe
./build/Tests/Release/SystemIntegrationTest.exe

# Run a single test (example)
./build/Tests/Release/RenderGraphValidation.exe

# Cross-backend validation (requires DX12 and Vulkan)
./build/Tests/Release/CrossBackendValidation.exe
```

## Code Style Guidelines

### Namespace

All code resides in the `RVX` namespace:

```cpp
namespace RVX
{
    // code here
} // namespace RVX
```

### Header Guards

Use `#pragma once` (no traditional include guards):

```cpp
#pragma once

#include "Core/Types.h"
```

### Include Order

1. Corresponding header (for .cpp files)
2. Project headers with `"quotes"`
3. External library headers with `<angle brackets>`
4. Standard library headers with `<angle brackets>`

```cpp
#include "Core/Log.h"              // Project headers first
#include <spdlog/spdlog.h>         // External libraries
#include <vector>                   // Standard library
```

### Naming Conventions

| Element | Convention | Example |
|---------|------------|---------|
| Classes/Structs | PascalCase | `RenderGraph`, `SceneEntity` |
| Interfaces | `I` prefix + PascalCase | `IRHIDevice`, `ISpatialEntity` |
| Methods | PascalCase | `CreateBuffer()`, `GetName()` |
| Member variables | `m_` prefix + camelCase | `m_device`, `m_position` |
| Static members | `s_` prefix + camelCase | `s_nextHandle`, `s_coreLogger` |
| Local variables | camelCase | `bufferDesc`, `devicePtr` |
| Constants | `RVX_` prefix + SCREAMING_SNAKE | `RVX_INVALID_INDEX`, `RVX_MAX_FRAME_COUNT` |
| Enums | `enum class` PascalCase | `RHIBackendType::Vulkan` |
| Typedefs | PascalCase or lowercase | `uint32`, `RHIBufferRef` |
| Smart pointer aliases | `Ref` suffix | `RHIBufferRef`, `RHITextureRef` |

### Type Definitions

Use project type aliases from `Core/Types.h`:

```cpp
using int8   = std::int8_t;
using int16  = std::int16_t;
using int32  = std::int32_t;
using int64  = std::int64_t;
using uint8  = std::uint8_t;
using uint16 = std::uint16_t;
using uint32 = std::uint32_t;
using uint64 = std::uint64_t;
using float32 = float;
using float64 = double;
```

### Enum Classes

Always use `enum class` with explicit underlying type:

```cpp
enum class RHIBackendType : uint8
{
    None = 0,
    Auto,
    DX11,
    DX12,
    Vulkan,
    Metal,
    OpenGL
};
```

### Class Structure

Use comment section separators for organization:

```cpp
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

private:
    // Member variables
    uint32 m_value = 0;
    std::string m_name;
};
```

### Documentation

Use Doxygen-style comments for files and public APIs:

```cpp
/**
 * @file RenderGraph.h
 * @brief Frame graph and automatic resource management
 */

/**
 * @brief Create a new buffer resource
 * @param desc Buffer description
 * @return Handle to created buffer
 */
RHIBufferRef CreateBuffer(const RHIBufferDesc& desc);
```

### Error Handling

Use logging macros with module prefixes:

```cpp
// Module-specific logging
RVX_CORE_INFO("Initializing engine");
RVX_RHI_ERROR("Failed to create device: {}", errorMsg);
RVX_RENDER_WARN("Shader compilation warning");

// Assertions
RVX_ASSERT(condition);
RVX_ASSERT_MSG(condition, "Error message: {}", value);
RVX_VERIFY(condition, "Non-fatal check failed");

// Debug-only assertions
RVX_DEBUG_ASSERT(expensiveCheck());
```

### Memory Management

- Use smart pointers (`std::unique_ptr`, `std::shared_ptr`)
- Use `Ref` type aliases for RHI resources
- Inherit from `NonCopyable` or `NonMovable` when appropriate

```cpp
class MyResource : public NonCopyable
{
    // Cannot be copied
};

RHIBufferRef buffer = device->CreateBuffer(desc);
```

### Forward Declarations

Prefer forward declarations in headers to reduce compile times:

```cpp
namespace RVX
{
    // Forward declarations
    class GPUResourceManager;
    class PipelineCache;
    class RenderScene;
    
    class OpaquePass : public IRenderPass { /* ... */ };
}
```

## Module Structure

Each module follows this directory layout:

```
ModuleName/
  Include/ModuleName/     # Public headers
    ModuleName.h          # Main module header
    SubComponent.h
  Private/                # Implementation files
    SubComponent.cpp
  CMakeLists.txt
```

## Key Modules

| Module | Purpose |
|--------|---------|
| `Core` | Types, logging, assertions, math, job system |
| `RHI` | Rendering Hardware Interface abstraction |
| `RHI_*` | Backend implementations (DX11, DX12, Vulkan, Metal, OpenGL) |
| `Render` | High-level rendering (RenderGraph, Passes, SceneRenderer) |
| `Scene` | Entity-Component system, mesh, materials |
| `Resource` | Asset loading and resource management |
| `ShaderCompiler` | HLSL compilation and cross-compilation |

## Important Patterns

1. **RHI Abstraction**: All GPU operations go through `IRHIDevice` interface
2. **RenderGraph**: Use for automatic resource state tracking and barrier insertion
3. **Component System**: Extend `Component` base class for entity behaviors
4. **Subsystems**: Engine features implement `EngineSubsystem` or `WorldSubsystem`

## Common Pitfalls

- Always check `RHIBackendType` when writing backend-specific code
- Use `RVX_INVALID_INDEX` for invalid handle checks
- Member variables must be initialized (use in-class initialization)
- Close namespace with `} // namespace RVX` comment
