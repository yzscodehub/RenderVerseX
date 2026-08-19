#pragma once

#include "RHI/RHIDefinitions.h"
#include "ShaderCompiler/ShaderReflection.h"
#include "ShaderCompiler/ShaderSourceInfo.h"

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace RVX
{
    // =============================================================================
    // Shader Macro
    // =============================================================================
    struct ShaderMacro
    {
        std::string name;
        std::string value;

        bool operator==(const ShaderMacro& other) const
        {
            return name == other.name && value == other.value;
        }
    };

    // =============================================================================
    // Shader Compile Options
    // =============================================================================
    struct ShaderCompileOptions
    {
        RHIShaderStage stage = RHIShaderStage::None;
        const char* entryPoint = "main";
        const char* sourceCode = nullptr;
        const char* sourcePath = nullptr;
        const char* targetProfile = nullptr;
        std::vector<ShaderMacro> defines;
        RHIBackendType targetBackend = RHIBackendType::DX12;
        bool enableDebugInfo = false;
        bool enableOptimization = true;
    };

    /** @brief Backend-neutral optimization policy selected for a shader compile. */
    enum class ShaderOptimizationMode : uint8
    {
        Disabled = 0,
        Level3,
    };

    /**
     * @brief Resolve optimization independently from debug-information emission.
     *
     * Debug information must not silently change optimization semantics. Every
     * backend consumes this policy so identical compile options select the same
     * optimization level for DXIL, SPIR-V, and compatibility bytecode.
     */
    [[nodiscard]] constexpr ShaderOptimizationMode ResolveShaderOptimizationMode(
        const ShaderCompileOptions& options) noexcept
    {
        return options.enableOptimization
            ? ShaderOptimizationMode::Level3
            : ShaderOptimizationMode::Disabled;
    }

    // =============================================================================
    // Shader Compile Result
    // =============================================================================
    struct ShaderCompileResult
    {
        bool success = false;
        std::vector<uint8> bytecode;
        std::string errorMessage;
        uint64 permutationHash = 0;
        ShaderReflection reflection;
        ShaderSourceInfo sourceInfo;

        // Metal-specific: MSL source and entry point (when targeting Metal backend)
        std::string mslSource;
        std::string mslEntryPoint;

        // OpenGL-specific: GLSL source and binding info
        std::string glslSource;
        uint32 glslVersion = 450;

        // GLSL binding info: maps (set, binding) to OpenGL binding point
        struct GLSLBindingInfo
        {
            std::unordered_map<std::string, uint32> uboBindings;
            std::unordered_map<std::string, uint32> ssboBindings;
            std::unordered_map<std::string, uint32> textureBindings;
            std::unordered_map<std::string, uint32> samplerBindings;
            std::unordered_map<std::string, uint32> imageBindings;

            // Combined key: (set << 16) | binding
            std::unordered_map<uint32, uint32> setBindingToGLBinding;

            static uint32 MakeKey(uint32 set, uint32 binding)
            {
                return (set << 16) | (binding & 0xFFFF);
            }

            uint32 GetGLBinding(uint32 set, uint32 binding) const
            {
                auto it = setBindingToGLBinding.find(MakeKey(set, binding));
                return it != setBindingToGLBinding.end() ? it->second : UINT32_MAX;
            }
        } glslBindings;

        // Push constant info for OpenGL
        struct GLSLPushConstant
        {
            uint32 glBinding = 0;    // OpenGL UBO binding point
            uint32 size = 0;         // Size in bytes
        };
        std::optional<GLSLPushConstant> glslPushConstant;
    };

    // =============================================================================
    // Shader Compiler Capability
    // =============================================================================
    /** @brief Stable classification for shader compiler capability queries. */
    enum class ShaderCompileSupportCode : uint8
    {
        Supported = 0,
        RuntimeCompilerUnavailable,
        BackendUnsupported,
        StageUnsupported,
    };

    /** @brief Result of querying a compiler for a backend/stage combination. */
    struct ShaderCompileSupport
    {
        ShaderCompileSupportCode code = ShaderCompileSupportCode::RuntimeCompilerUnavailable;
        std::string reason;

        [[nodiscard]] bool IsSupported() const
        {
            return code == ShaderCompileSupportCode::Supported;
        }

        static ShaderCompileSupport Supported()
        {
            return {ShaderCompileSupportCode::Supported, {}};
        }
    };

    // =============================================================================
    // Shader Compiler Interface
    // =============================================================================
    class IShaderCompiler
    {
    public:
        virtual ~IShaderCompiler() = default;

        /** @brief Query support without attempting compilation. */
        [[nodiscard]] virtual ShaderCompileSupport QuerySupport(
            const ShaderCompileOptions& options) const = 0;
        virtual ShaderCompileResult Compile(const ShaderCompileOptions& options) = 0;
    };

    // =============================================================================
    // Factory
    // =============================================================================
    std::unique_ptr<IShaderCompiler> CreateShaderCompiler();

} // namespace RVX
