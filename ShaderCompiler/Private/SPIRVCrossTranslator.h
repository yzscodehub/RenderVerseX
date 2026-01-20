#pragma once

#include "RHI/RHIDefinitions.h"
#include "ShaderCompiler/ShaderReflection.h"
#include <vector>
#include <string>
#include <cstdint>

namespace RVX
{
    // =============================================================================
    // SPIRV-Cross Translation Options
    // =============================================================================
    struct SPIRVToMSLOptions
    {
        uint32_t mslVersionMajor = 2;
        uint32_t mslVersionMinor = 1;
        bool useArgumentBuffers = false;
        bool enablePointSizeBuiltin = false;
        bool iOS = false;
    };

    // =============================================================================
    // SPIRV-Cross Translation Result
    // =============================================================================
    struct SPIRVToMSLResult
    {
        bool success = false;
        std::string mslSource;
        std::string errorMessage;
        std::string entryPointName;  // Metal entry point name (may differ from HLSL)
        ShaderReflection reflection; // Extracted reflection data
    };

    // =============================================================================
    // SPIRV-Cross Translator
    // Converts SPIR-V bytecode to Metal Shading Language (MSL)
    // =============================================================================
    class SPIRVCrossTranslator
    {
    public:
        SPIRVCrossTranslator() = default;
        ~SPIRVCrossTranslator() = default;

        // Convert SPIR-V bytecode to MSL source code
        SPIRVToMSLResult TranslateToMSL(
            const std::vector<uint8_t>& spirvBytecode,
            RHIShaderStage stage,
            const char* entryPoint,
            const SPIRVToMSLOptions& options = {});

        // Extract reflection data from SPIR-V (without translation)
        static ShaderReflection ReflectSPIRV(
            const std::vector<uint8_t>& spirvBytecode,
            RHIShaderStage stage);
    };

} // namespace RVX
