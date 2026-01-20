#include "ShaderCompiler/ShaderCompiler.h"
#include "SPIRVCrossTranslator.h"
#include "Core/Log.h"

#include <glslang/Public/ShaderLang.h>
#include <glslang/Public/ResourceLimits.h>
#include <glslang/SPIRV/GlslangToSpv.h>

#include <cstring>
#include <vector>

namespace RVX
{
    namespace
    {
        EShLanguage GetGlslangStage(RHIShaderStage stage)
        {
            switch (stage)
            {
                case RHIShaderStage::Vertex:   return EShLangVertex;
                case RHIShaderStage::Pixel:    return EShLangFragment;
                case RHIShaderStage::Compute:  return EShLangCompute;
                case RHIShaderStage::Geometry: return EShLangGeometry;
                case RHIShaderStage::Hull:     return EShLangTessControl;
                case RHIShaderStage::Domain:   return EShLangTessEvaluation;
                default: return EShLangVertex;
            }
        }

        class GlslangInitializer
        {
        public:
            GlslangInitializer()
            {
                glslang::InitializeProcess();
            }
            ~GlslangInitializer()
            {
                glslang::FinalizeProcess();
            }
        };

        // Ensure glslang is initialized once
        GlslangInitializer& GetGlslangInit()
        {
            static GlslangInitializer init;
            return init;
        }
    }

    // =============================================================================
    // Apple Shader Compiler using glslang
    // Uses glslang for HLSL -> SPIR-V, then SPIRV-Cross for SPIR-V -> MSL
    // =============================================================================
    class AppleGlslangShaderCompiler final : public IShaderCompiler
    {
    public:
        AppleGlslangShaderCompiler()
        {
            // Ensure glslang is initialized
            GetGlslangInit();
            RVX_CORE_INFO("Apple glslang Shader Compiler initialized");
        }

        ~AppleGlslangShaderCompiler() = default;

        ShaderCompileResult Compile(const ShaderCompileOptions& options) override
        {
            ShaderCompileResult result;

            if (!options.sourceCode || !options.entryPoint)
            {
                result.errorMessage = "Missing shader source or entry point";
                return result;
            }

            // Step 1: Compile HLSL to SPIR-V using glslang
            std::vector<uint8_t> spirvBytecode;
            std::string compileError;
            
            if (!CompileHLSLToSPIRV(options, spirvBytecode, compileError))
            {
                result.errorMessage = compileError;
                return result;
            }
            
            RVX_CORE_DEBUG("glslang: Compiled HLSL to {} bytes of SPIR-V", spirvBytecode.size());

            // For Vulkan backend, return SPIR-V directly
            if (options.targetBackend == RHIBackendType::Vulkan)
            {
                result.success = true;
                result.bytecode = std::move(spirvBytecode);
                return result;
            }

            // Step 2: For Metal backend, translate SPIR-V to MSL
            if (options.targetBackend == RHIBackendType::Metal)
            {
                SPIRVCrossTranslator translator;
                SPIRVToMSLOptions mslOptions;
                mslOptions.mslVersionMajor = 2;
                mslOptions.mslVersionMinor = 1;
                mslOptions.useArgumentBuffers = false;
                mslOptions.iOS = false;

                auto mslResult = translator.TranslateToMSL(
                    spirvBytecode,
                    options.stage,
                    options.entryPoint,
                    mslOptions);

                if (!mslResult.success)
                {
                    result.errorMessage = mslResult.errorMessage;
                    return result;
                }

                result.success = true;
                result.bytecode.resize(mslResult.mslSource.size());
                std::memcpy(result.bytecode.data(), mslResult.mslSource.data(), mslResult.mslSource.size());
                result.mslSource = std::move(mslResult.mslSource);
                result.mslEntryPoint = std::move(mslResult.entryPointName);
                result.reflection = std::move(mslResult.reflection);
                return result;
            }

            result.errorMessage = "Unsupported target backend for Apple platform";
            return result;
        }

    private:
        bool CompileHLSLToSPIRV(const ShaderCompileOptions& options,
                                std::vector<uint8_t>& outSPIRV,
                                std::string& outError)
        {
            EShLanguage stage = GetGlslangStage(options.stage);
            glslang::TShader shader(stage);

            // Set source
            const char* sourceStrings[] = { options.sourceCode };
            const int sourceLengths[] = { static_cast<int>(strlen(options.sourceCode)) };
            const char* sourceNames[] = { options.sourcePath ? options.sourcePath : "shader" };
            
            shader.setStringsWithLengthsAndNames(sourceStrings, sourceLengths, sourceNames, 1);

            // Set entry point and source type
            shader.setEntryPoint(options.entryPoint);
            shader.setSourceEntryPoint(options.entryPoint);
            shader.setEnvInput(glslang::EShSourceHlsl, stage, glslang::EShClientVulkan, 100);
            shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_2);
            shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_5);

            // Set up preamble with defines
            std::string preamble;
            for (const auto& def : options.defines)
            {
                preamble += "#define " + def.name + " " + def.value + "\n";
            }
            if (!preamble.empty())
            {
                shader.setPreamble(preamble.c_str());
            }

            // Parse
            TBuiltInResource resources = *GetDefaultResources();
            EShMessages messages = static_cast<EShMessages>(
                EShMsgSpvRules | EShMsgVulkanRules | EShMsgReadHlsl);

            if (!shader.parse(&resources, 100, false, messages))
            {
                outError = "HLSL parse error: ";
                outError += shader.getInfoLog();
                return false;
            }

            // Link
            glslang::TProgram program;
            program.addShader(&shader);

            if (!program.link(messages))
            {
                outError = "HLSL link error: ";
                outError += program.getInfoLog();
                return false;
            }

            // Generate SPIR-V
            std::vector<uint32_t> spirv;
            spv::SpvBuildLogger logger;
            glslang::SpvOptions spvOptions;
            spvOptions.generateDebugInfo = options.enableDebugInfo;
            spvOptions.disableOptimizer = !options.enableOptimization;
            spvOptions.optimizeSize = false;

            glslang::GlslangToSpv(*program.getIntermediate(stage), spirv, &logger, &spvOptions);

            if (spirv.empty())
            {
                outError = "SPIR-V generation failed";
                return false;
            }

            // Convert to bytes
            outSPIRV.resize(spirv.size() * sizeof(uint32_t));
            std::memcpy(outSPIRV.data(), spirv.data(), outSPIRV.size());

            RVX_CORE_DEBUG("glslang: Compiled HLSL to {} bytes of SPIR-V", outSPIRV.size());
            return true;
        }
    };

    std::unique_ptr<IShaderCompiler> CreateDXCShaderCompiler()
    {
        return std::make_unique<AppleGlslangShaderCompiler>();
    }

} // namespace RVX
