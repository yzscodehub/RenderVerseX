#pragma once

#include "RHI/RHIDefinitions.h"
#include "ShaderCompiler/ShaderReflection.h"
#include <vector>
#include <string>
#include <cstdint>
#include <unordered_map>
#include <optional>

namespace RVX
{
    // =============================================================================
    // SPIRV-Cross MSL Translation Options
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
    // SPIRV-Cross MSL Translation Result
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
    // SPIRV-Cross GLSL Translation Options
    // =============================================================================
    struct SPIRVToGLSLOptions
    {
        uint32_t glslVersion = 450;          // GLSL version (450 = OpenGL 4.5)
        bool es = false;                     // OpenGL ES
        bool vulkanSemantics = false;        // Keep as false for OpenGL
        bool enable420Pack = true;           // Enable layout(binding=) support
        bool emitPushConstantAsUBO = true;   // Convert push constants to UBO
        bool forceZeroInit = true;           // Force zero initialization
    };

    // =============================================================================
    // GLSL Binding Remap Information
    // =============================================================================
    struct GLSLBindingRemap
    {
        std::string name;           // Resource name
        uint32_t originalSet;       // Original descriptor set
        uint32_t originalBinding;   // Original binding
        uint32_t glBinding;         // OpenGL binding point
        RHIBindingType type;        // Resource type
    };

    // =============================================================================
    // SPIRV-Cross GLSL Translation Result
    // =============================================================================
    struct SPIRVToGLSLResult
    {
        bool success = false;
        std::string glslSource;
        std::string errorMessage;
        ShaderReflection reflection;

        // Binding remapping table (for runtime binding)
        std::vector<GLSLBindingRemap> bindingRemaps;

        // Push constant info (if any)
        struct PushConstantInfo
        {
            std::string uboName;     // Converted UBO name
            uint32_t glBinding;      // OpenGL binding point
            uint32_t size;           // Size in bytes
        };
        std::optional<PushConstantInfo> pushConstantInfo;

        // Helper: Get GL binding from set/binding
        uint32_t GetGLBinding(uint32_t set, uint32_t binding) const
        {
            for (const auto& remap : bindingRemaps)
            {
                if (remap.originalSet == set && remap.originalBinding == binding)
                    return remap.glBinding;
            }
            return UINT32_MAX;
        }
    };

    // =============================================================================
    // SPIRV-Cross Translator
    // Converts SPIR-V bytecode to MSL or GLSL
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

        // Convert SPIR-V bytecode to GLSL source code
        SPIRVToGLSLResult TranslateToGLSL(
            const std::vector<uint8_t>& spirvBytecode,
            RHIShaderStage stage,
            const char* entryPoint,
            const SPIRVToGLSLOptions& options = {});

        // Extract reflection data from SPIR-V (without translation)
        static ShaderReflection ReflectSPIRV(
            const std::vector<uint8_t>& spirvBytecode,
            RHIShaderStage stage);
    };

} // namespace RVX
