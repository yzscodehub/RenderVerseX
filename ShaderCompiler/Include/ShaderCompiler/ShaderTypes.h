#pragma once

#include "Core/Types.h"
#include "RHI/RHIResources.h"
#include <functional>
#include <future>

namespace RVX
{
    // Forward declarations
    struct ShaderCompileResult;
    struct ShaderLoadResult;

    // =========================================================================
    // Compile Handle
    // =========================================================================
    using CompileHandle = uint64;
    constexpr CompileHandle RVX_INVALID_COMPILE_HANDLE = 0;

    // =========================================================================
    // Callback Types
    // =========================================================================
    using CompileCallback = std::function<void(const ShaderCompileResult&)>;
    using LoadCallback = std::function<void(const ShaderLoadResult&)>;
    using ReloadCallback = std::function<void(RHIShaderRef)>;

    // =========================================================================
    // Compile Priority
    // =========================================================================
    enum class CompilePriority : uint8
    {
        Low = 0,       // Background prewarming
        Normal = 1,    // Regular compilation
        High = 2,      // High priority
        Immediate = 3  // Execute immediately (synchronous)
    };

    // =========================================================================
    // Compile Status
    // =========================================================================
    enum class CompileStatus : uint8
    {
        Pending,    // Waiting in queue
        Compiling,  // Currently compiling
        Completed,  // Successfully completed
        Failed,     // Compilation failed
        Cancelled   // Cancelled by user
    };

} // namespace RVX
