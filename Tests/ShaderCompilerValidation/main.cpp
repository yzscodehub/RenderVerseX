#include "Core/Log.h"
#include "ShaderContracts/GLSLBindingABI.h"
#include "ShaderContracts/MetalBindingABI.h"
#include "ShaderContracts/ShaderArtifact.h"
#include "ShaderCompiler/ShaderCacheFormat.h"
#include "ShaderCompiler/ShaderCacheManager.h"
#include "ShaderCompiler/ShaderCompiler.h"
#include "ShaderCompiler/ShaderCompileService.h"
#include "ShaderCompiler/ShaderHotReloader.h"
#include "ShaderContracts/ShaderLayout.h"
#include "ShaderCompiler/ShaderManager.h"
#include "ShaderCompiler/ShaderPermutation.h"
#include "RHI/RHI.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace
{
    namespace fs = std::filesystem;

    class ShaderCompilerValidationFixture : public ::testing::Test
    {
    protected:
        static void SetUpTestSuite()
        {
            RVX::Log::Initialize();
        }

        static void TearDownTestSuite()
        {
            RVX::Log::Shutdown();
        }
    };

    class TempDirectory
    {
    public:
        explicit TempDirectory(const char* prefix)
        {
            const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
            m_path = fs::temp_directory_path() / (std::string(prefix) + "_" + std::to_string(ticks));
            fs::create_directories(m_path);
        }

        ~TempDirectory()
        {
            std::error_code ec;
            fs::remove_all(m_path, ec);
        }

        const fs::path& Path() const { return m_path; }

    private:
        fs::path m_path;
    };

    void WriteTextFile(const fs::path& path, const std::string& text)
    {
        fs::create_directories(path.parent_path());
        std::ofstream file(path, std::ios::binary);
        file << text;
    }

    std::string ReadTextFile(const fs::path& path)
    {
        std::ifstream file(path, std::ios::binary);
        return {
            std::istreambuf_iterator<char>(file),
            std::istreambuf_iterator<char>()};
    }

    bool HasTrackedInclude(
        const RVX::ShaderCompileResult& result,
        const char* filename)
    {
        return std::any_of(
            result.sourceInfo.includeFiles.begin(),
            result.sourceInfo.includeFiles.end(),
            [filename](const std::string& include)
            {
                return fs::path(include).filename() == filename;
            });
    }

    bool HasVertexInput(
        const RVX::ShaderCompileResult& result,
        const char* semantic,
        RVX::uint32 semanticIndex)
    {
        return std::any_of(
            result.reflection.inputs.begin(),
            result.reflection.inputs.end(),
            [semantic, semanticIndex](
                const RVX::ShaderReflection::InputAttribute& input)
            {
                return input.semantic == semantic &&
                       input.semanticIndex == semanticIndex &&
                       !input.systemValue;
            });
    }

    bool HasResourceBinding(
        const RVX::ShaderCompileResult& result,
        const char* name)
    {
        return std::any_of(
            result.reflection.resources.begin(),
            result.reflection.resources.end(),
            [name](const RVX::ShaderReflection::ResourceBinding& resource)
            {
                return resource.name == name;
            });
    }

    bool HasDXILContainerHeader(const std::vector<RVX::uint8>& bytecode)
    {
        return bytecode.size() >= 4 &&
               bytecode[0] == static_cast<RVX::uint8>('D') &&
               bytecode[1] == static_cast<RVX::uint8>('X') &&
               bytecode[2] == static_cast<RVX::uint8>('B') &&
               bytecode[3] == static_cast<RVX::uint8>('C');
    }

    bool HasDXBCContainerHeader(const std::vector<RVX::uint8>& bytecode)
    {
        return HasDXILContainerHeader(bytecode);
    }

    bool HasSPIRVHeader(const std::vector<RVX::uint8>& bytecode)
    {
        if (bytecode.size() < sizeof(RVX::uint32))
        {
            return false;
        }

        RVX::uint32 magic = 0;
        std::memcpy(&magic, bytecode.data(), sizeof(magic));
        return magic == 0x07230203u;
    }

    bool HasSPIRVExtension(
        const std::vector<RVX::uint8>& bytecode,
        std::string_view extension)
    {
        return std::search(
                   bytecode.begin(),
                   bytecode.end(),
                   extension.begin(),
                   extension.end(),
                   [](RVX::uint8 byte, char character)
                   {
                       return byte == static_cast<RVX::uint8>(character);
                   }) != bytecode.end();
    }

    bool HasSPIRVCapability(
        const std::vector<RVX::uint8>& bytecode,
        RVX::uint32 expectedCapability)
    {
        constexpr RVX::uint32 SPIRV_CAPABILITY_OPCODE = 17u;
        constexpr size_t SPIRV_HEADER_WORD_COUNT = 5u;

        if (!HasSPIRVHeader(bytecode) ||
            bytecode.size() % sizeof(RVX::uint32) != 0u)
        {
            return false;
        }

        const size_t wordCount = bytecode.size() / sizeof(RVX::uint32);
        for (size_t wordIndex = SPIRV_HEADER_WORD_COUNT;
             wordIndex < wordCount;)
        {
            RVX::uint32 instruction = 0;
            std::memcpy(
                &instruction,
                bytecode.data() + wordIndex * sizeof(RVX::uint32),
                sizeof(instruction));

            const RVX::uint32 instructionWordCount = instruction >> 16u;
            const RVX::uint32 opcode = instruction & 0xffffu;
            if (instructionWordCount == 0u ||
                instructionWordCount > wordCount - wordIndex)
            {
                return false;
            }

            if (opcode == SPIRV_CAPABILITY_OPCODE && instructionWordCount >= 2u)
            {
                RVX::uint32 capability = 0;
                std::memcpy(
                    &capability,
                    bytecode.data() + (wordIndex + 1u) * sizeof(RVX::uint32),
                    sizeof(capability));
                if (capability == expectedCapability)
                {
                    return true;
                }
            }

            wordIndex += instructionWordCount;
        }

        return false;
    }

    struct SurfaceShaderCompileCase
    {
        const char* label = nullptr;
        const char* filename = nullptr;
        RVX::RHIShaderStage stage = RVX::RHIShaderStage::None;
        const char* entryPoint = nullptr;
        bool requiresTexCoord1 = false;
        const char* defineName = nullptr;
        const char* defineValue = nullptr;
        bool gpuSceneRasterOnly = false;
        const char* requiredResource = nullptr;
    };

    constexpr std::array<SurfaceShaderCompileCase, 22> GFX06_SURFACE_SHADER_CASES = {{
        // DefaultLit is used by direct, GPU-driven, and GPU-scene raster paths.
        {"DefaultLit PS", "DefaultLit.hlsl", RVX::RHIShaderStage::Pixel, "PSMain", false},
        {"DefaultLit ThinTransmission PS", "DefaultLit.hlsl", RVX::RHIShaderStage::Pixel, "PSForwardTranslucencyGuarded", false},
        {"DefaultLit Forward surface data PS", "DefaultLit.hlsl", RVX::RHIShaderStage::Pixel, "PSForwardSurfaceData", false},
        {"DefaultLit VS direct", "DefaultLit.hlsl", RVX::RHIShaderStage::Vertex, "VSMain", true},
        {"DefaultLit VS rigid", "DefaultLit.hlsl", RVX::RHIShaderStage::Vertex, "VSMainRigid", true},
        {"DefaultLit VS GPU-driven", "DefaultLit.hlsl", RVX::RHIShaderStage::Vertex, "VSMainGPUDriven", true, nullptr, nullptr, false, "GPUDrivenInstances"},
        {"DefaultLit VS instanced material", "DefaultLit.hlsl", RVX::RHIShaderStage::Vertex, "VSMainInstancedMaterial", true, nullptr, nullptr, false, "GPUDrivenInstances"},
        {"DefaultLit VS GPU-scene", "DefaultLit.hlsl", RVX::RHIShaderStage::Vertex, "VSMainGPUScene", true, "RVX_GPU_SCENE_RASTER", "1", true, "GPUSceneRasterCandidates"},
        {"DefaultLit VS GPU-scene instanced material", "DefaultLit.hlsl", RVX::RHIShaderStage::Vertex, "VSMainGPUSceneInstancedMaterial", true, "RVX_GPU_SCENE_RASTER", "1", true, "GPUSceneRasterCandidates"},

        // DepthOnly supplies both the depth-prepass and raster shadow caster.
        {"Shadow VS direct", "DepthOnly.hlsl", RVX::RHIShaderStage::Vertex, "VSMain", false},
        {"Shadow VS rigid", "DepthOnly.hlsl", RVX::RHIShaderStage::Vertex, "VSMainRigid", false},
        {"Shadow VS GPU-driven", "DepthOnly.hlsl", RVX::RHIShaderStage::Vertex, "VSMainGPUDriven", false, nullptr, nullptr, false, "GPUDrivenInstances"},
        {"Shadow VS GPU-scene", "DepthOnly.hlsl", RVX::RHIShaderStage::Vertex, "VSMainGPUScene", false, "RVX_GPU_SCENE_RASTER", "1", true, "GPUSceneRasterCandidates"},
        {"Shadow VS masked", "DepthOnly.hlsl", RVX::RHIShaderStage::Vertex, "VSMainMasked", true},
        {"Shadow VS masked rigid", "DepthOnly.hlsl", RVX::RHIShaderStage::Vertex, "VSMainMaskedRigid", true},
        {"Shadow PS masked", "DepthOnly.hlsl", RVX::RHIShaderStage::Pixel, "PSMainMasked", false},

        // ObjectVelocity has a direct/skinned family; it intentionally does not
        // advertise a GPU-scene entry point until motion history is published
        // through that path as well.
        {"ObjectVelocity VS skinned", "ObjectVelocity.hlsl", RVX::RHIShaderStage::Vertex, "VSMain", false},
        {"ObjectVelocity VS rigid", "ObjectVelocity.hlsl", RVX::RHIShaderStage::Vertex, "VSMainRigid", false},
        {"ObjectVelocity VS masked", "ObjectVelocity.hlsl", RVX::RHIShaderStage::Vertex, "VSMainMasked", true},
        {"ObjectVelocity VS masked rigid", "ObjectVelocity.hlsl", RVX::RHIShaderStage::Vertex, "VSMainMaskedRigid", true},
        {"ObjectVelocity PS", "ObjectVelocity.hlsl", RVX::RHIShaderStage::Pixel, "PSMain", false},
        {"ObjectVelocity PS masked", "ObjectVelocity.hlsl", RVX::RHIShaderStage::Pixel, "PSMainMasked", false},
    }};

    bool SupportsGPUSceneRaster(RVX::RHIBackendType backend)
    {
        return backend == RVX::RHIBackendType::DX12 ||
               backend == RVX::RHIBackendType::Vulkan ||
               backend == RVX::RHIBackendType::Metal;
    }

    void VerifyGFX06SurfaceShaderMatrix(
        RVX::IShaderCompiler& compiler,
        RVX::RHIBackendType backend)
    {
        const fs::path shaderDirectory =
            fs::path(RVX_SOURCE_DIR) / "Source" / "Runtime" / "Render" /
            "Shaders";
        size_t expectedCaseCount = 0;
        size_t compiledCaseCount = 0;
        for (const SurfaceShaderCompileCase& shaderCase : GFX06_SURFACE_SHADER_CASES)
        {
            if (!shaderCase.gpuSceneRasterOnly || SupportsGPUSceneRaster(backend))
            {
                ++expectedCaseCount;
            }
        }

        for (const SurfaceShaderCompileCase& shaderCase : GFX06_SURFACE_SHADER_CASES)
        {
            const fs::path shaderPath = shaderDirectory / shaderCase.filename;
            SCOPED_TRACE(
                std::string(shaderCase.label) + " for backend " +
                std::to_string(static_cast<RVX::uint32>(backend)));

            // GPU-scene raster is deliberately not a compatibility-backend
            // fallback. The feature is only a production permutation on the
            // backends with the qualified GPU-scene resource model.
            if (shaderCase.gpuSceneRasterOnly && !SupportsGPUSceneRaster(backend))
            {
                continue;
            }
            ++compiledCaseCount;

            if (!fs::exists(shaderPath))
            {
                ADD_FAILURE() << "Missing renderer shader: " << shaderPath.string();
                continue;
            }

            const std::string source = ReadTextFile(shaderPath);
            if (source.empty())
            {
                ADD_FAILURE() << "Failed to read renderer shader: " << shaderPath.string();
                continue;
            }

            const std::string shaderPathString = shaderPath.string();
            RVX::ShaderCompileOptions options;
            options.stage = shaderCase.stage;
            options.entryPoint = shaderCase.entryPoint;
            options.sourceCode = source.c_str();
            options.sourcePath = shaderPathString.c_str();
            options.targetBackend = backend;
            // FXC's unoptimized SM5 validator exhausts an internal temporary
            // on the complete ThinTransmission composite. The production DX11
            // path is optimized, so compile that exact configuration.
            options.enableOptimization =
                backend == RVX::RHIBackendType::DX11 &&
                std::string_view(shaderCase.entryPoint) ==
                    "PSForwardTranslucencyGuarded";
            if (shaderCase.defineName != nullptr)
            {
                options.defines.push_back({shaderCase.defineName, shaderCase.defineValue});
            }

            const RVX::ShaderCompileSupport support = compiler.QuerySupport(options);
            ASSERT_TRUE(support.IsSupported())
                << "Compiler does not support " << shaderCase.label
                << ": " << support.reason;

            const RVX::ShaderCompileResult result = compiler.Compile(options);
            ASSERT_TRUE(result.success)
                << "Failed to compile " << shaderCase.label
                << ": " << result.errorMessage;

            ASSERT_TRUE(result.reflection.valid)
                << shaderCase.label << " did not produce reflection";
            ASSERT_FALSE(result.bytecode.empty())
                << shaderCase.label << " did not produce bytecode";
            // FXC's standard include handler does not expose an include
            // dependency transcript. DX11 still verifies a real DXBC artifact;
            // compilers that can report dependencies must prove that the shared
            // surface contract was consumed.
            if (backend != RVX::RHIBackendType::DX11)
            {
                ASSERT_TRUE(HasTrackedInclude(result, "SurfaceMaterial.hlsli"))
                    << shaderCase.label
                    << " did not track the shared material surface contract";
            }

            if (backend == RVX::RHIBackendType::DX12)
            {
                EXPECT_TRUE(HasDXILContainerHeader(result.bytecode))
                    << shaderCase.label << " did not produce a DXIL container";
            }
            else if (backend == RVX::RHIBackendType::DX11)
            {
                EXPECT_TRUE(HasDXBCContainerHeader(result.bytecode))
                    << shaderCase.label << " did not produce DX11 shader bytecode";
            }
            else if (backend == RVX::RHIBackendType::Vulkan ||
                     backend == RVX::RHIBackendType::OpenGL)
            {
                EXPECT_TRUE(HasSPIRVHeader(result.bytecode))
                    << shaderCase.label << " did not produce SPIR-V";
            }

            if (backend == RVX::RHIBackendType::OpenGL)
            {
                EXPECT_FALSE(result.glslSource.empty())
                    << shaderCase.label << " did not produce GLSL";
                EXPECT_NE(result.glslSource.find("#version"), std::string::npos)
                    << shaderCase.label << " did not produce versioned GLSL";
                EXPECT_EQ(result.glslVersion, 450u)
                    << shaderCase.label << " used an unexpected GLSL version";
            }
            else if (backend == RVX::RHIBackendType::Metal)
            {
                EXPECT_FALSE(result.mslSource.empty())
                    << shaderCase.label << " did not produce MSL";
                EXPECT_EQ(result.mslEntryPoint, shaderCase.entryPoint)
                    << shaderCase.label << " produced an unexpected MSL entry point";
            }

            if (shaderCase.requiresTexCoord1)
            {
                EXPECT_TRUE(HasVertexInput(result, "TEXCOORD", 1))
                    << shaderCase.label
                    << " lost the authored UV1 / vertex-slot-7 contract";
            }

            if (shaderCase.requiredResource != nullptr)
            {
                EXPECT_TRUE(HasResourceBinding(result, shaderCase.requiredResource))
                    << shaderCase.label << " did not retain the expected "
                    << shaderCase.requiredResource << " permutation resource";
            }
        }

        // Keep compatibility-backend exclusions explicit and measurable. A
        // GPU-scene permutation is not silently replaced by a direct shader.
        EXPECT_EQ(compiledCaseCount, expectedCaseCount);
    }

    constexpr std::array<const char*, 4> GFX19_SSR_ENTRY_POINTS = {
        "PSHiZBase",
        "PSHiZReduce",
        "PSTrace",
        "PSResolve",
    };

    void VerifyGFX19SSRShaderMatrix(
        RVX::IShaderCompiler& compiler,
        RVX::RHIBackendType backend)
    {
        const fs::path shaderPath =
            fs::path(RVX_SOURCE_DIR) / "Source" / "Runtime" / "Render" /
            "Shaders" / "Reflection" / "HiZScreenSpaceReflection.hlsl";
        ASSERT_TRUE(fs::exists(shaderPath))
            << "Missing GFX19 SSR shader: " << shaderPath.string();
        const std::string source = ReadTextFile(shaderPath);
        ASSERT_FALSE(source.empty())
            << "Failed to read GFX19 SSR shader: " << shaderPath.string();
        // Keep A3's bandwidth win and fail-closed edge handling explicit.
        // The native qualification proves rendered behavior; these source
        // checks prevent a future shader rewrite from silently replacing the
        // guided resolve with nearest-neighbor sampling or a fifth pass.
        EXPECT_NE(source.find("RVX_SSR_FLAG_HALF_RESOLUTION_TRACE"),
                  std::string::npos);
        EXPECT_NE(source.find("output.radiance.a = receiverViewDepth;"),
                  std::string::npos);
        EXPECT_NE(source.find("RVX_ResolveScreenSpaceSample"),
                  std::string::npos);
        EXPECT_NE(source.find("RVX_ReconstructNormal(candidateUV)"),
                  std::string::npos);
        EXPECT_NE(source.find("guideWeight * candidateConfidence"),
                  std::string::npos);
        EXPECT_NE(source.find("hitWeightSum / guideWeightSum"),
                  std::string::npos);
        EXPECT_EQ(source.find("PSUpscale"), std::string::npos);
        const std::string shaderPathString = shaderPath.string();

        for (const char* const entryPoint : GFX19_SSR_ENTRY_POINTS)
        {
            SCOPED_TRACE(
                std::string("GFX19 SSR ") + entryPoint + " backend " +
                std::to_string(static_cast<RVX::uint32>(backend)));
            RVX::ShaderCompileOptions options;
            options.stage = RVX::RHIShaderStage::Pixel;
            options.entryPoint = entryPoint;
            options.sourceCode = source.c_str();
            options.sourcePath = shaderPathString.c_str();
            options.targetBackend = backend;
            options.enableOptimization = false;

            const RVX::ShaderCompileSupport support =
                compiler.QuerySupport(options);
            ASSERT_TRUE(support.IsSupported()) << support.reason;

            const RVX::ShaderCompileResult result = compiler.Compile(options);
            ASSERT_TRUE(result.success) << result.errorMessage;
            ASSERT_TRUE(result.reflection.valid);
            ASSERT_FALSE(result.bytecode.empty());
            if (backend == RVX::RHIBackendType::DX12)
            {
                EXPECT_TRUE(HasDXILContainerHeader(result.bytecode));
            }
            else if (backend == RVX::RHIBackendType::Vulkan)
            {
                EXPECT_TRUE(HasSPIRVHeader(result.bytecode));
            }

            const auto findResource = [&result](const char* name)
            {
                return std::find_if(
                    result.reflection.resources.begin(),
                    result.reflection.resources.end(),
                    [name](const RVX::ShaderReflection::ResourceBinding& resource)
                    {
                        return resource.name == name;
                    });
            };
            const auto expectSampledTexture =
                [&findResource, &result](const char* name,
                                         RVX::uint32 binding)
            {
                const auto resource = findResource(name);
                ASSERT_NE(resource, result.reflection.resources.end());
                EXPECT_EQ(resource->set, 0u);
                EXPECT_EQ(resource->binding, binding);
                EXPECT_EQ(resource->type,
                          RVX::RHIBindingType::SampledTexture);
                EXPECT_EQ(resource->count, 1u);
            };
            const std::string_view entry(entryPoint);
            if (entry == "PSTrace")
            {
                expectSampledTexture("SceneColor", 1u);
                expectSampledTexture("StageSource", 2u);
                expectSampledTexture("NormalRoughness", 3u);
                expectSampledTexture("HiZPyramid", 4u);
            }
            // PSResolve is the ABI owner of the fixed-capacity local-probe
            // array, guided-upscale inputs, and materialized reflection
            // surfaces. Check compiled reflection, rather than source text,
            // so register drift cannot hide behind a stale descriptor layout.
            if (entry != "PSResolve")
            {
                continue;
            }
            expectSampledTexture("StageSource", 2u);
            expectSampledTexture("NormalRoughness", 3u);
            expectSampledTexture("RawReflection", 5u);
            expectSampledTexture("ReflectionConfidence", 6u);
            const auto localProbes = findResource("LocalProbes");
            ASSERT_NE(localProbes, result.reflection.resources.end());
            EXPECT_EQ(localProbes->set, 0u);
            EXPECT_EQ(localProbes->binding, 7u);
            EXPECT_EQ(localProbes->type, RVX::RHIBindingType::SampledTexture);
            EXPECT_EQ(localProbes->count, 8u);

            const auto baseIndirectSpecular = findResource("BaseIndirectSpecular");
            ASSERT_NE(baseIndirectSpecular, result.reflection.resources.end());
            EXPECT_EQ(baseIndirectSpecular->set, 0u);
            EXPECT_EQ(baseIndirectSpecular->binding, 15u);
            EXPECT_EQ(baseIndirectSpecular->type, RVX::RHIBindingType::SampledTexture);
            EXPECT_EQ(baseIndirectSpecular->count, 1u);

            const auto exposureState = findResource("ExposureState");
            ASSERT_NE(exposureState, result.reflection.resources.end());
            EXPECT_EQ(exposureState->set, 0u);
            EXPECT_EQ(exposureState->binding, 16u);
            EXPECT_EQ(exposureState->type, RVX::RHIBindingType::ShaderResourceBuffer);
            EXPECT_EQ(exposureState->count, 1u);

            const auto specularResponse = findResource("SpecularResponse");
            ASSERT_NE(specularResponse, result.reflection.resources.end());
            EXPECT_EQ(specularResponse->set, 0u);
            EXPECT_EQ(specularResponse->binding, 17u);
            EXPECT_EQ(specularResponse->type, RVX::RHIBindingType::SampledTexture);
            EXPECT_EQ(specularResponse->count, 1u);

            const auto sampler = findResource("LinearClampSampler");
            ASSERT_NE(sampler, result.reflection.resources.end());
            EXPECT_EQ(sampler->set, 0u);
            EXPECT_EQ(sampler->binding, 19u);
            EXPECT_EQ(sampler->type, RVX::RHIBindingType::Sampler);
            EXPECT_EQ(sampler->count, 1u);

            if (backend == RVX::RHIBackendType::OpenGL)
            {
                EXPECT_FALSE(result.glslSource.empty());
                EXPECT_NE(result.glslSource.find("#version"), std::string::npos);
            }
            else if (backend == RVX::RHIBackendType::Metal)
            {
                EXPECT_FALSE(result.mslSource.empty());
                EXPECT_FALSE(result.mslEntryPoint.empty());
            }
        }
    }

    void VerifyGFX22WeightedBlendedOITShaderMatrix(
        RVX::IShaderCompiler& compiler,
        RVX::RHIBackendType backend)
    {
        struct ShaderCase
        {
            const char* asset;
            RVX::RHIShaderStage stage;
            const char* entry;
            bool fullscreenPermutation = false;
        };
        constexpr std::array cases = {
            ShaderCase{"DefaultLit.hlsl", RVX::RHIShaderStage::Pixel,
                       "PSWeightedBlendedOIT", false},
            ShaderCase{"Particle/ParticleBillboard.hlsl",
                       RVX::RHIShaderStage::Pixel,
                       "PSWeightedBlendedOIT", true},
            ShaderCase{"Translucency/WeightedBlendedOITComposite.hlsl",
                       RVX::RHIShaderStage::Vertex, "VSMain", true},
            ShaderCase{"Translucency/WeightedBlendedOITComposite.hlsl",
                       RVX::RHIShaderStage::Pixel, "PSMain", true},
        };
        const fs::path shaderDirectory =
            fs::path(RVX_SOURCE_DIR) / "Source" / "Runtime" / "Render" /
            "Shaders";

        for (const ShaderCase& shaderCase : cases)
        {
            const fs::path shaderPath = shaderDirectory / shaderCase.asset;
            SCOPED_TRACE(
                std::string("GFX22 WBOIT ") + shaderCase.asset + ":" +
                shaderCase.entry + " backend " +
                std::to_string(static_cast<RVX::uint32>(backend)));
            ASSERT_TRUE(fs::exists(shaderPath)) << shaderPath.string();
            const std::string source = ReadTextFile(shaderPath);
            ASSERT_FALSE(source.empty()) << shaderPath.string();
            const std::string shaderPathString = shaderPath.string();

            RVX::ShaderCompileOptions options;
            options.stage = shaderCase.stage;
            options.entryPoint = shaderCase.entry;
            options.sourceCode = source.c_str();
            options.sourcePath = shaderPathString.c_str();
            options.targetBackend = backend;
            options.enableOptimization = false;
            if (shaderCase.fullscreenPermutation)
            {
                options.defines.push_back({
                    "RVX_FULLSCREEN_UV_Y_MATCHES_CLIP_Y",
                    backend == RVX::RHIBackendType::Vulkan ||
                            backend == RVX::RHIBackendType::OpenGL
                        ? "1"
                        : "0"});
            }

            const RVX::ShaderCompileSupport support =
                compiler.QuerySupport(options);
            ASSERT_TRUE(support.IsSupported()) << support.reason;
            const RVX::ShaderCompileResult result = compiler.Compile(options);
            ASSERT_TRUE(result.success) << result.errorMessage;
            ASSERT_TRUE(result.reflection.valid);
            ASSERT_FALSE(result.bytecode.empty());
            if (backend != RVX::RHIBackendType::DX11)
            {
                EXPECT_TRUE(HasTrackedInclude(
                    result, "WeightedBlendedOIT.hlsli"));
            }
            if (backend == RVX::RHIBackendType::DX12)
            {
                EXPECT_TRUE(HasDXILContainerHeader(result.bytecode));
            }
            else if (backend == RVX::RHIBackendType::DX11)
            {
                EXPECT_TRUE(HasDXBCContainerHeader(result.bytecode));
            }
            else if (backend == RVX::RHIBackendType::Vulkan ||
                     backend == RVX::RHIBackendType::OpenGL)
            {
                EXPECT_TRUE(HasSPIRVHeader(result.bytecode));
            }
            if (backend == RVX::RHIBackendType::OpenGL)
            {
                EXPECT_FALSE(result.glslSource.empty());
            }
            else if (backend == RVX::RHIBackendType::Metal)
            {
                EXPECT_FALSE(result.mslSource.empty());
                EXPECT_EQ(result.mslEntryPoint, shaderCase.entry);
            }

            if (std::string_view(shaderCase.asset) !=
                    "Translucency/WeightedBlendedOITComposite.hlsl" ||
                shaderCase.stage != RVX::RHIShaderStage::Pixel)
            {
                continue;
            }
            const auto findResource = [&result](const char* name)
            {
                return std::find_if(
                    result.reflection.resources.begin(),
                    result.reflection.resources.end(),
                    [name](const RVX::ShaderReflection::ResourceBinding& resource)
                    {
                        return resource.name == name;
                    });
            };
            EXPECT_EQ(findResource("SourceSceneHDR"),
                      result.reflection.resources.end());
            for (const auto [name, binding] : {
                     std::pair{"OITAccumulation", 0u},
                     std::pair{"OITRevealage", 1u}})
            {
                const auto resource = findResource(name);
                ASSERT_NE(resource, result.reflection.resources.end()) << name;
                EXPECT_EQ(resource->set, 0u) << name;
                EXPECT_EQ(resource->binding, binding) << name;
                EXPECT_EQ(resource->type, RVX::RHIBindingType::SampledTexture)
                    << name;
                EXPECT_EQ(resource->count, 1u) << name;
            }
        }
    }

    void VerifyGFX24TAAShaderMatrix(
        RVX::IShaderCompiler& compiler,
        RVX::RHIBackendType backend)
    {
        constexpr std::array<const char*, 1> entryPoints = {
            "PSResolve",
        };
        const fs::path shaderPath =
            fs::path(RVX_SOURCE_DIR) / "Source" / "Runtime" / "Render" /
            "Shaders" / "PostProcess" / "TAA.hlsl";
        ASSERT_TRUE(fs::exists(shaderPath)) << shaderPath.string();
        const std::string source = ReadTextFile(shaderPath);
        ASSERT_FALSE(source.empty()) << shaderPath.string();
        const std::string shaderPathString = shaderPath.string();

        for (const char* const entryPoint : entryPoints)
        {
            SCOPED_TRACE(
                std::string("GFX24 TAA ") + entryPoint + " backend " +
                std::to_string(static_cast<RVX::uint32>(backend)));

            RVX::ShaderCompileOptions options;
            options.stage = RVX::RHIShaderStage::Pixel;
            options.entryPoint = entryPoint;
            options.sourceCode = source.c_str();
            options.sourcePath = shaderPathString.c_str();
            options.targetBackend = backend;
            options.enableOptimization = false;
            options.defines.push_back({
                "RVX_FULLSCREEN_UV_Y_MATCHES_CLIP_Y",
                backend == RVX::RHIBackendType::Vulkan ||
                        backend == RVX::RHIBackendType::OpenGL
                    ? "1"
                    : "0"});

            const RVX::ShaderCompileSupport support =
                compiler.QuerySupport(options);
            ASSERT_TRUE(support.IsSupported()) << support.reason;
            const RVX::ShaderCompileResult result = compiler.Compile(options);
            ASSERT_TRUE(result.success) << result.errorMessage;
            ASSERT_TRUE(result.reflection.valid);
            ASSERT_FALSE(result.bytecode.empty());
            if (backend != RVX::RHIBackendType::DX11)
            {
                EXPECT_TRUE(HasTrackedInclude(result, "FullscreenTriangle.hlsli"));
                EXPECT_TRUE(HasTrackedInclude(result, "Exposure.hlsli"));
            }
            if (backend == RVX::RHIBackendType::DX12)
            {
                EXPECT_TRUE(HasDXILContainerHeader(result.bytecode));
            }
            else if (backend == RVX::RHIBackendType::DX11)
            {
                EXPECT_TRUE(HasDXBCContainerHeader(result.bytecode));
            }
            else if (backend == RVX::RHIBackendType::Vulkan ||
                     backend == RVX::RHIBackendType::OpenGL)
            {
                EXPECT_TRUE(HasSPIRVHeader(result.bytecode));
            }
            if (backend == RVX::RHIBackendType::OpenGL)
            {
                EXPECT_FALSE(result.glslSource.empty());
            }
            else if (backend == RVX::RHIBackendType::Metal)
            {
                EXPECT_FALSE(result.mslSource.empty());
                EXPECT_EQ(result.mslEntryPoint, entryPoint);
            }

            const auto findResource = [&result](const char* name)
            {
                return std::find_if(
                    result.reflection.resources.begin(),
                    result.reflection.resources.end(),
                    [name](const RVX::ShaderReflection::ResourceBinding& resource)
                    {
                        return resource.name == name;
                    });
            };
            const auto expectResource = [&result, &findResource](
                                            const char* name,
                                            RVX::uint32 binding,
                                            RVX::RHIBindingType type)
            {
                const auto resource = findResource(name);
                ASSERT_NE(resource, result.reflection.resources.end()) << name;
                EXPECT_EQ(resource->set, 0u) << name;
                EXPECT_EQ(resource->binding, binding) << name;
                EXPECT_EQ(resource->type, type) << name;
                EXPECT_EQ(resource->count, 1u) << name;
            };

            expectResource(
                "CurrentColor", 1u, RVX::RHIBindingType::SampledTexture);
            expectResource(
                "CurrentDepth", 3u, RVX::RHIBindingType::SampledTexture);
            expectResource(
                "LinearClampSampler", 7u, RVX::RHIBindingType::Sampler);

            expectResource(
                "TAAConstants", 0u, RVX::RHIBindingType::UniformBuffer);
            expectResource(
                "HistoryColor", 2u, RVX::RHIBindingType::SampledTexture);
            expectResource(
                "HistoryDepth", 4u, RVX::RHIBindingType::SampledTexture);
            expectResource(
                "Velocity", 5u, RVX::RHIBindingType::SampledTexture);
            expectResource(
                "ReactiveMask", 6u, RVX::RHIBindingType::SampledTexture);
            expectResource(
                "ExposureState", 8u,
                RVX::RHIBindingType::ShaderResourceBuffer);

            constexpr std::array<RVX::RHIFormat, 3> expectedOutputFormats = {
                RVX::RHIFormat::RGBA32_FLOAT,
                RVX::RHIFormat::RGBA32_FLOAT,
                RVX::RHIFormat::R32_FLOAT,
            };
            ASSERT_EQ(result.reflection.outputs.size(),
                      expectedOutputFormats.size());
            for (RVX::uint32 location = 0;
                 location < expectedOutputFormats.size();
                 ++location)
            {
                const auto output = std::find_if(
                    result.reflection.outputs.begin(),
                    result.reflection.outputs.end(),
                    [location](
                        const RVX::ShaderReflection::InputAttribute& attribute)
                    {
                        return attribute.location == location;
                    });
                ASSERT_NE(output, result.reflection.outputs.end()) << location;
                EXPECT_EQ(output->format, expectedOutputFormats[location])
                    << location;
            }
        }
    }

    void VerifyOpaqueSceneColorPyramidShaderMatrix(
        RVX::IShaderCompiler& compiler,
        RVX::RHIBackendType backend)
    {
        const fs::path shaderPath =
            fs::path(RVX_SOURCE_DIR) / "Source" / "Runtime" / "Render" /
            "Shaders" / "PostProcess" / "OpaqueSceneColorPyramid.hlsl";
        ASSERT_TRUE(fs::exists(shaderPath)) << shaderPath.string();
        const std::string source = ReadTextFile(shaderPath);
        ASSERT_FALSE(source.empty()) << shaderPath.string();
        const std::string shaderPathString = shaderPath.string();

        for (const auto [stage, entryPoint] : {
                 std::pair{RVX::RHIShaderStage::Vertex,
                           std::string_view{"VSMain"}},
                 std::pair{RVX::RHIShaderStage::Pixel,
                           std::string_view{"PSMain"}}})
        {
            SCOPED_TRACE(
                std::string("OpaqueSceneColorPyramid ") +
                std::string(entryPoint) + " backend " +
                std::to_string(static_cast<RVX::uint32>(backend)));
            RVX::ShaderCompileOptions options;
            options.stage = stage;
            options.entryPoint = entryPoint.data();
            options.sourceCode = source.c_str();
            options.sourcePath = shaderPathString.c_str();
            options.targetBackend = backend;
            options.enableOptimization = false;
            options.defines.push_back({
                "RVX_FULLSCREEN_UV_Y_MATCHES_CLIP_Y",
                backend == RVX::RHIBackendType::Vulkan ||
                        backend == RVX::RHIBackendType::OpenGL
                    ? "1"
                    : "0"});

            const RVX::ShaderCompileSupport support =
                compiler.QuerySupport(options);
            ASSERT_TRUE(support.IsSupported()) << support.reason;
            const RVX::ShaderCompileResult result = compiler.Compile(options);
            ASSERT_TRUE(result.success) << result.errorMessage;
            ASSERT_TRUE(result.reflection.valid);
            ASSERT_FALSE(result.bytecode.empty());
            if (backend != RVX::RHIBackendType::DX11)
            {
                EXPECT_TRUE(HasTrackedInclude(
                    result, "FullscreenTriangle.hlsli"));
            }
            if (backend == RVX::RHIBackendType::DX12)
            {
                EXPECT_TRUE(HasDXILContainerHeader(result.bytecode));
            }
            else if (backend == RVX::RHIBackendType::DX11)
            {
                EXPECT_TRUE(HasDXBCContainerHeader(result.bytecode));
            }
            else if (backend == RVX::RHIBackendType::Vulkan ||
                     backend == RVX::RHIBackendType::OpenGL)
            {
                EXPECT_TRUE(HasSPIRVHeader(result.bytecode));
            }
            if (backend == RVX::RHIBackendType::OpenGL)
            {
                EXPECT_FALSE(result.glslSource.empty());
            }
            else if (backend == RVX::RHIBackendType::Metal)
            {
                EXPECT_FALSE(result.mslSource.empty());
                EXPECT_EQ(result.mslEntryPoint, entryPoint);
            }

            if (stage != RVX::RHIShaderStage::Pixel)
            {
                continue;
            }
            const auto findResource = [&result](const char* name)
            {
                return std::find_if(
                    result.reflection.resources.begin(),
                    result.reflection.resources.end(),
                    [name](
                        const RVX::ShaderReflection::ResourceBinding& resource)
                    {
                        return resource.name == name;
                    });
            };
            const auto expectResource = [&result, &findResource](
                                            const char* name,
                                            RVX::uint32 binding,
                                            RVX::RHIBindingType type)
            {
                const auto resource = findResource(name);
                ASSERT_NE(resource, result.reflection.resources.end()) << name;
                EXPECT_EQ(resource->set, 0u) << name;
                EXPECT_EQ(resource->binding, binding) << name;
                EXPECT_EQ(resource->type, type) << name;
                EXPECT_EQ(resource->count, 1u) << name;
            };
            expectResource("OpaqueSceneColorPyramidConstants", 0u,
                           RVX::RHIBindingType::UniformBuffer);
            expectResource("InputSceneColor", 1u,
                           RVX::RHIBindingType::SampledTexture);
            expectResource("LinearClampSampler", 2u,
                           RVX::RHIBindingType::Sampler);
        }
    }

    std::string TestVertexShaderSource()
    {
        return R"(
#include "Common.hlsli"

cbuffer Camera : register(b0, space0)
{
    float4x4 gViewProjection;
};

struct VSInput
{
    float3 position : POSITION;
    float2 texCoord : TEXCOORD1;
    uint4 blendIndices : BLENDINDICES0;
};

struct VSOutput
{
    float4 position : SV_POSITION;
    float2 texCoord : TEXCOORD1;
};

VSOutput main(VSInput input)
{
    VSOutput output;
    float blendOffset = float(input.blendIndices.x) * 0.000001;
    output.position = mul(
        gViewProjection,
        float4(
            input.position + RVX_TEST_OFFSET +
                float3(blendOffset, 0.0, 0.0),
            1.0));
    output.texCoord = input.texCoord;
    return output;
}
)";
    }

    class BlockingEchoCompiler final : public RVX::IShaderCompiler
    {
    public:
        BlockingEchoCompiler(
            std::shared_ptr<std::promise<void>> entered,
            std::shared_future<void> release,
            std::shared_ptr<std::mutex> observedMutex,
            std::shared_ptr<std::vector<std::string>> observedValues)
            : m_entered(std::move(entered))
            , m_release(std::move(release))
            , m_observedMutex(std::move(observedMutex))
            , m_observedValues(std::move(observedValues))
        {
        }

        RVX::ShaderCompileSupport QuerySupport(
            const RVX::ShaderCompileOptions&) const override
        {
            return RVX::ShaderCompileSupport::Supported();
        }

        RVX::ShaderCompileResult Compile(const RVX::ShaderCompileOptions& options) override
        {
            m_entered->set_value();
            m_release.wait();

            std::vector<std::string> values;
            values.emplace_back(options.sourceCode ? options.sourceCode : "");
            values.emplace_back(options.entryPoint ? options.entryPoint : "");
            values.emplace_back(options.sourcePath ? options.sourcePath : "");
            values.emplace_back(options.targetProfile ? options.targetProfile : "");
            values.emplace_back(options.defines.empty() ? "" : options.defines.front().name + "=" + options.defines.front().value);

            {
                std::lock_guard<std::mutex> lock(*m_observedMutex);
                *m_observedValues = values;
            }

            RVX::ShaderCompileResult result;
            result.success = true;
            result.bytecode.assign(values.front().begin(), values.front().end());
            return result;
        }

    private:
        std::shared_ptr<std::promise<void>> m_entered;
        std::shared_future<void> m_release;
        std::shared_ptr<std::mutex> m_observedMutex;
        std::shared_ptr<std::vector<std::string>> m_observedValues;
    };

    class StaticResultCompiler final : public RVX::IShaderCompiler
    {
    public:
        explicit StaticResultCompiler(RVX::ShaderCompileResult result)
            : m_result(std::move(result))
        {
        }

        RVX::ShaderCompileSupport QuerySupport(
            const RVX::ShaderCompileOptions&) const override
        {
            return RVX::ShaderCompileSupport::Supported();
        }

        RVX::ShaderCompileResult Compile(const RVX::ShaderCompileOptions&) override
        {
            return m_result;
        }

    private:
        RVX::ShaderCompileResult m_result;
    };

    class CapturedShader final : public RVX::RHIShader
    {
    public:
        explicit CapturedShader(const RVX::RHIShaderDesc& desc)
            : RHIShader(desc)
            , m_stage(desc.stage)
        {
            if (desc.bytecode && desc.bytecodeSize > 0)
            {
                const auto* bytes = static_cast<const RVX::uint8*>(desc.bytecode);
                m_bytecode.assign(bytes, bytes + desc.bytecodeSize);
            }
            SetDebugName(desc.debugName);
        }

        RVX::RHIShaderStage GetStage() const override { return m_stage; }
        const std::vector<RVX::uint8>& GetBytecode() const override { return m_bytecode; }

    private:
        RVX::RHIShaderStage m_stage = RVX::RHIShaderStage::None;
        std::vector<RVX::uint8> m_bytecode;
    };

    class CapturingDevice final : public RVX::IRHIDevice
    {
    public:
        explicit CapturingDevice(RVX::RHIBackendType backend)
            : m_backend(backend)
        {
        }

        RVX::RHIShaderRef CreateShader(const RVX::RHIShaderDesc& desc) override
        {
            createShaderCalls++;
            if (desc.bytecode && desc.bytecodeSize > 0)
            {
                lastShaderSource.assign(static_cast<const char*>(desc.bytecode), static_cast<size_t>(desc.bytecodeSize));
            }
            return RVX::MakeRef<CapturedShader>(desc);
        }

        RVX::RHIBufferRef CreateBuffer(const RVX::RHIBufferDesc&) override { return {}; }
        RVX::RHITextureRef CreateTexture(const RVX::RHITextureDesc&) override { return {}; }
        RVX::RHITextureViewRef CreateTextureView(RVX::RHITexture*, const RVX::RHITextureViewDesc&) override { return {}; }
        RVX::RHISamplerRef CreateSampler(const RVX::RHISamplerDesc&) override { return {}; }
        RVX::RHIHeapRef CreateHeap(const RVX::RHIHeapDesc&) override { return {}; }
        RVX::RHITextureRef CreatePlacedTexture(RVX::RHIHeap*, RVX::uint64, const RVX::RHITextureDesc&) override { return {}; }
        RVX::RHIBufferRef CreatePlacedBuffer(RVX::RHIHeap*, RVX::uint64, const RVX::RHIBufferDesc&) override { return {}; }
        MemoryRequirements GetTextureMemoryRequirements(const RVX::RHITextureDesc&) override { return {}; }
        MemoryRequirements GetBufferMemoryRequirements(const RVX::RHIBufferDesc&) override { return {}; }
        RVX::RHIDescriptorSetLayoutRef CreateDescriptorSetLayout(const RVX::RHIDescriptorSetLayoutDesc&) override { return {}; }
        RVX::RHIPipelineLayoutRef CreatePipelineLayout(const RVX::RHIPipelineLayoutDesc&) override { return {}; }
        RVX::RHIPipelineRef CreateGraphicsPipeline(const RVX::RHIGraphicsPipelineDesc&) override { return {}; }
        RVX::RHIPipelineRef CreateComputePipeline(const RVX::RHIComputePipelineDesc&) override { return {}; }
        RVX::RHIDescriptorSetRef CreateDescriptorSet(const RVX::RHIDescriptorSetDesc&) override { return {}; }
        RVX::RHIQueryPoolRef CreateQueryPool(const RVX::RHIQueryPoolDesc&) override { return {}; }
        RVX::RHICommandContextRef CreateCommandContext(RVX::RHICommandQueueType) override { return {}; }
        RVX::uint64 SubmitCommandContext(RVX::RHICommandContext*, RVX::RHIFence*) override { return 0; }
        RVX::uint64 SubmitCommandContexts(std::span<RVX::RHICommandContext* const>, RVX::RHIFence*) override { return 0; }
        RVX::RHISwapChainRef CreateSwapChain(const RVX::RHISwapChainDesc&) override { return {}; }
        RVX::RHIFenceRef CreateFence(RVX::uint64) override { return {}; }
        void WaitForFence(RVX::RHIFence*, RVX::uint64) override {}
        void WaitIdle() override {}
        void BeginFrame() override {}
        void EndFrame() override {}
        RVX::uint32 GetCurrentFrameIndex() const override { return 0; }
        RVX::RHIStagingBufferRef CreateStagingBuffer(const RVX::RHIStagingBufferDesc&) override { return {}; }
        RVX::RHIRingBufferRef CreateRingBuffer(const RVX::RHIRingBufferDesc&) override { return {}; }
        RVX::RHIMemoryStats GetMemoryStats() const override { return {}; }
        void BeginResourceGroup(const char*) override {}
        void EndResourceGroup() override {}
        const RVX::RHICapabilities& GetCapabilities() const override { return m_capabilities; }
        RVX::RHIBackendType GetBackendType() const override { return m_backend; }

        RVX::uint32 createShaderCalls = 0;
        std::string lastShaderSource;

    private:
        RVX::RHIBackendType m_backend = RVX::RHIBackendType::None;
        RVX::RHICapabilities m_capabilities;
    };
}

TEST_F(ShaderCompilerValidationFixture, InvalidCompileOptionsReturnVisibleFailure)
{
    auto compiler = RVX::CreateShaderCompiler();
    ASSERT_NE(compiler, nullptr);

    RVX::ShaderCompileOptions options;
    options.sourceCode = nullptr;
    options.entryPoint = "main";

    RVX::ShaderCompileResult result = compiler->Compile(options);
    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.errorMessage.empty());
}

#if RVX_ENABLE_DX11 || RVX_ENABLE_DX12 || RVX_ENABLE_VULKAN || RVX_ENABLE_OPENGL || RVX_ENABLE_METAL
TEST_F(ShaderCompilerValidationFixture, TransmissionFootprintCompilesAcrossEnabledBackends)
{
    TempDirectory temp("rvx_transmission_footprint_compile");
    const fs::path includePath = fs::path(RVX_SOURCE_DIR) /
        "Source/Runtime/Render/Shaders/Translucency/TransmissionFootprint.hlsli";
    ASSERT_TRUE(fs::is_regular_file(includePath));
    const std::string source = "#include \"" + includePath.generic_string() + "\"\n" + R"(
Texture2D<float> DepthSource : register(t1, space0);
Texture2D<float> DepthPyramid : register(t6, space0);
float4 main(float4 position : SV_POSITION) : SV_TARGET
{
    float2 uv = saturate(position.xy / float2(33.0f, 17.0f));
    float reduced = RVXReduceTransmissionDepth(
        DepthSource, uv, position.x < 8.0f, position.y < 4.0f, false);
    float guarded = RVXSampleTransmissionDepthPyramid(DepthPyramid, uv, position.z + 1.25f);
    return float4(reduced, guarded, 0.0f, 1.0f);
}
)";
    const fs::path productionPath = includePath.parent_path() / "TransmissionDepthPyramid.hlsl";
    const std::string productionSource = ReadTextFile(productionPath);
    const std::string productionPathString = productionPath.string();
    ASSERT_FALSE(productionSource.empty());
    const fs::path path = temp.Path() / "Footprint.hlsl";
    WriteTextFile(path, source);
    const std::string pathString = path.string();
    auto compiler = RVX::CreateShaderCompiler();
    ASSERT_NE(compiler, nullptr);
    std::vector<RVX::RHIBackendType> backends;
#if RVX_ENABLE_DX11
    backends.push_back(RVX::RHIBackendType::DX11);
#endif
#if RVX_ENABLE_DX12
    backends.push_back(RVX::RHIBackendType::DX12);
#endif
#if RVX_ENABLE_VULKAN
    backends.push_back(RVX::RHIBackendType::Vulkan);
#endif
#if RVX_ENABLE_OPENGL
    backends.push_back(RVX::RHIBackendType::OpenGL);
#endif
#if RVX_ENABLE_METAL
    backends.push_back(RVX::RHIBackendType::Metal);
#endif
    ASSERT_FALSE(backends.empty());
    for (const auto backend : backends)
    {
        RVX::ShaderCompileOptions options;
        options.stage = RVX::RHIShaderStage::Pixel;
        options.entryPoint = "main";
        options.sourceCode = source.c_str();
        options.sourcePath = pathString.c_str();
        options.targetBackend = backend;
        options.enableOptimization = false;
        const auto support = compiler->QuerySupport(options);
        ASSERT_TRUE(support.IsSupported()) << support.reason;
        const auto result = compiler->Compile(options);
        EXPECT_TRUE(result.success) << static_cast<int>(backend) << ": " << result.errorMessage;
        options.entryPoint = "PSMain";
        options.sourceCode = productionSource.c_str();
        options.sourcePath = productionPathString.c_str();
        const auto productionResult = compiler->Compile(options);
        EXPECT_TRUE(productionResult.success)
            << static_cast<int>(backend) << ": " << productionResult.errorMessage;
    }
}
#endif

TEST_F(ShaderCompilerValidationFixture, TransparentTemporalRasterCompilesAcrossEnabledBackends)
{
    const fs::path rawPath = fs::path(RVX_SOURCE_DIR) /
        "Source/Runtime/Render/Shaders/PostProcess/TransparentTemporalResolve.hlsl";
    const std::string rawSource = ReadTextFile(rawPath);
    const std::string rawPathString = rawPath.string();
    ASSERT_FALSE(rawSource.empty());
    const fs::path path = fs::path(RVX_SOURCE_DIR) /
        "Source/Runtime/Render/Shaders/Translucency/TransparentTemporalRaster.hlsl";
    const std::string source = ReadTextFile(path);
    const std::string pathString = path.string();
    ASSERT_FALSE(source.empty());
    auto compiler = RVX::CreateShaderCompiler();
    ASSERT_NE(compiler, nullptr);
    std::vector<RVX::RHIBackendType> backends;
#if RVX_ENABLE_DX11
    backends.push_back(RVX::RHIBackendType::DX11);
#endif
#if RVX_ENABLE_DX12
    backends.push_back(RVX::RHIBackendType::DX12);
#endif
#if RVX_ENABLE_VULKAN
    backends.push_back(RVX::RHIBackendType::Vulkan);
#endif
#if RVX_ENABLE_OPENGL
    backends.push_back(RVX::RHIBackendType::OpenGL);
#endif
#if RVX_ENABLE_METAL
    backends.push_back(RVX::RHIBackendType::Metal);
#endif
    ASSERT_FALSE(backends.empty());
    for (const auto backend : backends)
    {
        for (const char* entry : {"VSRigid", "VSSkinned", "PSSorted", "PSWeighted"})
        {
            SCOPED_TRACE(static_cast<int>(backend));
            SCOPED_TRACE(entry);
            RVX::ShaderCompileOptions options;
            options.stage = entry[0] == 'V' ? RVX::RHIShaderStage::Vertex : RVX::RHIShaderStage::Pixel;
            options.entryPoint = entry;
            options.sourceCode = source.c_str();
            options.sourcePath = pathString.c_str();
            options.targetBackend = backend;
            const auto support = compiler->QuerySupport(options);
            ASSERT_TRUE(support.IsSupported()) << support.reason;
            const auto result = compiler->Compile(options);
            EXPECT_TRUE(result.success) << result.errorMessage;
        }
        RVX::ShaderCompileOptions rawOptions;
        rawOptions.stage = RVX::RHIShaderStage::Pixel;
        rawOptions.entryPoint = "PSRaw";
        rawOptions.sourceCode = rawSource.c_str();
        rawOptions.sourcePath = rawPathString.c_str();
        rawOptions.targetBackend = backend;
        const auto rawResult = compiler->Compile(rawOptions);
        EXPECT_TRUE(rawResult.success) << static_cast<int>(backend) << ": " << rawResult.errorMessage;
    }
}

TEST_F(ShaderCompilerValidationFixture, QuerySupportProvidesStructuredFailureForUnsupportedBackend)
{
    auto compiler = RVX::CreateShaderCompiler();
    ASSERT_NE(compiler, nullptr);

    const std::string source = "float4 main() : SV_Target { return 1.0; }";
    RVX::ShaderCompileOptions options;
    options.stage = RVX::RHIShaderStage::Pixel;
    options.sourceCode = source.c_str();
    options.entryPoint = "main";
    options.sourcePath = "UnsupportedBackend.hlsl";
    options.targetBackend = RVX::RHIBackendType::None;

    const RVX::ShaderCompileSupport support = compiler->QuerySupport(options);
    EXPECT_FALSE(support.IsSupported());
    EXPECT_NE(support.code, RVX::ShaderCompileSupportCode::Supported);
    EXPECT_FALSE(support.reason.empty());

    const RVX::ShaderCompileResult result = compiler->Compile(options);
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.errorMessage, support.reason);
}

#if RVX_ENABLE_DX12
TEST_F(ShaderCompilerValidationFixture,
       DX12RayLibraryReflectionSelectsTheRequestedEntryPoint)
{
    auto compiler = RVX::CreateShaderCompiler();
    ASSERT_NE(compiler, nullptr);

    const std::string source = R"(
RWTexture2D<float4> Output : register(u0, space2);

[shader("raygeneration")]
void RayGen()
{
    Output[DispatchRaysIndex().xy] = float4(1.0, 0.5, 0.25, 1.0);
}
)";
    RVX::ShaderCompileOptions options;
    options.stage = RVX::RHIShaderStage::RayGeneration;
    options.sourceCode = source.c_str();
    options.entryPoint = "RayGen";
    options.sourcePath = "RayLibraryReflection.hlsl";
    options.targetProfile = "lib_6_3";
    options.targetBackend = RVX::RHIBackendType::DX12;

    const RVX::ShaderCompileResult result = compiler->Compile(options);
    ASSERT_TRUE(result.success) << result.errorMessage;
    ASSERT_TRUE(result.reflection.valid);
    const auto output = std::find_if(
        result.reflection.resources.begin(),
        result.reflection.resources.end(),
        [](const RVX::ShaderReflection::ResourceBinding& resource)
        {
            return resource.name == "Output";
        });
    ASSERT_NE(output, result.reflection.resources.end());
    EXPECT_EQ(output->set, 2u);
    EXPECT_EQ(output->binding, 0u);
    EXPECT_EQ(output->count, 1u);
    EXPECT_EQ(output->type, RVX::RHIBindingType::StorageTexture);
}
#endif

TEST_F(ShaderCompilerValidationFixture, ShaderManagerUsesInjectedCompiler)
{
    RVX::ShaderCompileResult injectedResult;
    injectedResult.success = true;
    injectedResult.bytecode = {'R', 'V', 'X'};
    injectedResult.reflection.valid = true;
    injectedResult.reflection.resources.push_back(
        {"InjectedConstants", 0, 0, RVX::RHIBindingType::UniformBuffer, 1});
    injectedResult.reflection.inputs.push_back(
        {"POSITION", 0, 0, RVX::RHIFormat::RGB32_FLOAT, false});

    TempDirectory temp("rvx_shader_manager_injection");
    RVX::ShaderManagerConfig config;
    config.cacheDirectory = temp.Path() / "Cache";
    config.enableMemoryCache = false;
    config.enableDiskCache = false;

    RVX::ShaderManager manager(
        config,
        std::make_unique<StaticResultCompiler>(std::move(injectedResult)));
    CapturingDevice device(RVX::RHIBackendType::DX12);

    RVX::ShaderLoadDesc desc;
    desc.path = "InjectedCompiler.hlsl";
    desc.entryPoint = "main";
    desc.stage = RVX::RHIShaderStage::Vertex;
    desc.backend = RVX::RHIBackendType::DX12;

    const RVX::ShaderLoadResult result = manager.LoadFromSource(
        &device,
        desc,
        "float4 main() : SV_Position { return 0.0; }");

    ASSERT_TRUE(result.compileResult.success)
        << result.compileResult.errorMessage;
    ASSERT_NE(result.shader, nullptr);
    EXPECT_EQ(device.createShaderCalls, 1u);
    EXPECT_EQ(device.lastShaderSource, "RVX");
    ASSERT_TRUE(result.shader->HasInterface());
    EXPECT_EQ(
        result.shader->GetInterface().stage,
        RVX::RHIShaderStage::Vertex);
    ASSERT_EQ(result.shader->GetInterface().inputs.size(), 1u);
    EXPECT_EQ(
        result.shader->GetInterface().inputs.front().semanticName,
        "POSITION");
    EXPECT_EQ(result.shader->GetEntryPoint(), "main");
}

TEST_F(ShaderCompilerValidationFixture, AsyncCompileOwnsOptionStrings)
{
    auto entered = std::make_shared<std::promise<void>>();
    auto release = std::make_shared<std::promise<void>>();
    std::future<void> enteredFuture = entered->get_future();
    std::shared_future<void> releaseFuture = release->get_future().share();
    auto observedMutex = std::make_shared<std::mutex>();
    auto observedValues = std::make_shared<std::vector<std::string>>();

    RVX::ShaderCompileService::Config config;
    config.maxConcurrentCompiles = 1;
    auto compiler = std::make_unique<BlockingEchoCompiler>(
        entered,
        releaseFuture,
        observedMutex,
        observedValues);
    RVX::ShaderCompileService service(std::move(compiler), config);

    RVX::CompileHandle handle = RVX::RVX_INVALID_COMPILE_HANDLE;
    {
        std::string source = "original source";
        std::string entryPoint = "main";
        std::string sourcePath = "original.hlsl";
        std::string targetProfile = "vs_6_0";

        RVX::ShaderCompileOptions options;
        options.stage = RVX::RHIShaderStage::Vertex;
        options.sourceCode = source.c_str();
        options.entryPoint = entryPoint.c_str();
        options.sourcePath = sourcePath.c_str();
        options.targetProfile = targetProfile.c_str();
        options.defines.push_back({"RVX_ASYNC_TEST", "1"});

        handle = service.CompileAsync(options);
        ASSERT_EQ(enteredFuture.wait_for(std::chrono::seconds(5)), std::future_status::ready);

        source = "mutated source";
        entryPoint = "changed";
        sourcePath = "changed.hlsl";
        targetProfile = "ps_6_0";
        options.defines.front().value = "2";
    }

    release->set_value();
    RVX::ShaderCompileResult result = service.Wait(handle);
    EXPECT_TRUE(result.success);

    std::lock_guard<std::mutex> lock(*observedMutex);
    ASSERT_EQ(observedValues->size(), 5u);
    EXPECT_EQ((*observedValues)[0], "original source");
    EXPECT_EQ((*observedValues)[1], "main");
    EXPECT_EQ((*observedValues)[2], "original.hlsl");
    EXPECT_EQ((*observedValues)[3], "vs_6_0");
    EXPECT_EQ((*observedValues)[4], "RVX_ASYNC_TEST=1");
}

TEST_F(ShaderCompilerValidationFixture, ShaderSourceInfoDetectsIncludeMutation)
{
    TempDirectory temp("rvx_shader_source_info");
    fs::path mainPath = temp.Path() / "Main.hlsl";
    fs::path includePath = temp.Path() / "Common.hlsli";

    WriteTextFile(mainPath, "float4 main() : SV_Target { return 1; }\n");
    WriteTextFile(includePath, "static const float RVX_VALUE = 1.0;\n");

    RVX::ShaderSourceInfo info;
    info.mainFile = mainPath.string();
    info.fileHashes[info.mainFile] = RVX::ShaderSourceInfo::ComputeFileHash(mainPath);
    info.AddInclude(includePath.string(), RVX::ShaderSourceInfo::ComputeFileHash(includePath));
    info.combinedHash = info.ComputeCombinedHash();

    EXPECT_FALSE(info.HasChanged());

    WriteTextFile(includePath, "static const float RVX_VALUE = 2.0;\n");
    EXPECT_TRUE(info.HasChanged());
}

TEST_F(ShaderCompilerValidationFixture, ShaderCacheHeaderRejectsLegacyVersions)
{
    RVX::ShaderCacheHeader header;
    EXPECT_TRUE(header.IsValid());

    header.version = 1;
    EXPECT_FALSE(header.IsValid());
}

TEST_F(ShaderCompilerValidationFixture, MemoryShaderCacheInvalidatesOnIncludeMutation)
{
    TempDirectory temp("rvx_shader_cache_memory");
    fs::path mainPath = temp.Path() / "Main.hlsl";
    fs::path includePath = temp.Path() / "Common.hlsli";

    WriteTextFile(mainPath, "float4 main() : SV_Target { return RVX_VALUE; }\n");
    WriteTextFile(includePath, "static const float4 RVX_VALUE = 1.0;\n");

    RVX::ShaderCacheManager::Config config;
    config.cacheDirectory = temp.Path() / "Cache";
    config.enableMemoryCache = true;
    config.enableDiskCache = false;
    config.validateOnLoad = true;
    RVX::ShaderCacheManager cache(config);

    RVX::ShaderCacheEntry entry;
    entry.bytecode = {1, 2, 3, 4};
    entry.sourceInfo.mainFile = mainPath.string();
    entry.sourceInfo.fileHashes[entry.sourceInfo.mainFile] = RVX::ShaderSourceInfo::ComputeFileHash(mainPath);
    entry.sourceInfo.AddInclude(includePath.string(), RVX::ShaderSourceInfo::ComputeFileHash(includePath));
    entry.sourceInfo.combinedHash = entry.sourceInfo.ComputeCombinedHash();

    constexpr RVX::uint64 key = 0x1234;
    cache.Save(key, entry);

    ASSERT_TRUE(cache.Load(key).has_value());

    WriteTextFile(includePath, "static const float4 RVX_VALUE = 2.0;\n");
    EXPECT_FALSE(cache.Load(key).has_value());
}

TEST_F(
    ShaderCompilerValidationFixture,
    DiskShaderCacheRoundTripsCompleteInterfaceReflection)
{
    TempDirectory temp("rvx_shader_cache_reflection");

    RVX::ShaderCacheManager::Config config;
    config.cacheDirectory = temp.Path() / "Cache";
    config.enableMemoryCache = true;
    config.enableDiskCache = true;
    config.validateOnLoad = false;
    RVX::ShaderCacheManager cache(config);

    RVX::ShaderCacheEntry entry;
    entry.bytecode = {1, 2, 3, 4};
    entry.backend = RVX::RHIBackendType::Vulkan;
    entry.stage = RVX::RHIShaderStage::Vertex;
    entry.reflection.valid = true;
    entry.reflection.resources.push_back(
        {"ObjectData", 2, 3, RVX::RHIBindingType::StorageBuffer, 4});
    entry.reflection.pushConstants.push_back({16, 32});
    entry.reflection.inputs.push_back(
        {"TEXCOORD", 1, 5, RVX::RHIFormat::RG32_FLOAT, false});
    entry.reflection.outputs.push_back(
        {"SV_POSITION",
         0,
         RVX::RVX_INVALID_INDEX,
         RVX::RHIFormat::RGBA32_FLOAT,
         true});

    constexpr RVX::uint64 key = 0xA11CE;
    cache.Save(key, entry);
    cache.ClearMemoryCache();

    const std::optional<RVX::ShaderCacheEntry> loaded =
        cache.Load(key);
    ASSERT_TRUE(loaded.has_value());
    EXPECT_TRUE(loaded->reflection.valid);
    ASSERT_EQ(loaded->reflection.resources.size(), 1u);
    EXPECT_EQ(loaded->reflection.resources[0].set, 2u);
    EXPECT_EQ(loaded->reflection.resources[0].binding, 3u);
    EXPECT_EQ(loaded->reflection.resources[0].count, 4u);
    ASSERT_EQ(loaded->reflection.pushConstants.size(), 1u);
    EXPECT_EQ(loaded->reflection.pushConstants[0].offset, 16u);
    EXPECT_EQ(loaded->reflection.pushConstants[0].size, 32u);
    ASSERT_EQ(loaded->reflection.inputs.size(), 1u);
    EXPECT_EQ(loaded->reflection.inputs[0].semantic, "TEXCOORD");
    EXPECT_EQ(loaded->reflection.inputs[0].semanticIndex, 1u);
    EXPECT_EQ(loaded->reflection.inputs[0].location, 5u);
    EXPECT_EQ(
        loaded->reflection.inputs[0].format,
        RVX::RHIFormat::RG32_FLOAT);
    EXPECT_FALSE(loaded->reflection.inputs[0].systemValue);
    ASSERT_EQ(loaded->reflection.outputs.size(), 1u);
    EXPECT_TRUE(loaded->reflection.outputs[0].systemValue);
    EXPECT_EQ(
        loaded->reflection.outputs[0].location,
        RVX::RVX_INVALID_INDEX);
}

#if RVX_ENABLE_DX12
TEST_F(ShaderCompilerValidationFixture, DX12CompileProducesReflectionAndSourceInfo)
{
    TempDirectory temp("rvx_shader_compile_dx12");
    fs::path shaderPath = temp.Path() / "Main.hlsl";
    fs::path includePath = temp.Path() / "Common.hlsli";
    std::string source = TestVertexShaderSource();

    WriteTextFile(includePath, "static const float3 RVX_TEST_OFFSET = float3(0.0, 0.0, 0.0);\n");
    WriteTextFile(shaderPath, source);

    auto compiler = RVX::CreateShaderCompiler();
    ASSERT_NE(compiler, nullptr);

    std::string shaderPathString = shaderPath.string();
    RVX::ShaderCompileOptions options;
    options.stage = RVX::RHIShaderStage::Vertex;
    options.entryPoint = "main";
    options.sourceCode = source.c_str();
    options.sourcePath = shaderPathString.c_str();
    options.targetBackend = RVX::RHIBackendType::DX12;
    options.enableOptimization = false;

    const RVX::ShaderCompileSupport support = compiler->QuerySupport(options);
    ASSERT_TRUE(support.IsSupported()) << support.reason;

    RVX::ShaderCompileResult result = compiler->Compile(options);
    ASSERT_TRUE(result.success) << result.errorMessage;
    EXPECT_FALSE(result.bytecode.empty());
    EXPECT_FALSE(result.sourceInfo.IsEmpty());
    EXPECT_FALSE(result.sourceInfo.includeFiles.empty());
    EXPECT_NE(result.sourceInfo.combinedHash, 0u);

    bool sawInclude = false;
    for (const auto& include : result.sourceInfo.includeFiles)
    {
        sawInclude = sawInclude || fs::path(include).filename() == includePath.filename();
    }
    EXPECT_TRUE(sawInclude);

    auto cameraIt = std::find_if(
        result.reflection.resources.begin(),
        result.reflection.resources.end(),
        [](const RVX::ShaderReflection::ResourceBinding& binding)
        {
            return binding.name == "Camera";
        });
    ASSERT_NE(cameraIt, result.reflection.resources.end());
    EXPECT_EQ(cameraIt->set, 0u);
    EXPECT_EQ(cameraIt->binding, 0u);
    EXPECT_EQ(cameraIt->type, RVX::RHIBindingType::UniformBuffer);

    ASSERT_TRUE(result.reflection.valid);
    auto texCoordIt = std::find_if(
        result.reflection.inputs.begin(),
        result.reflection.inputs.end(),
        [](const RVX::ShaderReflection::InputAttribute& input)
        {
            return input.semantic == "TEXCOORD" &&
                   input.semanticIndex == 1;
        });
    ASSERT_NE(texCoordIt, result.reflection.inputs.end());
    EXPECT_EQ(texCoordIt->format, RVX::RHIFormat::RG32_FLOAT);
    EXPECT_FALSE(texCoordIt->systemValue);

    auto blendIndicesIt = std::find_if(
        result.reflection.inputs.begin(),
        result.reflection.inputs.end(),
        [](const RVX::ShaderReflection::InputAttribute& input)
        {
            return input.semantic == "BLENDINDICES" &&
                   input.semanticIndex == 0;
        });
    ASSERT_NE(blendIndicesIt, result.reflection.inputs.end());
    EXPECT_EQ(
        blendIndicesIt->format,
        RVX::RHIFormat::RGBA32_UINT);

    auto positionOutputIt = std::find_if(
        result.reflection.outputs.begin(),
        result.reflection.outputs.end(),
        [](const RVX::ShaderReflection::InputAttribute& output)
        {
            return output.semantic == "SV_POSITION";
        });
    ASSERT_NE(positionOutputIt, result.reflection.outputs.end());
    EXPECT_TRUE(positionOutputIt->systemValue);

    const RVX::RHIShaderInterface shaderInterface =
        RVX::BuildRHIShaderInterface(
            RVX::RHIShaderStage::Vertex,
            result.reflection);
    EXPECT_TRUE(shaderInterface.available);
    EXPECT_NE(shaderInterface.hash, 0u);
    EXPECT_EQ(
        shaderInterface.inputs.size(),
        result.reflection.inputs.size());
    EXPECT_EQ(
        shaderInterface.outputs.size(),
        result.reflection.outputs.size());

    RVX::AutoPipelineLayout layout = RVX::BuildAutoPipelineLayout({{result.reflection, RVX::RHIShaderStage::Vertex}});
    ASSERT_FALSE(layout.setLayouts.empty());
    ASSERT_FALSE(layout.setLayouts[0].entries.empty());
    EXPECT_EQ(layout.setLayouts[0].entries[0].type, RVX::RHIBindingType::UniformBuffer);
}
#endif

#if RVX_ENABLE_VULKAN
TEST_F(
    ShaderCompilerValidationFixture,
    VulkanCompilePreservesPortableInterfaceSemantics)
{
    TempDirectory temp("rvx_shader_compile_vulkan");
    const fs::path shaderPath = temp.Path() / "Main.hlsl";
    const fs::path includePath = temp.Path() / "Common.hlsli";
    const std::string source = TestVertexShaderSource();

    WriteTextFile(
        includePath,
        "static const float3 RVX_TEST_OFFSET = "
        "float3(0.0, 0.0, 0.0);\n");
    WriteTextFile(shaderPath, source);

    auto compiler = RVX::CreateShaderCompiler();
    ASSERT_NE(compiler, nullptr);

    const std::string shaderPathString = shaderPath.string();
    RVX::ShaderCompileOptions options;
    options.stage = RVX::RHIShaderStage::Vertex;
    options.entryPoint = "main";
    options.sourceCode = source.c_str();
    options.sourcePath = shaderPathString.c_str();
    options.targetBackend = RVX::RHIBackendType::Vulkan;
    options.enableOptimization = false;

    const RVX::ShaderCompileSupport support =
        compiler->QuerySupport(options);
    ASSERT_TRUE(support.IsSupported()) << support.reason;

    const RVX::ShaderCompileResult result =
        compiler->Compile(options);
    ASSERT_TRUE(result.success) << result.errorMessage;
    ASSERT_TRUE(result.reflection.valid);

    auto findInput =
        [&result](const char* semantic, RVX::uint32 semanticIndex)
    {
        return std::find_if(
            result.reflection.inputs.begin(),
            result.reflection.inputs.end(),
            [semantic, semanticIndex](
                const RVX::ShaderReflection::InputAttribute& input)
            {
                return input.semantic == semantic &&
                       input.semanticIndex == semanticIndex;
            });
    };

    const auto positionIt = findInput("POSITION", 0);
    ASSERT_NE(positionIt, result.reflection.inputs.end());
    EXPECT_EQ(positionIt->location, 0u);
    EXPECT_EQ(positionIt->format, RVX::RHIFormat::RGB32_FLOAT);

    const auto texCoordIt = findInput("TEXCOORD", 1);
    ASSERT_NE(texCoordIt, result.reflection.inputs.end());
    EXPECT_EQ(texCoordIt->format, RVX::RHIFormat::RG32_FLOAT);

    const auto blendIndicesIt = findInput("BLENDINDICES", 0);
    ASSERT_NE(blendIndicesIt, result.reflection.inputs.end());
    EXPECT_EQ(
        blendIndicesIt->format,
        RVX::RHIFormat::RGBA32_UINT);

    const auto texCoordOutputIt = std::find_if(
        result.reflection.outputs.begin(),
        result.reflection.outputs.end(),
        [](const RVX::ShaderReflection::InputAttribute& output)
        {
            return output.semantic == "TEXCOORD" &&
                   output.semanticIndex == 1;
        });
    ASSERT_NE(texCoordOutputIt, result.reflection.outputs.end());
    EXPECT_EQ(texCoordOutputIt->format, RVX::RHIFormat::RG32_FLOAT);

    const RVX::RHIShaderInterface shaderInterface =
        RVX::BuildRHIShaderInterface(
            RVX::RHIShaderStage::Vertex,
            result.reflection);
    EXPECT_TRUE(shaderInterface.available);
    EXPECT_NE(shaderInterface.hash, 0u);
}

TEST_F(
    ShaderCompilerValidationFixture,
    VulkanCompileUsesPortableExtensionsAndDistinguishesStructuredBuffers)
{
    const std::string source = R"(
StructuredBuffer<uint> gReadOnly : register(t0, space0);
RWStructuredBuffer<uint> gReadWrite : register(u0, space1);

[numthreads(1, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    gReadWrite[dispatchThreadId.x] = gReadOnly[dispatchThreadId.x];
}
)";

    auto compiler = RVX::CreateShaderCompiler();
    ASSERT_NE(compiler, nullptr);

    RVX::ShaderCompileOptions options;
    options.stage = RVX::RHIShaderStage::Compute;
    options.entryPoint = "main";
    options.sourceCode = source.c_str();
    options.targetBackend = RVX::RHIBackendType::Vulkan;
    options.enableOptimization = false;

    const RVX::ShaderCompileSupport support =
        compiler->QuerySupport(options);
    ASSERT_TRUE(support.IsSupported()) << support.reason;

    const RVX::ShaderCompileResult result = compiler->Compile(options);
    ASSERT_TRUE(result.success) << result.errorMessage;
    ASSERT_TRUE(result.reflection.valid);

    const std::string spirvText(
        reinterpret_cast<const char*>(result.bytecode.data()),
        result.bytecode.size());
    EXPECT_EQ(
        spirvText.find("SPV_GOOGLE_hlsl_functionality1"),
        std::string::npos);
    EXPECT_EQ(
        spirvText.find("SPV_GOOGLE_user_type"),
        std::string::npos);

    const auto findResource =
        [&result](const char* name)
        {
            return std::find_if(
                result.reflection.resources.begin(),
                result.reflection.resources.end(),
                [name](const RVX::ShaderReflection::ResourceBinding& resource)
                {
                    return resource.name == name;
                });
        };

    const auto readOnlyIt = findResource("gReadOnly");
    ASSERT_NE(readOnlyIt, result.reflection.resources.end());
    EXPECT_EQ(
        readOnlyIt->type,
        RVX::RHIBindingType::ShaderResourceBuffer);

    const auto readWriteIt = findResource("gReadWrite");
    ASSERT_NE(readWriteIt, result.reflection.resources.end());
    EXPECT_EQ(
        readWriteIt->type,
        RVX::RHIBindingType::StorageBuffer);
}

TEST_F(
    ShaderCompilerValidationFixture,
    VulkanRayTracingLibrariesAllowDescriptorIndexingWithoutRayQuery)
{
    constexpr RVX::uint32 SPIRV_CAPABILITY_RAY_TRACING_KHR = 4479u;
    constexpr RVX::uint32 SPIRV_CAPABILITY_RAY_QUERY_KHR = 4472u;
    constexpr RVX::uint32 SPIRV_CAPABILITY_SHADER_NON_UNIFORM_EXT = 5301u;

    const std::string rayTracingSource = R"(
RaytracingAccelerationStructure Scene : register(t0, space0);
Texture2D<float4> MaterialTextures[] : register(t1, space0);
RWTexture2D<float4> Output : register(u0, space0);

struct RayPayload
{
    float3 radiance;
};

[shader("raygeneration")]
void RayGen()
{
    const uint materialTextureIndex =
        NonUniformResourceIndex(DispatchRaysIndex().x & 1u);
    RayDesc ray;
    ray.Origin = float3(0.0f, 0.0f, 0.0f);
    ray.Direction = float3(0.0f, 0.0f, 1.0f);
    ray.TMin = 0.001f;
    ray.TMax = 1000.0f;

    RayPayload payload = (RayPayload)0;
    TraceRay(Scene, RAY_FLAG_NONE, 0xff, 0, 0, 0, ray, payload);
    Output[DispatchRaysIndex().xy] =
        MaterialTextures[materialTextureIndex].Load(int3(0, 0, 0)) +
        float4(payload.radiance, 1.0f);
}
)";

    auto compiler = RVX::CreateShaderCompiler();
    ASSERT_NE(compiler, nullptr);

    RVX::ShaderCompileOptions rayTracingOptions;
    rayTracingOptions.stage = RVX::RHIShaderStage::RayGeneration;
    rayTracingOptions.entryPoint = "RayGen";
    rayTracingOptions.sourceCode = rayTracingSource.c_str();
    rayTracingOptions.sourcePath = "VulkanRayTracingDescriptorIndexing.hlsl";
    rayTracingOptions.targetProfile = "lib_6_3";
    rayTracingOptions.targetBackend = RVX::RHIBackendType::Vulkan;
    rayTracingOptions.enableOptimization = false;

    const RVX::ShaderCompileSupport rayTracingSupport =
        compiler->QuerySupport(rayTracingOptions);
    ASSERT_TRUE(rayTracingSupport.IsSupported()) << rayTracingSupport.reason;

    const RVX::ShaderCompileResult rayTracingResult =
        compiler->Compile(rayTracingOptions);
    ASSERT_TRUE(rayTracingResult.success) << rayTracingResult.errorMessage;
    ASSERT_TRUE(HasSPIRVHeader(rayTracingResult.bytecode));
    EXPECT_TRUE(HasSPIRVExtension(
        rayTracingResult.bytecode,
        "SPV_KHR_ray_tracing"));
    EXPECT_TRUE(HasSPIRVExtension(
        rayTracingResult.bytecode,
        "SPV_EXT_descriptor_indexing"));
    EXPECT_FALSE(HasSPIRVExtension(
        rayTracingResult.bytecode,
        "SPV_KHR_ray_query"));
    EXPECT_TRUE(HasSPIRVCapability(
        rayTracingResult.bytecode,
        SPIRV_CAPABILITY_RAY_TRACING_KHR));
    EXPECT_TRUE(HasSPIRVCapability(
        rayTracingResult.bytecode,
        SPIRV_CAPABILITY_SHADER_NON_UNIFORM_EXT));
    EXPECT_FALSE(HasSPIRVCapability(
        rayTracingResult.bytecode,
        SPIRV_CAPABILITY_RAY_QUERY_KHR));

    const std::string rasterSource = R"(
[numthreads(1, 1, 1)]
void main()
{
}
)";
    RVX::ShaderCompileOptions rasterOptions;
    rasterOptions.stage = RVX::RHIShaderStage::Compute;
    rasterOptions.entryPoint = "main";
    rasterOptions.sourceCode = rasterSource.c_str();
    rasterOptions.sourcePath = "VulkanRasterBaseline.hlsl";
    rasterOptions.targetBackend = RVX::RHIBackendType::Vulkan;
    rasterOptions.enableOptimization = false;

    const RVX::ShaderCompileResult rasterResult = compiler->Compile(rasterOptions);
    ASSERT_TRUE(rasterResult.success) << rasterResult.errorMessage;
    ASSERT_TRUE(HasSPIRVHeader(rasterResult.bytecode));
    EXPECT_FALSE(HasSPIRVExtension(
        rasterResult.bytecode,
        "SPV_KHR_ray_tracing"));
    EXPECT_FALSE(HasSPIRVExtension(
        rasterResult.bytecode,
        "SPV_EXT_descriptor_indexing"));
    EXPECT_FALSE(HasSPIRVCapability(
        rasterResult.bytecode,
        SPIRV_CAPABILITY_RAY_TRACING_KHR));
    EXPECT_FALSE(HasSPIRVCapability(
        rasterResult.bytecode,
        SPIRV_CAPABILITY_SHADER_NON_UNIFORM_EXT));
}
#endif

#if RVX_ENABLE_OPENGL
TEST_F(
    ShaderCompilerValidationFixture,
    OpenGLReflectionDistinguishesReadOnlyAndWritableStructuredBuffers)
{
    const std::string source = R"(
StructuredBuffer<uint> gReadOnly : register(t0, space0);
RWStructuredBuffer<uint> gReadWrite : register(u2, space0);

[numthreads(1, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    gReadWrite[dispatchThreadId.x] = gReadOnly[dispatchThreadId.x];
}
)";

    auto compiler = RVX::CreateShaderCompiler();
    ASSERT_NE(compiler, nullptr);

    RVX::ShaderCompileOptions options;
    options.stage = RVX::RHIShaderStage::Compute;
    options.entryPoint = "main";
    options.sourceCode = source.c_str();
    options.sourcePath = "OpenGLStructuredBufferReflection.hlsl";
    options.targetBackend = RVX::RHIBackendType::OpenGL;
    options.enableOptimization = false;

    const RVX::ShaderCompileSupport support = compiler->QuerySupport(options);
    ASSERT_TRUE(support.IsSupported()) << support.reason;

    const RVX::ShaderCompileResult result = compiler->Compile(options);
    ASSERT_TRUE(result.success) << result.errorMessage;

    const auto findResource =
        [&result](const char* name)
        {
            return std::find_if(
                result.reflection.resources.begin(),
                result.reflection.resources.end(),
                [name](const RVX::ShaderReflection::ResourceBinding& resource)
                {
                    return resource.name == name;
                });
        };
    const auto readOnlyIt = findResource("gReadOnly");
    ASSERT_NE(readOnlyIt, result.reflection.resources.end());
    EXPECT_EQ(readOnlyIt->type, RVX::RHIBindingType::ShaderResourceBuffer);
    EXPECT_EQ(readOnlyIt->count, 1u);
    const auto readWriteIt = findResource("gReadWrite");
    ASSERT_NE(readWriteIt, result.reflection.resources.end());
    EXPECT_EQ(readWriteIt->type, RVX::RHIBindingType::StorageBuffer);
    EXPECT_EQ(readWriteIt->count, 1u);
}

TEST_F(
    ShaderCompilerValidationFixture,
    OpenGLReflectionPreservesStaticSeparateImageDescriptorArrayCount)
{
    const std::string source = R"(
Texture2D<float4> FixedTextures[3] : register(t4, space0);

float4 main(float4 position : SV_Position) : SV_Target
{
    return FixedTextures[0].Load(int3(0, 0, 0)) +
           FixedTextures[1].Load(int3(0, 0, 0)) +
           FixedTextures[2].Load(int3(0, 0, 0));
}
)";

    auto compiler = RVX::CreateShaderCompiler();
    ASSERT_NE(compiler, nullptr);

    RVX::ShaderCompileOptions options;
    options.stage = RVX::RHIShaderStage::Pixel;
    options.entryPoint = "main";
    options.sourceCode = source.c_str();
    options.sourcePath = "OpenGLStaticDescriptorArrayReflection.hlsl";
    options.targetBackend = RVX::RHIBackendType::OpenGL;
    options.enableOptimization = false;

    const RVX::ShaderCompileSupport support = compiler->QuerySupport(options);
    ASSERT_TRUE(support.IsSupported()) << support.reason;

    const RVX::ShaderCompileResult result = compiler->Compile(options);
    ASSERT_TRUE(result.success) << result.errorMessage;
    ASSERT_TRUE(result.reflection.valid);

    const auto texture = std::find_if(
        result.reflection.resources.begin(),
        result.reflection.resources.end(),
        [](const RVX::ShaderReflection::ResourceBinding& resource)
        {
            return resource.name == "FixedTextures";
        });
    ASSERT_NE(texture, result.reflection.resources.end());
    EXPECT_EQ(texture->set, 0u);
    EXPECT_EQ(texture->binding, 4u);
    EXPECT_EQ(texture->type, RVX::RHIBindingType::SampledTexture);
    EXPECT_EQ(texture->count, 3u);
}
#endif

#if RVX_ENABLE_DX11
TEST_F(ShaderCompilerValidationFixture, DX11DefaultProfileSupportsRegisterSpaces)
{
    const std::string source = R"(
cbuffer Camera : register(b0, space0)
{
    float4x4 gViewProjection;
};

cbuffer Object : register(b0, space1)
{
    float4 gOffset;
};

struct VSInput
{
    float3 position : POSITION;
};

struct VSOutput
{
    float4 position : SV_POSITION;
};

VSOutput main(VSInput input)
{
    VSOutput output;
    output.position = mul(gViewProjection, float4(input.position + gOffset.xyz, 1.0));
    return output;
}
)";

    auto compiler = RVX::CreateShaderCompiler();
    ASSERT_NE(compiler, nullptr);

    RVX::ShaderCompileOptions options;
    options.stage = RVX::RHIShaderStage::Vertex;
    options.entryPoint = "main";
    options.sourceCode = source.c_str();
    options.sourcePath = "DX11RegisterSpaceTest.hlsl";
    options.targetBackend = RVX::RHIBackendType::DX11;
    options.enableOptimization = false;

    const RVX::ShaderCompileSupport support = compiler->QuerySupport(options);
    ASSERT_TRUE(support.IsSupported()) << support.reason;

    RVX::ShaderCompileResult result = compiler->Compile(options);
    ASSERT_TRUE(result.success) << result.errorMessage;
    EXPECT_FALSE(result.bytecode.empty());

    auto objectIt = std::find_if(
        result.reflection.resources.begin(),
        result.reflection.resources.end(),
        [](const RVX::ShaderReflection::ResourceBinding& binding)
        {
            return binding.name == "Object";
        });
    ASSERT_NE(objectIt, result.reflection.resources.end());
    EXPECT_EQ(objectIt->set, 1u);
    EXPECT_EQ(objectIt->binding, 0u);
}

TEST_F(ShaderCompilerValidationFixture,
       DX11SamplerABIRoundTripsFrameAndCompactMaterialSamplersWithoutCollisions)
{
    const std::string source = R"(
Texture2D<float4> FrameTexture : register(t1, space0);
SamplerState FrameSampler0 : register(s0);
SamplerState FrameSampler1 : register(s1);
SamplerState FrameSampler2 : register(s2, space0);
SamplerState FrameSampler5 : register(s5, space0);
SamplerState FrameSampler7 : register(s7, space0);
SamplerState FrameSampler9 : register(s9, space0);
SamplerState FrameSampler13 : register(s13, space0);
SamplerState FrameSampler19 : register(s19, space0);

Texture2D<float4> MaterialTexture1 : register(t1, space2);
Texture2D<float4> MaterialTexture2 : register(t2, space2);
Texture2D<float4> MaterialTexture3 : register(t3, space2);
Texture2D<float4> MaterialTexture4 : register(t4, space2);
Texture2D<float4> MaterialTexture5 : register(t5, space2);
Texture2D<float4> MaterialTexture6 : register(t6, space2);
Texture2D<float4> MaterialTexture7 : register(t7, space2);
Texture2D<float4> MaterialTexture8 : register(t8, space2);
SamplerState MaterialSampler14 : register(s14, space2);
SamplerState MaterialSampler15 : register(s15, space2);
SamplerState MaterialSampler16 : register(s16, space2);
SamplerState MaterialSampler17 : register(s17, space2);
SamplerState MaterialSampler18 : register(s18, space2);
SamplerState MaterialSampler19 : register(s19, space2);
SamplerState MaterialSampler20 : register(s20, space2);
SamplerState MaterialSampler21 : register(s21, space2);

float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    float4 value = FrameTexture.Sample(FrameSampler0, uv);
    value += FrameTexture.Sample(FrameSampler1, uv);
    value += FrameTexture.Sample(FrameSampler2, uv);
    value += FrameTexture.Sample(FrameSampler5, uv);
    value += FrameTexture.Sample(FrameSampler7, uv);
    value += FrameTexture.Sample(FrameSampler9, uv);
    value += FrameTexture.Sample(FrameSampler13, uv);
    value += FrameTexture.Sample(FrameSampler19, uv);
    value += MaterialTexture1.Sample(MaterialSampler14, uv);
    value += MaterialTexture2.Sample(MaterialSampler15, uv);
    value += MaterialTexture3.Sample(MaterialSampler16, uv);
    value += MaterialTexture4.Sample(MaterialSampler17, uv);
    value += MaterialTexture5.Sample(MaterialSampler18, uv);
    value += MaterialTexture6.Sample(MaterialSampler19, uv);
    value += MaterialTexture7.Sample(MaterialSampler20, uv);
    value += MaterialTexture8.Sample(MaterialSampler21, uv);
    return value;
}
)";

    auto compiler = RVX::CreateShaderCompiler();
    ASSERT_NE(compiler, nullptr);

    RVX::ShaderCompileOptions options;
    options.stage = RVX::RHIShaderStage::Pixel;
    options.entryPoint = "main";
    options.sourceCode = source.c_str();
    options.sourcePath = "DX11SamplerABITest.hlsl";
    options.targetBackend = RVX::RHIBackendType::DX11;
    options.enableOptimization = false;

    const RVX::ShaderCompileSupport support = compiler->QuerySupport(options);
    ASSERT_TRUE(support.IsSupported()) << support.reason;

    const RVX::ShaderCompileResult result = compiler->Compile(options);
    ASSERT_TRUE(result.success) << result.errorMessage;
    EXPECT_FALSE(result.bytecode.empty());

    const auto expectSampler = [&result](const char* name,
                                         RVX::uint32 set,
                                         RVX::uint32 binding)
    {
        const auto found = std::find_if(
            result.reflection.resources.begin(),
            result.reflection.resources.end(),
            [name](const RVX::ShaderReflection::ResourceBinding& resource)
            {
                return resource.name == name;
            });
        ASSERT_NE(result.reflection.resources.end(), found);
        EXPECT_EQ(RVX::RHIBindingType::Sampler, found->type);
        EXPECT_EQ(set, found->set);
        EXPECT_EQ(binding, found->binding);
    };
    const std::array<std::pair<const char*, RVX::uint32>, 8>
        frameSamplers = {{{"FrameSampler0", 0u},
                          {"FrameSampler1", 1u},
                          {"FrameSampler2", 2u},
                          {"FrameSampler5", 5u},
                          {"FrameSampler7", 7u},
                          {"FrameSampler9", 9u},
                          {"FrameSampler13", 13u},
                          {"FrameSampler19", 19u}}};
    for (const auto& sampler : frameSamplers)
    {
        expectSampler(sampler.first, 0u, sampler.second);
    }

    const std::array<std::pair<const char*, RVX::uint32>, 8>
        materialSamplers = {{{"MaterialSampler14", 14u},
                              {"MaterialSampler15", 15u},
                              {"MaterialSampler16", 16u},
                              {"MaterialSampler17", 17u},
                              {"MaterialSampler18", 18u},
                              {"MaterialSampler19", 19u},
                              {"MaterialSampler20", 20u},
                              {"MaterialSampler21", 21u}}};
    for (const auto& sampler : materialSamplers)
    {
        expectSampler(sampler.first, 2u, sampler.second);
    }

    const auto expectTexture = [&result](const char* name,
                                         RVX::uint32 set,
                                         RVX::uint32 binding)
    {
        const auto found = std::find_if(
            result.reflection.resources.begin(),
            result.reflection.resources.end(),
            [name](const RVX::ShaderReflection::ResourceBinding& resource)
            {
                return resource.name == name;
            });
        ASSERT_NE(result.reflection.resources.end(), found);
        EXPECT_EQ(RVX::RHIBindingType::SampledTexture, found->type);
        EXPECT_EQ(set, found->set);
        EXPECT_EQ(binding, found->binding);
    };
    expectTexture("FrameTexture", 0, 1);
    expectTexture("MaterialTexture1", 2, 1);
    expectTexture("MaterialTexture2", 2, 2);
    expectTexture("MaterialTexture3", 2, 3);
    expectTexture("MaterialTexture4", 2, 4);
    expectTexture("MaterialTexture5", 2, 5);
    expectTexture("MaterialTexture6", 2, 6);
    expectTexture("MaterialTexture7", 2, 7);
    expectTexture("MaterialTexture8", 2, 8);
}

TEST_F(ShaderCompilerValidationFixture,
       DX11SamplerABIRejectsUnsupportedLogicalSetOrBinding)
{
    auto compiler = RVX::CreateShaderCompiler();
    ASSERT_NE(compiler, nullptr);

    const auto expectRejected = [&compiler](const char* source,
                                             const char* sourcePath,
                                             const char* expectedFragment)
    {
        RVX::ShaderCompileOptions options;
        options.stage = RVX::RHIShaderStage::Pixel;
        options.entryPoint = "main";
        options.sourceCode = source;
        options.sourcePath = sourcePath;
        options.targetBackend = RVX::RHIBackendType::DX11;
        options.enableOptimization = false;

        const RVX::ShaderCompileResult result = compiler->Compile(options);
        EXPECT_FALSE(result.success);
        EXPECT_NE(result.errorMessage.find("DX11 sampler ABI rejects"),
                  std::string::npos);
        EXPECT_NE(result.errorMessage.find(expectedFragment),
                  std::string::npos);
    };

    expectRejected(R"(
Texture2D<float4> SourceTexture : register(t1, space0);
SamplerState UnsupportedFrameSampler : register(s6, space0);
float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    return SourceTexture.Sample(UnsupportedFrameSampler, uv);
}
)",
                   "DX11UnsupportedFrameSampler.hlsl",
                   "set 0 logical sampler s6");

    expectRejected(R"(
Texture2D<float4> SourceTexture : register(t1, space1);
SamplerState UnsupportedSetSampler : register(s9, space1);
float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    return SourceTexture.Sample(UnsupportedSetSampler, uv);
}
)",
                   "DX11UnsupportedSetSampler.hlsl",
                   "set 1 logical sampler s9");
}

TEST_F(ShaderCompilerValidationFixture,
       DX11DefaultLitPixelReflectionPreservesCompactMaterialSamplerABI)
{
    const fs::path shaderPath =
        fs::path(RVX_SOURCE_DIR) / "Source" / "Runtime" / "Render" / "Shaders" /
        "DefaultLit.hlsl";
    std::ifstream shaderFile(shaderPath, std::ios::binary);
    ASSERT_TRUE(shaderFile.is_open()) << shaderPath.string();
    const std::string source{
        std::istreambuf_iterator<char>(shaderFile),
        std::istreambuf_iterator<char>()};

    auto compiler = RVX::CreateShaderCompiler();
    ASSERT_NE(compiler, nullptr);

    const std::string shaderPathString = shaderPath.string();
    RVX::ShaderCompileOptions options;
    options.stage = RVX::RHIShaderStage::Pixel;
    options.entryPoint = "PSMain";
    options.sourceCode = source.c_str();
    options.sourcePath = shaderPathString.c_str();
    options.targetBackend = RVX::RHIBackendType::DX11;
    options.targetProfile = "ps_5_0";
    options.enableOptimization = false;

    const std::string samplerInclude = ReadTextFile(
        shaderPath.parent_path() / "Include" / "MaterialSamplers.hlsli");
    ASSERT_FALSE(samplerInclude.empty());
    EXPECT_NE(source.find("#include \"Include/MaterialSamplers.hlsli\""),
              std::string::npos);
    EXPECT_NE(samplerInclude.find("SamplerState BaseColorSampler : register(s8);"),
              std::string::npos);
    EXPECT_NE(samplerInclude.find("SamplerState ThicknessSampler : register(s15);"),
              std::string::npos);
    EXPECT_NE(samplerInclude.find("#define SpecularSampler MetallicRoughnessSampler"),
              std::string::npos);
    EXPECT_NE(samplerInclude.find("#define SpecularColorSampler BaseColorSampler"),
              std::string::npos);
    EXPECT_NE(samplerInclude.find("#define ClearcoatNormalSampler NormalSampler"),
              std::string::npos);

    const RVX::ShaderCompileSupport support = compiler->QuerySupport(options);
    ASSERT_TRUE(support.IsSupported()) << support.reason;

    const RVX::ShaderCompileResult result = compiler->Compile(options);
    ASSERT_TRUE(result.success) << result.errorMessage;
    const auto expectSamplerBinding = [](
                                         const RVX::ShaderCompileResult& compilation,
                                         const char* name,
                                         RVX::uint32 logicalBinding)
    {
        const auto resource = std::find_if(
            compilation.reflection.resources.begin(),
            compilation.reflection.resources.end(),
            [name](const RVX::ShaderReflection::ResourceBinding& binding)
            {
                return binding.name == name;
            });
        ASSERT_NE(compilation.reflection.resources.end(), resource);
        EXPECT_EQ(RVX::RHIBindingType::Sampler, resource->type);
        EXPECT_EQ(2u, resource->set);
        EXPECT_EQ(logicalBinding, resource->binding);
    };
    expectSamplerBinding(result, "BaseColorSampler", 14u);
    expectSamplerBinding(result, "NormalSampler", 15u);
    expectSamplerBinding(result, "MetallicRoughnessSampler", 16u);
    expectSamplerBinding(result, "OcclusionSampler", 17u);
    expectSamplerBinding(result, "EmissiveSampler", 18u);

    options.entryPoint = "PSForwardTranslucencyGuarded";
    // FXC's unoptimized SM5 validator exhausts an internal temporary on the
    // full Forward+ translucent path after Material V2 adds its layered BRDF
    // locals. Shipping DX11 compilation is optimized; exercise that exact
    // production path rather than treating a debug-validator limitation as a
    // material ABI failure.
    options.enableOptimization = true;
    const RVX::ShaderCompileResult translucentResult = compiler->Compile(options);
    ASSERT_TRUE(translucentResult.success) << translucentResult.errorMessage;
    expectSamplerBinding(translucentResult, "TransmissionSampler", 20u);
    expectSamplerBinding(translucentResult, "ThicknessSampler", 21u);
}

#endif

#if RVX_ENABLE_DX12
TEST_F(ShaderCompilerValidationFixture,
       DeferredLightingCompilesExactReflectionReplacementUAVsAsDXIL)
{
    const fs::path shaderPath =
        fs::path(RVX_SOURCE_DIR) / "Source" / "Runtime" / "Render" /
        "Shaders" / "DeferredLighting.hlsl";
    const std::string source = ReadTextFile(shaderPath);
    ASSERT_FALSE(source.empty()) << shaderPath.string();

    auto compiler = RVX::CreateShaderCompiler();
    ASSERT_NE(compiler, nullptr);

    const std::string shaderPathString = shaderPath.string();
    RVX::ShaderCompileOptions options;
    options.stage = RVX::RHIShaderStage::Compute;
    options.entryPoint = "CSMain";
    options.sourceCode = source.c_str();
    options.sourcePath = shaderPathString.c_str();
    options.targetBackend = RVX::RHIBackendType::DX12;
    options.enableOptimization = false;

    const RVX::ShaderCompileSupport support = compiler->QuerySupport(options);
    ASSERT_TRUE(support.IsSupported()) << support.reason;
    const RVX::ShaderCompileResult result = compiler->Compile(options);
    ASSERT_TRUE(result.success) << result.errorMessage;
    ASSERT_TRUE(result.reflection.valid);
    EXPECT_TRUE(HasDXILContainerHeader(result.bytecode));

    const auto expectUAV = [&result](const char* name, RVX::uint32 binding)
    {
        const auto resource = std::find_if(
            result.reflection.resources.begin(), result.reflection.resources.end(),
            [name](const RVX::ShaderReflection::ResourceBinding& candidate)
            {
                return candidate.name == name;
            });
        ASSERT_NE(result.reflection.resources.end(), resource) << name;
        EXPECT_EQ(0u, resource->set) << name;
        EXPECT_EQ(binding, resource->binding) << name;
        EXPECT_EQ(RVX::RHIBindingType::StorageTexture, resource->type) << name;
    };
    expectUAV("BaseIndirectSpecular", 24u);
    expectUAV("SpecularResponse", 25u);
}

TEST_F(ShaderCompilerValidationFixture,
       GFX06SurfaceShadersCompileAsDXIL)
{
    auto compiler = RVX::CreateShaderCompiler();
    ASSERT_NE(compiler, nullptr);

    RVX::ShaderCompileOptions supportOptions;
    supportOptions.stage = RVX::RHIShaderStage::Pixel;
    supportOptions.entryPoint = "PSMain";
    supportOptions.sourceCode = "float4 PSMain() : SV_TARGET { return 0.0f; }";
    supportOptions.targetBackend = RVX::RHIBackendType::DX12;

    const RVX::ShaderCompileSupport support =
        compiler->QuerySupport(supportOptions);
    ASSERT_TRUE(support.IsSupported()) << support.reason;

    VerifyGFX06SurfaceShaderMatrix(*compiler, RVX::RHIBackendType::DX12);
}

TEST_F(ShaderCompilerValidationFixture,
       GFX19SSRShadersCompileAsDXILWithProbeFallbackABI)
{
    auto compiler = RVX::CreateShaderCompiler();
    ASSERT_NE(compiler, nullptr);

    VerifyGFX19SSRShaderMatrix(*compiler, RVX::RHIBackendType::DX12);
}

TEST_F(ShaderCompilerValidationFixture,
       GFX22WeightedBlendedOITShadersCompileAsDXIL)
{
    auto compiler = RVX::CreateShaderCompiler();
    ASSERT_NE(compiler, nullptr);
    VerifyGFX22WeightedBlendedOITShaderMatrix(
        *compiler, RVX::RHIBackendType::DX12);
}

TEST_F(ShaderCompilerValidationFixture,
       GFX24TAAShadersCompileAsDXILWithDescriptorABI)
{
    auto compiler = RVX::CreateShaderCompiler();
    ASSERT_NE(compiler, nullptr);
    VerifyGFX24TAAShaderMatrix(*compiler, RVX::RHIBackendType::DX12);
}

TEST_F(ShaderCompilerValidationFixture,
       OpaqueSceneColorPyramidShadersCompileAsDXILWithDescriptorABI)
{
    auto compiler = RVX::CreateShaderCompiler();
    ASSERT_NE(compiler, nullptr);
    VerifyOpaqueSceneColorPyramidShaderMatrix(
        *compiler, RVX::RHIBackendType::DX12);
}
#endif

#if RVX_ENABLE_DX11
TEST_F(ShaderCompilerValidationFixture,
       GFX06SurfaceShadersCompileAsDXBC)
{
    auto compiler = RVX::CreateShaderCompiler();
    ASSERT_NE(compiler, nullptr);

    RVX::ShaderCompileOptions supportOptions;
    supportOptions.stage = RVX::RHIShaderStage::Pixel;
    supportOptions.entryPoint = "PSMain";
    supportOptions.sourceCode = "float4 PSMain() : SV_TARGET { return 0.0f; }";
    supportOptions.targetBackend = RVX::RHIBackendType::DX11;

    const RVX::ShaderCompileSupport support =
        compiler->QuerySupport(supportOptions);
    ASSERT_TRUE(support.IsSupported()) << support.reason;

    VerifyGFX06SurfaceShaderMatrix(*compiler, RVX::RHIBackendType::DX11);
}

TEST_F(ShaderCompilerValidationFixture,
       GFX22WeightedBlendedOITShadersCompileAsDXBC)
{
    auto compiler = RVX::CreateShaderCompiler();
    ASSERT_NE(compiler, nullptr);
    VerifyGFX22WeightedBlendedOITShaderMatrix(
        *compiler, RVX::RHIBackendType::DX11);
}

TEST_F(ShaderCompilerValidationFixture,
       GFX24TAAShadersCompileAsDXBCWithDescriptorABI)
{
    auto compiler = RVX::CreateShaderCompiler();
    ASSERT_NE(compiler, nullptr);
    VerifyGFX24TAAShaderMatrix(*compiler, RVX::RHIBackendType::DX11);
}

TEST_F(ShaderCompilerValidationFixture,
       OpaqueSceneColorPyramidShadersCompileAsDXBCWithDescriptorABI)
{
    auto compiler = RVX::CreateShaderCompiler();
    ASSERT_NE(compiler, nullptr);
    VerifyOpaqueSceneColorPyramidShaderMatrix(
        *compiler, RVX::RHIBackendType::DX11);
}
#endif

#if RVX_ENABLE_VULKAN
TEST_F(ShaderCompilerValidationFixture,
       GFX06SurfaceShadersCompileAsSPIRV)
{
    auto compiler = RVX::CreateShaderCompiler();
    ASSERT_NE(compiler, nullptr);

    RVX::ShaderCompileOptions supportOptions;
    supportOptions.stage = RVX::RHIShaderStage::Pixel;
    supportOptions.entryPoint = "PSMain";
    supportOptions.sourceCode = "float4 PSMain() : SV_TARGET { return 0.0f; }";
    supportOptions.targetBackend = RVX::RHIBackendType::Vulkan;

    const RVX::ShaderCompileSupport support =
        compiler->QuerySupport(supportOptions);
    ASSERT_TRUE(support.IsSupported()) << support.reason;

    VerifyGFX06SurfaceShaderMatrix(*compiler, RVX::RHIBackendType::Vulkan);
}

TEST_F(ShaderCompilerValidationFixture,
       GFX19SSRShadersCompileAsSPIRVWithProbeFallbackABI)
{
    auto compiler = RVX::CreateShaderCompiler();
    ASSERT_NE(compiler, nullptr);

    VerifyGFX19SSRShaderMatrix(*compiler, RVX::RHIBackendType::Vulkan);
}

TEST_F(ShaderCompilerValidationFixture,
       GFX22WeightedBlendedOITShadersCompileAsSPIRV)
{
    auto compiler = RVX::CreateShaderCompiler();
    ASSERT_NE(compiler, nullptr);
    VerifyGFX22WeightedBlendedOITShaderMatrix(
        *compiler, RVX::RHIBackendType::Vulkan);
}

TEST_F(ShaderCompilerValidationFixture,
       GFX24TAAShadersCompileAsSPIRVWithDescriptorABI)
{
    auto compiler = RVX::CreateShaderCompiler();
    ASSERT_NE(compiler, nullptr);
    VerifyGFX24TAAShaderMatrix(*compiler, RVX::RHIBackendType::Vulkan);
}

TEST_F(ShaderCompilerValidationFixture,
       OpaqueSceneColorPyramidShadersCompileAsSPIRVWithDescriptorABI)
{
    auto compiler = RVX::CreateShaderCompiler();
    ASSERT_NE(compiler, nullptr);
    VerifyOpaqueSceneColorPyramidShaderMatrix(
        *compiler, RVX::RHIBackendType::Vulkan);
}
#endif

#if RVX_ENABLE_OPENGL
TEST_F(ShaderCompilerValidationFixture,
       GFX06SurfaceShadersCompileAsOpenGLGLSL)
{
    auto compiler = RVX::CreateShaderCompiler();
    ASSERT_NE(compiler, nullptr);

    RVX::ShaderCompileOptions supportOptions;
    supportOptions.stage = RVX::RHIShaderStage::Pixel;
    supportOptions.entryPoint = "PSMain";
    supportOptions.sourceCode = "float4 PSMain() : SV_TARGET { return 0.0f; }";
    supportOptions.targetBackend = RVX::RHIBackendType::OpenGL;

    const RVX::ShaderCompileSupport support =
        compiler->QuerySupport(supportOptions);
    ASSERT_TRUE(support.IsSupported()) << support.reason;

    VerifyGFX06SurfaceShaderMatrix(*compiler, RVX::RHIBackendType::OpenGL);
}

TEST_F(ShaderCompilerValidationFixture,
       GFX19SSRShadersCompileAsOpenGLGLSLWithMaterializedReflectionABI)
{
    auto compiler = RVX::CreateShaderCompiler();
    ASSERT_NE(compiler, nullptr);

    VerifyGFX19SSRShaderMatrix(*compiler, RVX::RHIBackendType::OpenGL);
}

TEST_F(ShaderCompilerValidationFixture,
       GFX22WeightedBlendedOITShadersCompileAsOpenGLGLSL)
{
    auto compiler = RVX::CreateShaderCompiler();
    ASSERT_NE(compiler, nullptr);
    VerifyGFX22WeightedBlendedOITShaderMatrix(
        *compiler, RVX::RHIBackendType::OpenGL);
}

TEST_F(ShaderCompilerValidationFixture,
       GFX24TAAShadersCompileAsOpenGLGLSLWithDescriptorABI)
{
    auto compiler = RVX::CreateShaderCompiler();
    ASSERT_NE(compiler, nullptr);
    VerifyGFX24TAAShaderMatrix(*compiler, RVX::RHIBackendType::OpenGL);
}

TEST_F(ShaderCompilerValidationFixture,
       OpaqueSceneColorPyramidShadersCompileAsOpenGLGLSLWithDescriptorABI)
{
    auto compiler = RVX::CreateShaderCompiler();
    ASSERT_NE(compiler, nullptr);
    VerifyOpaqueSceneColorPyramidShaderMatrix(
        *compiler, RVX::RHIBackendType::OpenGL);
}

TEST_F(ShaderCompilerValidationFixture,
       OpenGLForwardTranslucencyCompilesWithOpaqueSceneColorPyramidSamplerPair)
{
    const fs::path shaderPath =
        fs::path(RVX_SOURCE_DIR) / "Source" / "Runtime" / "Render" /
        "Shaders" / "DefaultLit.hlsl";
    std::ifstream shaderFile(shaderPath, std::ios::binary);
    ASSERT_TRUE(shaderFile.is_open()) << shaderPath.string();
    const std::string source{
        std::istreambuf_iterator<char>(shaderFile),
        std::istreambuf_iterator<char>()};

    auto compiler = RVX::CreateShaderCompiler();
    ASSERT_NE(compiler, nullptr);

    const std::string shaderPathString = shaderPath.string();
    RVX::ShaderCompileOptions options;
    options.stage = RVX::RHIShaderStage::Pixel;
    options.entryPoint = "PSForwardTranslucencyGuarded";
    options.sourceCode = source.c_str();
    options.sourcePath = shaderPathString.c_str();
    options.targetBackend = RVX::RHIBackendType::OpenGL;
    options.enableOptimization = false;

    const RVX::ShaderCompileSupport support = compiler->QuerySupport(options);
    ASSERT_TRUE(support.IsSupported()) << support.reason;

    const RVX::ShaderCompileResult result = compiler->Compile(options);
    ASSERT_TRUE(result.success) << result.errorMessage;
    EXPECT_FALSE(result.glslSource.empty());
    EXPECT_NE(
        result.glslSource.find(
            "layout(binding = 14) uniform sampler2D "
            "OpaqueSceneColorPyramidTexture_IBLLinearClampSampler"),
        std::string::npos);
    EXPECT_NE(
        result.glslSource.find(
            "layout(binding = 16) uniform sampler3D "
            "FroxelVolumetricLightingTexture_IBLLinearClampSampler"),
        std::string::npos);
}
#endif

#if RVX_ENABLE_METAL
TEST_F(ShaderCompilerValidationFixture,
       GFX06SurfaceShadersCompileAsMSL)
{
    auto compiler = RVX::CreateShaderCompiler();
    ASSERT_NE(compiler, nullptr);

    RVX::ShaderCompileOptions supportOptions;
    supportOptions.stage = RVX::RHIShaderStage::Pixel;
    supportOptions.entryPoint = "PSMain";
    supportOptions.sourceCode = "float4 PSMain() : SV_TARGET { return 0.0f; }";
    supportOptions.targetBackend = RVX::RHIBackendType::Metal;

    const RVX::ShaderCompileSupport support =
        compiler->QuerySupport(supportOptions);
    ASSERT_TRUE(support.IsSupported()) << support.reason;

    VerifyGFX06SurfaceShaderMatrix(*compiler, RVX::RHIBackendType::Metal);
}

TEST_F(ShaderCompilerValidationFixture,
       GFX19SSRShadersCompileAsMSLWithMaterializedReflectionABI)
{
    auto compiler = RVX::CreateShaderCompiler();
    ASSERT_NE(compiler, nullptr);

    VerifyGFX19SSRShaderMatrix(*compiler, RVX::RHIBackendType::Metal);
}

TEST_F(ShaderCompilerValidationFixture,
       GFX22WeightedBlendedOITShadersCompileAsMSL)
{
    auto compiler = RVX::CreateShaderCompiler();
    ASSERT_NE(compiler, nullptr);
    VerifyGFX22WeightedBlendedOITShaderMatrix(
        *compiler, RVX::RHIBackendType::Metal);
}

TEST_F(ShaderCompilerValidationFixture,
       GFX24TAAShadersCompileAsMSLWithDescriptorABI)
{
    auto compiler = RVX::CreateShaderCompiler();
    ASSERT_NE(compiler, nullptr);
    VerifyGFX24TAAShaderMatrix(*compiler, RVX::RHIBackendType::Metal);
}

TEST_F(ShaderCompilerValidationFixture,
       OpaqueSceneColorPyramidShadersCompileAsMSLWithDescriptorABI)
{
    auto compiler = RVX::CreateShaderCompiler();
    ASSERT_NE(compiler, nullptr);
    VerifyOpaqueSceneColorPyramidShaderMatrix(
        *compiler, RVX::RHIBackendType::Metal);
}
#endif

#if RVX_ENABLE_OPENGL
TEST_F(ShaderCompilerValidationFixture,
       OpenGLToneMappingPixelCompileSupportsPortableCBufferLayout)
{
    const fs::path shaderPath = fs::path(RVX_SOURCE_DIR) / "Source" / "Runtime" /
                                "Render" / "Shaders" / "PostProcess" /
                                "ToneMapping.hlsl";
    std::ifstream shaderFile(shaderPath, std::ios::binary);
    ASSERT_TRUE(shaderFile.is_open()) << shaderPath.string();
    const std::string source{
        std::istreambuf_iterator<char>(shaderFile),
        std::istreambuf_iterator<char>()};

    auto compiler = RVX::CreateShaderCompiler();
    ASSERT_NE(compiler, nullptr);

    const std::string shaderPathString = shaderPath.string();
    RVX::ShaderCompileOptions options;
    options.stage = RVX::RHIShaderStage::Pixel;
    options.entryPoint = "PSMain";
    options.sourceCode = source.c_str();
    options.sourcePath = shaderPathString.c_str();
    options.targetBackend = RVX::RHIBackendType::OpenGL;
    options.enableOptimization = false;
    options.defines.push_back({"RVX_FULLSCREEN_UV_Y_MATCHES_CLIP_Y", "1"});

    const RVX::ShaderCompileSupport support = compiler->QuerySupport(options);
    ASSERT_TRUE(support.IsSupported()) << support.reason;

    const RVX::ShaderCompileResult result = compiler->Compile(options);
    ASSERT_TRUE(result.success) << result.errorMessage;
    EXPECT_FALSE(result.glslSource.empty());
}

TEST_F(ShaderCompilerValidationFixture, OpenGLCompileProducesGLSLSource)
{
    TempDirectory temp("rvx_shader_compile_gl");
    fs::path shaderPath = temp.Path() / "Main.hlsl";
    fs::path includePath = temp.Path() / "Common.hlsli";
    std::string source = TestVertexShaderSource();

    WriteTextFile(includePath, "static const float3 RVX_TEST_OFFSET = float3(0.0, 0.0, 0.0);\n");
    WriteTextFile(shaderPath, source);

    auto compiler = RVX::CreateShaderCompiler();
    ASSERT_NE(compiler, nullptr);

    std::string shaderPathString = shaderPath.string();
    RVX::ShaderCompileOptions options;
    options.stage = RVX::RHIShaderStage::Vertex;
    options.entryPoint = "main";
    options.sourceCode = source.c_str();
    options.sourcePath = shaderPathString.c_str();
    options.targetBackend = RVX::RHIBackendType::OpenGL;
    options.enableOptimization = false;

    const RVX::ShaderCompileSupport support = compiler->QuerySupport(options);
    ASSERT_TRUE(support.IsSupported()) << support.reason;

    RVX::ShaderCompileResult result = compiler->Compile(options);
    ASSERT_TRUE(result.success) << result.errorMessage;
    EXPECT_FALSE(result.bytecode.empty());
    EXPECT_FALSE(result.glslSource.empty());
    EXPECT_NE(result.glslSource.find("#version"), std::string::npos);
    EXPECT_FALSE(result.reflection.resources.empty());
    EXPECT_FALSE(result.sourceInfo.IsEmpty());
}

TEST_F(ShaderCompilerValidationFixture,
       OpenGLCompileProducesCompleteRuntimeArtifacts)
{
    TempDirectory temp("rvx_shader_compile_gl_runtime_artifacts");
    const fs::path shaderPath = temp.Path() / "Main.hlsl";
    const fs::path includePath = temp.Path() / "Common.hlsli";
    const std::string source = R"(
#include "Common.hlsli"

struct PushConstants
{
    float4 PushTint;
};
[[vk::push_constant]] PushConstants gPushConstants;

cbuffer FrameConstants : register(b0, space0)
{
    float4 FrameTint;
};

Texture2D<float4> BaseTexture : register(t1, space2);
SamplerState BaseSampler : register(s14, space2);

float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    return RVXApplyIdentity(BaseTexture.Sample(BaseSampler, uv)) *
        gPushConstants.PushTint * FrameTint;
}
)";
    WriteTextFile(includePath, "float4 RVXApplyIdentity(float4 value) { return value; }\n");
    WriteTextFile(shaderPath, source);

    auto compiler = RVX::CreateShaderCompiler();
    ASSERT_NE(compiler, nullptr);

    const std::string shaderPathString = shaderPath.string();
    RVX::ShaderCompileOptions options;
    options.stage = RVX::RHIShaderStage::Pixel;
    options.entryPoint = "main";
    options.sourceCode = source.c_str();
    options.sourcePath = shaderPathString.c_str();
    options.targetBackend = RVX::RHIBackendType::OpenGL;
    options.enableOptimization = false;

    const RVX::ShaderCompileSupport support = compiler->QuerySupport(options);
    ASSERT_TRUE(support.IsSupported()) << support.reason;

    const RVX::ShaderCompileResult result = compiler->Compile(options);
    ASSERT_TRUE(result.success) << result.errorMessage;
    EXPECT_TRUE(HasSPIRVHeader(result.bytecode));
    EXPECT_FALSE(result.glslSource.empty());
    EXPECT_EQ(result.glslVersion, 450u);
    EXPECT_TRUE(result.reflection.valid);
    EXPECT_FALSE(result.glslBindings.uboBindings.empty());
    EXPECT_FALSE(result.glslBindings.textureBindings.empty());
    EXPECT_FALSE(result.glslBindings.samplerBindings.empty());
    ASSERT_TRUE(result.glslPushConstant.has_value());
    EXPECT_GT(result.glslPushConstant->size, 0u);
    EXPECT_FALSE(result.sourceInfo.IsEmpty());
    EXPECT_TRUE(HasTrackedInclude(result, "Common.hlsli"));
}

TEST_F(ShaderCompilerValidationFixture, OpenGLRemapsDescriptorSetsByStableSetBindingOrder)
{
    TempDirectory temp("rvx_shader_gl_binding_order");
    fs::path shaderPath = temp.Path() / "BindingOrder.hlsl";
    const std::string source = R"(
cbuffer FrameConstants : register(b0, space0)
{
    float4 FrameTint;
};

cbuffer MaterialConstants : register(b0, space2)
{
    float4 MaterialTint;
};

Texture2D<float4> ShadowTexture : register(t1, space0);
SamplerState ShadowSampler : register(s2, space0);
Texture2D<float4> RayMaskTexture : register(t6, space0);

Texture2D<float4> MaterialTexture1 : register(t1, space2);
Texture2D<float4> MaterialTexture2 : register(t2, space2);
Texture2D<float4> MaterialTexture3 : register(t3, space2);
Texture2D<float4> MaterialTexture4 : register(t4, space2);
Texture2D<float4> MaterialTexture5 : register(t5, space2);
Texture2D<float4> MaterialTexture6 : register(t6, space2);
Texture2D<float4> MaterialTexture7 : register(t7, space2);
Texture2D<float4> MaterialTexture8 : register(t8, space2);
Texture2D<float4> MaterialTexture9 : register(t9, space2);
Texture2D<float4> MaterialTexture10 : register(t10, space2);
Texture2D<float4> MaterialTexture11 : register(t11, space2);
Texture2D<float4> MaterialTexture12 : register(t12, space2);
Texture2D<float4> MaterialTexture13 : register(t13, space2);
SamplerState MaterialSampler1 : register(s14, space2);
SamplerState MaterialSampler2 : register(s15, space2);
SamplerState MaterialSampler3 : register(s16, space2);
SamplerState MaterialSampler4 : register(s17, space2);
SamplerState MaterialSampler5 : register(s18, space2);
SamplerState MaterialSampler6 : register(s19, space2);
SamplerState MaterialSampler7 : register(s20, space2);
SamplerState MaterialSampler8 : register(s21, space2);
SamplerState MaterialSampler9 : register(s22, space2);
SamplerState MaterialSampler10 : register(s23, space2);
SamplerState MaterialSampler11 : register(s24, space2);
SamplerState MaterialSampler12 : register(s25, space2);
SamplerState MaterialSampler13 : register(s26, space2);
StructuredBuffer<float4> MaterialTable : register(t27, space2);

float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    float4 color = ShadowTexture.Sample(ShadowSampler, uv);
    color += RayMaskTexture.Load(int3(0, 0, 0));
    color += MaterialTexture1.Sample(MaterialSampler1, uv);
    color += MaterialTexture2.Sample(MaterialSampler2, uv);
    color += MaterialTexture3.Sample(MaterialSampler3, uv);
    color += MaterialTexture4.Sample(MaterialSampler4, uv);
    color += MaterialTexture5.Sample(MaterialSampler5, uv);
    color += MaterialTexture6.Sample(MaterialSampler6, uv);
    color += MaterialTexture7.Sample(MaterialSampler7, uv);
    color += MaterialTexture8.Sample(MaterialSampler8, uv);
    color += MaterialTexture9.Sample(MaterialSampler9, uv);
    color += MaterialTexture10.Sample(MaterialSampler10, uv);
    color += MaterialTexture11.Sample(MaterialSampler11, uv);
    color += MaterialTexture12.Sample(MaterialSampler12, uv);
    color += MaterialTexture13.Sample(MaterialSampler13, uv);
    return color * FrameTint * MaterialTint + MaterialTable[0];
}
)";
    WriteTextFile(shaderPath, source);

    auto compiler = RVX::CreateShaderCompiler();
    ASSERT_NE(compiler, nullptr);

    std::string shaderPathString = shaderPath.string();
    RVX::ShaderCompileOptions options;
    options.stage = RVX::RHIShaderStage::Pixel;
    options.entryPoint = "main";
    options.sourceCode = source.c_str();
    options.sourcePath = shaderPathString.c_str();
    options.targetBackend = RVX::RHIBackendType::OpenGL;
    options.enableOptimization = false;

    const RVX::ShaderCompileSupport support = compiler->QuerySupport(options);
    ASSERT_TRUE(support.IsSupported()) << support.reason;

    RVX::ShaderCompileResult result = compiler->Compile(options);
    ASSERT_TRUE(result.success) << result.errorMessage;
    EXPECT_FALSE(result.glslSource.empty());

    static_assert(RVX::RVX_SHADER_COMPILER_CACHE_ABI_VERSION >= 18,
                   "OpenGL GLSL binding ABI changes must invalidate cached GLSL");
    static_assert(RVX::RVX_SHADER_COMPILER_ABI_VERSION >= 17,
                  "OpenGL cooked GLSL binding remaps must invalidate artifacts");
    EXPECT_EQ(RVX::GLSLBindingABI::FlattenBinding(RVX::RHIBindingType::UniformBuffer, 0, 0), 1u);
    EXPECT_EQ(RVX::GLSLBindingABI::FlattenBinding(RVX::RHIBindingType::UniformBuffer, 2, 0), 9u);
    constexpr std::array<RVX::uint32, 13> expectedMaterialTextureUnits = {
        2u, 3u, 7u, 8u, 9u, 4u, 5u, 13u, 17u, 18u, 19u, 20u, 21u};
    for (RVX::uint32 textureBinding = 1;
         textureBinding <= expectedMaterialTextureUnits.size();
         ++textureBinding)
    {
        const RVX::uint32 expectedUnit =
            expectedMaterialTextureUnits[textureBinding - 1];
        EXPECT_EQ(
            RVX::GLSLBindingABI::FlattenBinding(
                RVX::RHIBindingType::SampledTexture, 2, textureBinding),
            expectedUnit);
        EXPECT_EQ(
            RVX::GLSLBindingABI::FlattenBinding(
                RVX::RHIBindingType::Sampler, 2, textureBinding + 13),
            expectedUnit);
    }
    EXPECT_EQ(
        RVX::GLSLBindingABI::FlattenBinding(
            RVX::RHIBindingType::ShaderResourceBuffer, 0, 11),
        11u);
    EXPECT_EQ(
        RVX::GLSLBindingABI::FlattenBinding(
            RVX::RHIBindingType::ShaderResourceBuffer, 1, 1),
        12u);
    EXPECT_EQ(
        RVX::GLSLBindingABI::FlattenBinding(
            RVX::RHIBindingType::ShaderResourceBuffer, 1, 3),
        14u);
    EXPECT_EQ(
        RVX::GLSLBindingABI::FlattenBinding(
            RVX::RHIBindingType::ShaderResourceBuffer, 2, 27),
        15u);

    EXPECT_EQ(result.glslBindings.uboBindings["FrameConstants"],
              RVX::GLSLBindingABI::FlattenBinding(RVX::RHIBindingType::UniformBuffer, 0, 0));
    EXPECT_EQ(result.glslBindings.uboBindings["MaterialConstants"],
              RVX::GLSLBindingABI::FlattenBinding(RVX::RHIBindingType::UniformBuffer, 2, 0));

    EXPECT_EQ(result.glslBindings.GetGLBinding(0, 1),
              RVX::GLSLBindingABI::FlattenBinding(RVX::RHIBindingType::SampledTexture, 0, 1));
    EXPECT_EQ(result.glslBindings.GetGLBinding(0, 6),
              RVX::GLSLBindingABI::FlattenBinding(RVX::RHIBindingType::SampledTexture, 0, 6));
    for (RVX::uint32 textureBinding = 1;
         textureBinding <= expectedMaterialTextureUnits.size();
         ++textureBinding)
    {
        const RVX::uint32 expectedUnit =
            expectedMaterialTextureUnits[textureBinding - 1];
        EXPECT_EQ(result.glslBindings.GetGLBinding(2, textureBinding),
                  expectedUnit);
        EXPECT_EQ(result.glslBindings.GetGLBinding(2, textureBinding + 13),
                  expectedUnit);
    }
    EXPECT_EQ(result.glslBindings.GetGLBinding(2, 27), 15u);
    EXPECT_EQ(result.glslBindings.ssboBindings["MaterialTable"], 15u);

    EXPECT_EQ(RVX::GLSLBindingABI::FlattenBinding(
                  RVX::RHIBindingType::SampledTexture, 0, 18),
              16u);

    RVX::uint32 froxelVolumeUnit = RVX::RVX_INVALID_INDEX;
    EXPECT_TRUE(RVX::GLSLBindingABI::TryGetTextureSamplerUnit(
        0, 18, 13, froxelVolumeUnit));
    EXPECT_EQ(froxelVolumeUnit, 16u);

    const auto shadowSamplerBinding =
        RVX::GLSLBindingABI::FlattenBinding(RVX::RHIBindingType::Sampler, 0, 2);
    const auto shadowTextureBinding =
        RVX::GLSLBindingABI::FlattenBinding(RVX::RHIBindingType::SampledTexture, 0, 1);

    EXPECT_EQ(result.glslBindings.samplerBindings["ShadowSampler"], shadowSamplerBinding);
    EXPECT_NE(result.glslSource.find(
                  "layout(binding = " + std::to_string(shadowTextureBinding) +
                  ") uniform sampler2D ShadowTexture_ShadowSampler"),
              std::string::npos);
    for (RVX::uint32 textureBinding = 1;
         textureBinding <= expectedMaterialTextureUnits.size();
         ++textureBinding)
    {
        const RVX::uint32 expectedUnit =
            expectedMaterialTextureUnits[textureBinding - 1];
        const std::string textureName =
            "MaterialTexture" + std::to_string(textureBinding);
        const std::string samplerName =
            "MaterialSampler" + std::to_string(textureBinding);

        EXPECT_EQ(result.glslBindings.samplerBindings[samplerName], expectedUnit);
        EXPECT_NE(result.glslSource.find(
                      "layout(binding = " + std::to_string(expectedUnit) +
                      ") uniform sampler2D " + textureName + "_" + samplerName),
                  std::string::npos);
    }
}

TEST_F(ShaderCompilerValidationFixture, OpenGLHotReloadUsesGeneratedGLSLSource)
{
    TempDirectory temp("rvx_shader_reload_gl");
    fs::path shaderPath = temp.Path() / "Reload.hlsl";
    WriteTextFile(shaderPath, "float4 main() : SV_Position { return 1; }\n");

    const std::string spirvBytes = "SPIRV-BYTES";
    const std::string glslSource = "#version 450\nvoid main() {}\n";

    RVX::ShaderCompileResult compileResult;
    compileResult.success = true;
    compileResult.bytecode.assign(spirvBytes.begin(), spirvBytes.end());
    compileResult.glslSource = glslSource;

    RVX::ShaderCompileService::Config serviceConfig;
    serviceConfig.maxConcurrentCompiles = 1;
    RVX::ShaderCompileService service(
        std::make_unique<StaticResultCompiler>(compileResult),
        serviceConfig);

    RVX::ShaderHotReloader::Config reloadConfig;
    reloadConfig.enabled = false;
    RVX::ShaderHotReloader reloader(&service, nullptr, reloadConfig);

    CapturingDevice device(RVX::RHIBackendType::OpenGL);

    RVX::RHIShaderDesc oldDesc;
    oldDesc.stage = RVX::RHIShaderStage::Vertex;
    oldDesc.bytecode = glslSource.data();
    oldDesc.bytecodeSize = glslSource.size();
    oldDesc.entryPoint = "main";
    RVX::RHIShaderRef oldShader = RVX::MakeRef<CapturedShader>(oldDesc);

    RVX::ShaderPermutationLoadDesc loadDesc;
    loadDesc.path = shaderPath.string();
    loadDesc.entryPoint = "main";
    loadDesc.stage = RVX::RHIShaderStage::Vertex;
    loadDesc.backend = RVX::RHIBackendType::OpenGL;
    loadDesc.enableOptimization = false;

    bool callbackCalled = false;
    RVX::ShaderReloadInfo callbackInfo;
    reloader.RegisterShader(
        &device,
        loadDesc.path,
        oldShader,
        loadDesc,
        [&](const RVX::ShaderReloadInfo& info)
        {
            callbackCalled = true;
            callbackInfo = info;
        });

    reloader.ForceReload(loadDesc.path);

    EXPECT_TRUE(callbackCalled);
    EXPECT_TRUE(callbackInfo.success) << callbackInfo.errorMessage;
    EXPECT_EQ(device.createShaderCalls, 1u);
    EXPECT_EQ(device.lastShaderSource, glslSource);
    EXPECT_NE(device.lastShaderSource, spirvBytes);
}

TEST_F(ShaderCompilerValidationFixture, OpenGLPermutationUsesGeneratedGLSLSource)
{
    TempDirectory temp("rvx_shader_permutation_gl");
    fs::path shaderPath = temp.Path() / "Variant.hlsl";
    WriteTextFile(shaderPath, "float4 main() : SV_Position { return 1; }\n");

    const std::string spirvBytes = "SPIRV-BYTES";
    const std::string glslSource = "#version 450\nvoid main() {}\n";

    RVX::ShaderCompileResult compileResult;
    compileResult.success = true;
    compileResult.bytecode.assign(spirvBytes.begin(), spirvBytes.end());
    compileResult.glslSource = glslSource;

    RVX::ShaderCompileService::Config serviceConfig;
    serviceConfig.maxConcurrentCompiles = 1;
    RVX::ShaderCompileService service(
        std::make_unique<StaticResultCompiler>(compileResult),
        serviceConfig);
    RVX::ShaderPermutationSystem permutationSystem(&service, nullptr);

    RVX::ShaderPermutationSpace space;
    RVX::ShaderPermutationLoadDesc loadDesc;
    loadDesc.path = shaderPath.string();
    loadDesc.entryPoint = "main";
    loadDesc.stage = RVX::RHIShaderStage::Vertex;
    loadDesc.backend = RVX::RHIBackendType::OpenGL;
    loadDesc.enableOptimization = false;

    CapturingDevice device(RVX::RHIBackendType::OpenGL);
    permutationSystem.RegisterShader(shaderPath.string(), space, loadDesc);

    RVX::RHIShaderRef shader = permutationSystem.GetVariant(&device, shaderPath.string(), {});
    ASSERT_TRUE(shader);
    EXPECT_EQ(device.createShaderCalls, 1u);
    EXPECT_EQ(device.lastShaderSource, glslSource);
    EXPECT_NE(device.lastShaderSource, spirvBytes);
}

TEST(ShaderBindingABIValidation, MaterialSet2UsesCompletePairedOpenGLAndMetalMappings)
{
    static_assert(RVX::RVX_SHADER_COMPILER_CACHE_ABI_VERSION >= 18,
                  "Cross-backend resource-index ABI changes must invalidate cached shader source");

    constexpr std::array<RVX::uint32, 13> expectedOpenGLTextureUnits = {
        2u, 3u, 7u, 8u, 9u, 4u, 5u, 13u, 17u, 18u, 19u, 20u, 21u};
    for (RVX::uint32 textureBinding = 1;
         textureBinding <= expectedOpenGLTextureUnits.size();
         ++textureBinding)
    {
        RVX::uint32 pairedUnit = RVX::RVX_INVALID_INDEX;
        EXPECT_TRUE(RVX::GLSLBindingABI::TryGetTextureSamplerUnit(
            2, textureBinding, textureBinding + 13, pairedUnit));
        EXPECT_EQ(pairedUnit, expectedOpenGLTextureUnits[textureBinding - 1]);
        EXPECT_EQ(
            RVX::GLSLBindingABI::FlattenBinding(
                RVX::RHIBindingType::SampledTexture, 2, textureBinding),
            pairedUnit);
        EXPECT_EQ(
            RVX::GLSLBindingABI::FlattenBinding(
                RVX::RHIBindingType::Sampler, 2, textureBinding + 13),
            pairedUnit);

        RVX::uint32 metalTexture = RVX::RVX_INVALID_INDEX;
        RVX::uint32 metalSampler = RVX::RVX_INVALID_INDEX;
        EXPECT_TRUE(RVX::MetalBindingABI::TryGetTextureIndex(
            2, textureBinding, 0, metalTexture));
        EXPECT_EQ(metalTexture, textureBinding - 1);
        if (textureBinding <= 8)
        {
            EXPECT_TRUE(RVX::MetalBindingABI::TryGetSamplerIndex(
                2, textureBinding + 13, 0, metalSampler));
            EXPECT_EQ(metalSampler, textureBinding - 1);
        }
        else
        {
            EXPECT_FALSE(RVX::MetalBindingABI::TryGetSamplerIndex(
                2, textureBinding + 13, 0, metalSampler));
        }
    }

    EXPECT_EQ(
        RVX::GLSLBindingABI::FlattenBinding(
            RVX::RHIBindingType::ShaderResourceBuffer, 2, 27),
        15u);

    RVX::uint32 opaqueSnapshotUnit = RVX::RVX_INVALID_INDEX;
    EXPECT_TRUE(RVX::GLSLBindingABI::TryGetTextureSamplerUnit(
        0, 16, 13, opaqueSnapshotUnit));
    EXPECT_EQ(opaqueSnapshotUnit, 14u);

    RVX::uint32 invalidOpaqueSnapshotPair = RVX::RVX_INVALID_INDEX;
    EXPECT_FALSE(RVX::GLSLBindingABI::TryGetTextureSamplerUnit(
        0, 16, 2, invalidOpaqueSnapshotPair));

    RVX::uint32 localShadowUnit = RVX::RVX_INVALID_INDEX;
    EXPECT_TRUE(RVX::GLSLBindingABI::TryGetTextureSamplerUnit(
        0, 17, 2, localShadowUnit));
    EXPECT_EQ(localShadowUnit, 0u);

    RVX::uint32 invalidLocalShadowPair = RVX::RVX_INVALID_INDEX;
    EXPECT_FALSE(RVX::GLSLBindingABI::TryGetTextureSamplerUnit(
        0, 17, 13, invalidLocalShadowPair));

    static_assert(
        RVX::GLSLBindingABI::RVX_GLSL_REQUIRED_TEXTURE_UNIT_COUNT == 23u,
        "Material V2 plus transmission depth requires twenty-three OpenGL texture units");
    static_assert(RVX::GLSLBindingABI::FlattenTextureBinding(0, 19) == 22u,
        "Transmission scene depth must not alias material or frame textures");
    static_assert(
        RVX::GLSLBindingABI::FlattenTextureBinding(0, 18) == 16u,
        "Froxel volume must retain a dedicated OpenGL texture unit");
    RVX::uint32 froxelVolumeUnit = RVX::RVX_INVALID_INDEX;
    EXPECT_TRUE(RVX::GLSLBindingABI::TryGetTextureSamplerUnit(
        0, 18, 13, froxelVolumeUnit));
    EXPECT_EQ(froxelVolumeUnit, 16u);
    EXPECT_FALSE(RVX::GLSLBindingABI::TryGetTextureSamplerUnit(
        0, 18, 2, froxelVolumeUnit));

    // GFX19's resolve shader has one read-only exposure SSBO and two
    // integer-load materialized surface textures. The latter must not gain a
    // sampler association as that would silently change the shader contract.
    static_assert(
        RVX::GLSLBindingABI::FlattenStorageBufferBinding(0, 16) == 0u,
        "The GFX19 exposure state must fit the OpenGL storage-buffer ABI");
    static_assert(
        RVX::GLSLBindingABI::FlattenTextureBinding(0, 15) == 15u,
        "The GFX19 base indirect texture must retain logical t15");
    static_assert(
        RVX::GLSLBindingABI::FlattenTextureBinding(0, 17) == 0u,
        "The GFX19 specular response texture must fit the OpenGL texture ABI");
    static_assert(
        RVX::GLSLBindingABI::FlattenSamplerBinding(0, 19) == 14u,
        "The GFX19 local-probe sampler must retain its reflected ABI slot");

    EXPECT_EQ(
        RVX::GLSLBindingABI::FlattenBinding(
            RVX::RHIBindingType::ShaderResourceBuffer, 0, 16),
        0u);
    EXPECT_EQ(
        RVX::GLSLBindingABI::FlattenBinding(
            RVX::RHIBindingType::SampledTexture, 0, 15),
        15u);
    EXPECT_EQ(
        RVX::GLSLBindingABI::FlattenBinding(
            RVX::RHIBindingType::SampledTexture, 0, 17),
        0u);

    constexpr std::array<RVX::uint32, 9> gfx19SamplerTextures = {
        1u, 7u, 8u, 9u, 10u, 11u, 12u, 13u, 14u};
    for (const RVX::uint32 textureBinding : gfx19SamplerTextures)
    {
        RVX::uint32 samplerUnit = RVX::RVX_INVALID_INDEX;
        EXPECT_TRUE(RVX::GLSLBindingABI::TryGetTextureSamplerUnit(
            0, textureBinding, 19, samplerUnit));
        EXPECT_EQ(
            samplerUnit,
            RVX::GLSLBindingABI::FlattenTextureBinding(0, textureBinding));
    }

    RVX::uint32 invalidGfx19SamplerPair = RVX::RVX_INVALID_INDEX;
    EXPECT_FALSE(RVX::GLSLBindingABI::TryGetTextureSamplerUnit(
        0, 15, 19, invalidGfx19SamplerPair));
    EXPECT_FALSE(RVX::GLSLBindingABI::TryGetTextureSamplerUnit(
        0, 17, 19, invalidGfx19SamplerPair));

    RVX::uint32 materialConstants = RVX::RVX_INVALID_INDEX;
    RVX::uint32 materialTable = RVX::RVX_INVALID_INDEX;
    EXPECT_TRUE(RVX::MetalBindingABI::TryGetBufferIndex(2, 0, 0, materialConstants));
    EXPECT_TRUE(RVX::MetalBindingABI::TryGetBufferIndex(2, 27, 0, materialTable));
    EXPECT_EQ(materialConstants, 20u);
    EXPECT_EQ(materialTable, 21u);
    EXPECT_LE(materialTable, RVX::MetalBindingABI::RVX_METAL_MAX_BUFFER_INDEX);

    RVX::uint32 deferredLocalShadowTexture = RVX::RVX_INVALID_INDEX;
    EXPECT_TRUE(RVX::MetalBindingABI::TryGetTextureIndex(
        0, 23, 0, deferredLocalShadowTexture));
    EXPECT_EQ(deferredLocalShadowTexture, 39u);

    RVX::uint32 gfx19BaseIndirectSpecular = RVX::RVX_INVALID_INDEX;
    RVX::uint32 gfx19ExposureState = RVX::RVX_INVALID_INDEX;
    RVX::uint32 gfx19SpecularResponse = RVX::RVX_INVALID_INDEX;
    RVX::uint32 gfx19LinearClampSampler = RVX::RVX_INVALID_INDEX;
    EXPECT_TRUE(RVX::MetalBindingABI::TryGetTextureIndex(
        0, 15, 0, gfx19BaseIndirectSpecular));
    EXPECT_TRUE(RVX::MetalBindingABI::TryGetBufferIndex(
        0, 16, 0, gfx19ExposureState));
    EXPECT_TRUE(RVX::MetalBindingABI::TryGetTextureIndex(
        0, 17, 0, gfx19SpecularResponse));
    EXPECT_TRUE(RVX::MetalBindingABI::TryGetSamplerIndex(
        0, 19, 0, gfx19LinearClampSampler));
    EXPECT_EQ(gfx19BaseIndirectSpecular, 31u);
    EXPECT_EQ(gfx19ExposureState, 16u);
    EXPECT_EQ(gfx19SpecularResponse, 33u);
    EXPECT_EQ(gfx19LinearClampSampler, 15u);

    RVX::uint32 ignored = RVX::RVX_INVALID_INDEX;
    EXPECT_FALSE(RVX::GLSLBindingABI::TryFlattenBinding(
        RVX::RHIBindingType::SampledTexture, 2, 14, ignored));
    EXPECT_FALSE(RVX::GLSLBindingABI::TryFlattenBinding(
        RVX::RHIBindingType::ShaderResourceBuffer, 2, 16, ignored));
    EXPECT_FALSE(RVX::MetalBindingABI::TryGetBufferIndex(2, 18, 0, ignored));
    EXPECT_FALSE(RVX::MetalBindingABI::TryGetSamplerIndex(2, 22, 0, ignored));
}
#endif

#if !defined(__APPLE__)
TEST_F(ShaderCompilerValidationFixture,
       GFX06MetalShaderCapabilityIsExplicitlyUnsupportedOnNonApple)
{
    auto compiler = RVX::CreateShaderCompiler();
    ASSERT_NE(compiler, nullptr);

    const std::string source =
        "float4 main() : SV_TARGET { return float4(0.0f, 0.0f, 0.0f, 1.0f); }";
    RVX::ShaderCompileOptions options;
    options.stage = RVX::RHIShaderStage::Pixel;
    options.entryPoint = "main";
    options.sourceCode = source.c_str();
    options.sourcePath = "GFX06MetalCapability.hlsl";
    options.targetBackend = RVX::RHIBackendType::Metal;

    const RVX::ShaderCompileSupport support = compiler->QuerySupport(options);
    EXPECT_FALSE(support.IsSupported());
    EXPECT_EQ(
        support.code,
        RVX::ShaderCompileSupportCode::BackendUnsupported);
    EXPECT_FALSE(support.reason.empty());
}

TEST_F(ShaderCompilerValidationFixture,
       GFX19MetalSSRShaderCapabilityIsExplicitlyUnsupportedOnNonApple)
{
    auto compiler = RVX::CreateShaderCompiler();
    ASSERT_NE(compiler, nullptr);

    const fs::path shaderPath =
        fs::path(RVX_SOURCE_DIR) / "Source" / "Runtime" / "Render" /
        "Shaders" / "Reflection" / "HiZScreenSpaceReflection.hlsl";
    ASSERT_TRUE(fs::exists(shaderPath)) << shaderPath.string();
    const std::string source = ReadTextFile(shaderPath);
    ASSERT_FALSE(source.empty()) << shaderPath.string();
    const std::string shaderPathString = shaderPath.string();

    RVX::ShaderCompileOptions options;
    options.stage = RVX::RHIShaderStage::Pixel;
    options.entryPoint = "PSResolve";
    options.sourceCode = source.c_str();
    options.sourcePath = shaderPathString.c_str();
    options.targetBackend = RVX::RHIBackendType::Metal;

    const RVX::ShaderCompileSupport support = compiler->QuerySupport(options);
    EXPECT_FALSE(support.IsSupported());
    EXPECT_EQ(
        support.code,
        RVX::ShaderCompileSupportCode::BackendUnsupported);
    EXPECT_FALSE(support.reason.empty());
}
#endif

TEST(ShaderCompilerPolicyValidation, DebugInformationDoesNotChangeOptimizationMode)
{
    RVX::ShaderCompileOptions options;

    options.enableDebugInfo = false;
    options.enableOptimization = false;
    EXPECT_EQ(
        RVX::ResolveShaderOptimizationMode(options),
        RVX::ShaderOptimizationMode::Disabled);

    options.enableDebugInfo = true;
    EXPECT_EQ(
        RVX::ResolveShaderOptimizationMode(options),
        RVX::ShaderOptimizationMode::Disabled);

    options.enableDebugInfo = false;
    options.enableOptimization = true;
    EXPECT_EQ(
        RVX::ResolveShaderOptimizationMode(options),
        RVX::ShaderOptimizationMode::Level3);

    options.enableDebugInfo = true;
    EXPECT_EQ(
        RVX::ResolveShaderOptimizationMode(options),
        RVX::ShaderOptimizationMode::Level3);

    static_assert(RVX::RVX_SHADER_COMPILER_CACHE_ABI_VERSION >= 13,
                  "Shader optimization-policy changes must invalidate cached bytecode");
}
