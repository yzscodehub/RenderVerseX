#pragma once

#include "RHI/RHIDefinitions.h"
#include <vector>
#include <string>

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
