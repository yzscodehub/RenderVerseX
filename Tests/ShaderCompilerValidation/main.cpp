#include "Core/Log.h"
#include "ShaderCompiler/ShaderCacheManager.h"
#include "ShaderCompiler/ShaderCompiler.h"
#include "ShaderCompiler/ShaderCompileService.h"
#include "ShaderCompiler/ShaderHotReloader.h"
#include "ShaderCompiler/ShaderLayout.h"
#include "ShaderCompiler/ShaderPermutation.h"
#include "RHI/RHI.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
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

    bool IsCompilerUnavailable(const RVX::ShaderCompileResult& result)
    {
        return result.errorMessage.find("not available") != std::string::npos ||
               result.errorMessage.find("not initialized") != std::string::npos;
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
};

struct VSOutput
{
    float4 position : SV_POSITION;
};

VSOutput main(VSInput input)
{
    VSOutput output;
    output.position = mul(gViewProjection, float4(input.position + RVX_TEST_OFFSET, 1.0));
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
            : m_stage(desc.stage)
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

    RVX::ShaderCompileResult result = compiler->Compile(options);
    if (!result.success && IsCompilerUnavailable(result))
    {
        GTEST_SKIP() << result.errorMessage;
    }

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

    RVX::AutoPipelineLayout layout = RVX::BuildAutoPipelineLayout({{result.reflection, RVX::RHIShaderStage::Vertex}});
    ASSERT_FALSE(layout.setLayouts.empty());
    ASSERT_FALSE(layout.setLayouts[0].entries.empty());
    EXPECT_EQ(layout.setLayouts[0].entries[0].type, RVX::RHIBindingType::UniformBuffer);
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

    RVX::ShaderCompileResult result = compiler->Compile(options);
    if (!result.success && IsCompilerUnavailable(result))
    {
        GTEST_SKIP() << result.errorMessage;
    }

    ASSERT_TRUE(result.success) << result.errorMessage;
    EXPECT_FALSE(result.bytecode.empty());
    EXPECT_FALSE(result.glslSource.empty());
    EXPECT_NE(result.glslSource.find("#version"), std::string::npos);
    EXPECT_FALSE(result.reflection.resources.empty());
    EXPECT_FALSE(result.sourceInfo.IsEmpty());
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
