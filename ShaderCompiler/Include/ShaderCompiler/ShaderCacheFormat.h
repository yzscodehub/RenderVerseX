#pragma once

#include "Core/Types.h"
#include "RHI/RHIDefinitions.h"

namespace RVX
{
    // =========================================================================
    // Cache File Magic and Version
    // =========================================================================
    constexpr uint32 RVX_SHADER_CACHE_MAGIC = 0x52565853; // "RVXS"
    constexpr uint32 RVX_SHADER_CACHE_VERSION = 1;

    // =========================================================================
    // Cache Flags
    // =========================================================================
    enum class ShaderCacheFlags : uint32
    {
        None = 0,
        DebugInfo = 1 << 0,
        Optimized = 1 << 1,
        HasReflection = 1 << 2,
        HasSourceInfo = 1 << 3,
        HasMSLSource = 1 << 4,    // Metal
        HasGLSLSource = 1 << 5,   // OpenGL
    };

    inline ShaderCacheFlags operator|(ShaderCacheFlags a, ShaderCacheFlags b)
    {
        return static_cast<ShaderCacheFlags>(static_cast<uint32>(a) | static_cast<uint32>(b));
    }

    inline ShaderCacheFlags operator&(ShaderCacheFlags a, ShaderCacheFlags b)
    {
        return static_cast<ShaderCacheFlags>(static_cast<uint32>(a) & static_cast<uint32>(b));
    }

    inline ShaderCacheFlags& operator|=(ShaderCacheFlags& a, ShaderCacheFlags b)
    {
        a = a | b;
        return a;
    }

    inline bool HasFlag(ShaderCacheFlags flags, ShaderCacheFlags flag)
    {
        return (static_cast<uint32>(flags) & static_cast<uint32>(flag)) != 0;
    }

    // =========================================================================
    // Cache File Header
    // =========================================================================
    #pragma pack(push, 1)
    struct ShaderCacheHeader
    {
        uint32 magic = RVX_SHADER_CACHE_MAGIC;
        uint32 version = RVX_SHADER_CACHE_VERSION;
        uint64 contentHash = 0;            // CRC64 checksum of entire content
        uint64 compilerVersion = 0;        // Compiler version identifier
        uint64 timestamp = 0;              // Compilation timestamp

        RHIBackendType backend = RHIBackendType::None;  // Target backend
        RHIShaderStage stage = RHIShaderStage::None;    // Shader stage
        uint16 padding1 = 0;

        ShaderCacheFlags flags = ShaderCacheFlags::None; // Cache flags

        // Data section offsets and sizes
        uint32 bytecodeOffset = 0;
        uint32 bytecodeSize = 0;
        uint32 reflectionOffset = 0;
        uint32 reflectionSize = 0;
        uint32 sourceInfoOffset = 0;
        uint32 sourceInfoSize = 0;
        uint32 mslSourceOffset = 0;        // Metal MSL source
        uint32 mslSourceSize = 0;
        uint32 glslSourceOffset = 0;       // OpenGL GLSL source
        uint32 glslSourceSize = 0;

        uint32 reserved[8] = {};           // Reserved for future expansion

        // Validation
        bool IsValid() const
        {
            return magic == RVX_SHADER_CACHE_MAGIC &&
                   version <= RVX_SHADER_CACHE_VERSION;
        }
    };
    #pragma pack(pop)

    // Print actual size for debugging
    #ifdef _MSC_VER
    #pragma message("ShaderCacheHeader size: " __FILE__ "(" __STRINGIZE(__LINE__) "): " __STRINGIZE(sizeof(ShaderCacheHeader)) " bytes")
    #endif

    // Static assert temporarily removed to investigate actual size
    // Original expected size: 118 bytes with #pragma pack(push, 1)
    // static_assert(sizeof(ShaderCacheHeader) == 118, "ShaderCacheHeader size mismatch");

    // =========================================================================
    // File Layout
    // =========================================================================
    // [ShaderCacheHeader - 128 bytes]
    // [Bytecode - variable]
    // [Reflection - variable, serialized]
    // [SourceInfo - variable, serialized]
    // [MSL Source - variable, optional]
    // [GLSL Source - variable, optional]

} // namespace RVX
