#include "ShaderCompiler/ShaderCompiler.h"
#include "ShaderCompiler/ShaderReflection.h"
#include "SPIRVCrossTranslator.h"
#include "Core/Log.h"

#include <glslang/Public/ShaderLang.h>
#include <glslang/Public/ResourceLimits.h>
#include <glslang/SPIRV/GlslangToSpv.h>

#include <algorithm>
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
                case RHIShaderStage::RayGeneration: return EShLangRayGen;
                case RHIShaderStage::Intersection:  return EShLangIntersect;
                case RHIShaderStage::AnyHit:        return EShLangAnyHit;
                case RHIShaderStage::ClosestHit:    return EShLangClosestHit;
                case RHIShaderStage::Miss:          return EShLangMiss;
                case RHIShaderStage::Callable:      return EShLangCallable;
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

            const std::vector<std::filesystem::path>& GetIncludedPaths() const
            {
                return m_includedPaths;
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

                if (std::find(
                        m_includedPaths.begin(),
                        m_includedPaths.end(),
                        resolvedPath) == m_includedPaths.end())
                {
                    m_includedPaths.push_back(resolvedPath);
                }

                return new IncludeResult(
                    resolvedPath.generic_string(),
                    contents->text.data(),
                    contents->text.size(),
                    contents);
            }

            std::filesystem::path m_sourcePath;
            std::filesystem::path m_includeRoot;
            std::vector<std::filesystem::path> m_includedPaths;
        };

        void PopulateSourceInfo(
            const ShaderCompileOptions& options,
            const ShaderFileIncluder& includer,
            ShaderSourceInfo& outSourceInfo)
        {
            outSourceInfo.Clear();

            const std::filesystem::path& canonicalSourcePath =
                includer.GetSourcePath();
            if (!canonicalSourcePath.empty())
            {
                outSourceInfo.mainFile = canonicalSourcePath.generic_string();
            }
            else if (options.sourcePath && options.sourcePath[0])
            {
                outSourceInfo.mainFile = options.sourcePath;
            }

            if (!outSourceInfo.mainFile.empty())
            {
                uint64 mainSourceHash =
                    ShaderSourceInfo::ComputeFileHash(outSourceInfo.mainFile);
                if (mainSourceHash == 0 && options.sourceCode)
                {
                    // Generated and non-local sources have no file-backed
                    // hash. Bind their declared identity to the exact input
                    // text so HasChanged cannot mistake a missing file for an
                    // unchanged generated source.
                    mainSourceHash =
                        ShaderSourceInfo::ComputeStringHash(options.sourceCode);
                }
                outSourceInfo.fileHashes[outSourceInfo.mainFile] =
                    mainSourceHash;
            }

            for (const std::filesystem::path& includePath :
                 includer.GetIncludedPaths())
            {
                const std::string includeName = includePath.generic_string();
                outSourceInfo.AddInclude(
                    includeName,
                    ShaderSourceInfo::ComputeFileHash(includePath));
            }

            outSourceInfo.combinedHash =
                outSourceInfo.ComputeCombinedHash();
        }

        void PopulateGLSLBindings(
            const SPIRVToGLSLResult& glslResult,
            ShaderCompileResult& outResult)
        {
            for (const GLSLBindingRemap& remap : glslResult.bindingRemaps)
            {
                const uint32 key =
                    ShaderCompileResult::GLSLBindingInfo::MakeKey(
                        remap.originalSet,
                        remap.originalBinding);
                outResult.glslBindings.setBindingToGLBinding[key] =
                    remap.glBinding;

                switch (remap.type)
                {
                    case RHIBindingType::UniformBuffer:
                        outResult.glslBindings.uboBindings[remap.name] =
                            remap.glBinding;
                        break;
                    case RHIBindingType::StorageBuffer:
                    case RHIBindingType::ShaderResourceBuffer:
                        outResult.glslBindings.ssboBindings[remap.name] =
                            remap.glBinding;
                        break;
                    case RHIBindingType::SampledTexture:
                    case RHIBindingType::CombinedTextureSampler:
                        outResult.glslBindings.textureBindings[remap.name] =
                            remap.glBinding;
                        break;
                    case RHIBindingType::Sampler:
                        outResult.glslBindings.samplerBindings[remap.name] =
                            remap.glBinding;
                        break;
                    case RHIBindingType::StorageTexture:
                        outResult.glslBindings.imageBindings[remap.name] =
                            remap.glBinding;
                        break;
                    default:
                        break;
                }
            }

            if (glslResult.pushConstantInfo)
            {
                outResult.glslPushConstant =
                    ShaderCompileResult::GLSLPushConstant{
                        glslResult.pushConstantInfo->glBinding,
                        glslResult.pushConstantInfo->size};
            }
        }
    }

    // =============================================================================
    // Portable non-Windows shader compiler using glslang. It emits SPIR-V directly
    // for Vulkan, translates SPIR-V to GLSL for OpenGL, and on Apple platforms
    // translates SPIR-V to MSL.
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
            if (options.targetBackend != RHIBackendType::Vulkan &&
                options.targetBackend != RHIBackendType::OpenGL
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

            if (!CompileHLSLToSPIRV(
                    options,
                    spirvBytecode,
                    result.sourceInfo,
                    compileError))
            {
                result.errorMessage = compileError;
                return result;
            }

            RVX_CORE_DEBUG("glslang: Compiled HLSL to {} bytes of SPIR-V", spirvBytecode.size());

            // For Vulkan backend, return SPIR-V directly
            if (options.targetBackend == RHIBackendType::Vulkan)
            {
                result.reflection = ReflectShader(
                    RHIBackendType::Vulkan,
                    options.stage,
                    spirvBytecode,
                    options.entryPoint);
                if (!result.reflection.valid)
                {
                    result.errorMessage = "SPIR-V reflection failed";
                    return result;
                }
                result.success = true;
                result.bytecode = std::move(spirvBytecode);
                return result;
            }

            if (options.targetBackend == RHIBackendType::OpenGL)
            {
                SPIRVCrossTranslator translator;
                SPIRVToGLSLOptions glslOptions;
                glslOptions.glslVersion = 450;
                glslOptions.es = false;
                glslOptions.vulkanSemantics = false;
                glslOptions.enable420Pack = true;
                glslOptions.emitPushConstantAsUBO = true;
                glslOptions.forceZeroInit = true;

                SPIRVToGLSLResult glslResult = translator.TranslateToGLSL(
                    spirvBytecode,
                    options.stage,
                    options.entryPoint,
                    glslOptions);
                if (!glslResult.success)
                {
                    result.errorMessage =
                        "SPIR-V to GLSL translation failed: " +
                        glslResult.errorMessage;
                    return result;
                }

                result.success = true;
                result.bytecode = std::move(spirvBytecode);
                PopulateGLSLBindings(glslResult, result);
                result.glslSource = std::move(glslResult.glslSource);
                result.glslVersion = glslOptions.glslVersion;
                result.reflection = std::move(glslResult.reflection);
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
                                ShaderSourceInfo& outSourceInfo,
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
            // User-defined graphics interfaces need HlslSemanticGOOGLE so
            // reflection can recover the original semantic and index. Compute
            // shaders have no such stage interface; omitting the extension
            // there keeps storage-only SPIR-V on the portable core contract.
            if (options.stage != RHIShaderStage::Compute)
            {
                shader.setEnvTargetHlslFunctionality1();
            }

            // Set up preamble with defines
            std::string preamble;
            for (const auto& def : options.defines)
            {
                preamble += "#define " + def.name + " " + def.value + "\n";
            }
            if (options.targetBackend == RHIBackendType::Metal)
            {
                preamble += "#define RVX_COMPACT_MATERIAL_SAMPLERS 1\n";
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
            PopulateSourceInfo(options, includer, outSourceInfo);

            RVX_CORE_DEBUG("glslang: Compiled HLSL to {} bytes of SPIR-V", outSPIRV.size());
            return true;
        }
    };

    std::unique_ptr<IShaderCompiler> CreateDXCShaderCompiler()
    {
        return std::make_unique<GlslangShaderCompiler>();
    }

} // namespace RVX
