#pragma once

#include "RHI/RHIDefinitions.h"
#include "ShaderCompiler/ShaderReflection.h"
#include <vector>
#include <string>
#include <unordered_map>
#include <optional>

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
    // Shader Compiler Interface
    // =============================================================================
    class IShaderCompiler
    {
    public:
        virtual ~IShaderCompiler() = default;

        virtual ShaderCompileResult Compile(const ShaderCompileOptions& options) = 0;
    };

    // =============================================================================
    // Factory
    // =============================================================================
    std::unique_ptr<IShaderCompiler> CreateShaderCompiler();

} // namespace RVX
