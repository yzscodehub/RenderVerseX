#include "ShaderCompiler/ShaderCompiler.h"
#include "SPIRVCrossTranslator.h"
#include "Core/Log.h"

#include <glslang/Public/ShaderLang.h>
#include <glslang/Public/ResourceLimits.h>
#include <glslang/SPIRV/GlslangToSpv.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
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

        class ShaderFileIncluder final : public glslang::TShader::Includer
        {
        public:
            explicit ShaderFileIncluder(const char* sourcePath)
            {
                if (!sourcePath || !sourcePath[0])
                {
                    return;
                }

                std::error_code ec;
                const std::filesystem::path requestedPath(sourcePath);
                if (!std::filesystem::exists(requestedPath, ec) || ec)
                {
                    return;
                }

                m_sourcePath = std::filesystem::weakly_canonical(requestedPath, ec);
                if (ec)
                {
                    m_sourcePath.clear();
                    return;
                }

                m_includeRoot = m_sourcePath.parent_path();
                for (std::filesystem::path candidate = m_includeRoot;
                     !candidate.empty();
                     candidate = candidate.parent_path())
                {
                    if (candidate.filename() == "Shaders")
                    {
                        m_includeRoot = candidate;
                        break;
                    }

                    if (candidate == candidate.root_path())
                    {
                        break;
                    }
                }
            }

            IncludeResult* includeLocal(
                const char* headerName,
                const char* includerName,
                size_t) override
            {
                if (!headerName || !headerName[0] || m_includeRoot.empty())
                {
                    return nullptr;
                }

                if (includerName && includerName[0])
                {
                    const std::filesystem::path includerPath(includerName);
                    if (IncludeResult* result = ReadFile(
                            includerPath.parent_path() / headerName))
                    {
                        return result;
                    }
                }

                return ReadFile(m_includeRoot / headerName);
            }

            IncludeResult* includeSystem(
                const char* headerName,
                const char*,
                size_t) override
            {
                if (!headerName || !headerName[0] || m_includeRoot.empty())
                {
                    return nullptr;
                }

                return ReadFile(m_includeRoot / headerName);
            }

            void releaseInclude(IncludeResult* result) override
            {
                if (!result)
                {
                    return;
                }

                auto* contents = static_cast<IncludeContents*>(result->userData);
                delete result;
                delete contents;
            }

            const std::filesystem::path& GetSourcePath() const
            {
                return m_sourcePath;
            }

        private:
            struct IncludeContents
            {
                std::string text;
            };

            bool IsInsideIncludeRoot(
                const std::filesystem::path& candidate) const
            {
                std::error_code ec;
                const std::filesystem::path relative =
                    std::filesystem::relative(candidate, m_includeRoot, ec);
                if (ec || relative.empty())
                {
                    return false;
                }

                for (const auto& component : relative)
                {
                    if (component == "..")
                    {
                        return false;
                    }
                }
                return true;
            }

            IncludeResult* ReadFile(const std::filesystem::path& requestedPath)
            {
                std::error_code ec;
                const std::filesystem::path resolvedPath =
                    std::filesystem::weakly_canonical(requestedPath, ec);
                if (ec || !IsInsideIncludeRoot(resolvedPath) ||
                    !std::filesystem::is_regular_file(resolvedPath, ec) || ec)
                {
                    return nullptr;
                }

                std::ifstream file(
                    resolvedPath,
                    std::ios::binary | std::ios::ate);
                if (!file)
                {
                    return nullptr;
                }

                const std::streamoff length = file.tellg();
                if (length < 0)
                {
                    return nullptr;
                }

                auto* contents = new IncludeContents();
                contents->text.resize(static_cast<size_t>(length));
                file.seekg(0, std::ios::beg);
                if (length > 0 &&
                    !file.read(
                        contents->text.data(),
                        static_cast<std::streamsize>(length)))
                {
                    delete contents;
                    return nullptr;
                }

                return new IncludeResult(
                    resolvedPath.generic_string(),
                    contents->text.data(),
                    contents->text.size(),
                    contents);
            }

            std::filesystem::path m_sourcePath;
            std::filesystem::path m_includeRoot;
        };
    }

    // =============================================================================
    // Portable non-Windows shader compiler using glslang. It emits SPIR-V directly for
    // Vulkan and, on Apple platforms, translates SPIR-V to MSL.
    // =============================================================================
    class GlslangShaderCompiler final : public IShaderCompiler
    {
    public:
        GlslangShaderCompiler()
        {
            // Ensure glslang is initialized
            GetGlslangInit();
            RVX_CORE_INFO("glslang shader compiler initialized");
        }

        ~GlslangShaderCompiler() = default;

        ShaderCompileSupport QuerySupport(
            const ShaderCompileOptions& options) const override
        {
            if (options.targetBackend != RHIBackendType::Vulkan
#if defined(__APPLE__)
                && options.targetBackend != RHIBackendType::Metal
#endif
                )
            {
                return {
                    ShaderCompileSupportCode::BackendUnsupported,
                    "glslang shader compiler does not support the requested backend"};
            }

            switch (options.stage)
            {
                case RHIShaderStage::Vertex:
                case RHIShaderStage::Pixel:
                case RHIShaderStage::Compute:
                case RHIShaderStage::Geometry:
                case RHIShaderStage::Hull:
                case RHIShaderStage::Domain:
                    return ShaderCompileSupport::Supported();

                default:
                    return {
                        ShaderCompileSupportCode::StageUnsupported,
                        "glslang shader compiler does not support the requested shader stage"};
            }
        }

        ShaderCompileResult Compile(const ShaderCompileOptions& options) override
        {
            ShaderCompileResult result;

            if (!options.sourceCode || !options.entryPoint)
            {
                result.errorMessage = "Missing shader source or entry point";
                return result;
            }

            const ShaderCompileSupport support = QuerySupport(options);
            if (!support.IsSupported())
            {
                result.errorMessage = support.reason;
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
                SPIRVCrossTranslator translator;
                result.reflection = translator.ReflectSPIRV(
                    spirvBytecode,
                    options.stage);
                if (!result.reflection.valid)
                {
                    result.errorMessage = "SPIR-V reflection failed";
                    return result;
                }
                result.success = true;
                result.bytecode = std::move(spirvBytecode);
                return result;
            }

            // Step 2: For Metal backend, translate SPIR-V to MSL.
#if defined(__APPLE__)
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
#endif

            result.errorMessage = "Unsupported target backend for glslang compiler";
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
            ShaderFileIncluder includer(options.sourcePath);
            const std::string sourceName = includer.GetSourcePath().empty()
                ? (options.sourcePath ? options.sourcePath : "shader")
                : includer.GetSourcePath().generic_string();
            const char* sourceNames[] = { sourceName.c_str() };

            shader.setStringsWithLengthsAndNames(sourceStrings, sourceLengths, sourceNames, 1);

            // Set entry point and source type
            shader.setEntryPoint(options.entryPoint);
            shader.setSourceEntryPoint(options.entryPoint);
            // Preserve HLSL register/semantic identity and provide Vulkan
            // locations for interfaces without vk::location attributes.
            shader.setHlslIoMapping(true);
            shader.setAutoMapBindings(true);
            shader.setAutoMapLocations(true);
            shader.setEnvInput(glslang::EShSourceHlsl, stage, glslang::EShClientVulkan, 100);
            shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_2);
            shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_5);
            shader.setEnvTargetHlslFunctionality1();

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

            if (!shader.parse(&resources, 100, false, messages, includer))
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

            if (!program.mapIO())
            {
                outError = "HLSL IO mapping error: ";
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
        return std::make_unique<GlslangShaderCompiler>();
    }

} // namespace RVX
