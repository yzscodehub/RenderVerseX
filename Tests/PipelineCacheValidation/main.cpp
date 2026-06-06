#include "Core/Log.h"
#include "Render/PipelineCache.h"
#include "RHI/RHI.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    namespace fs = std::filesystem;

    class PipelineCacheValidationFixture : public ::testing::Test
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

    fs::path FindShaderDirectory()
    {
        fs::path cursor = fs::current_path();
        for (uint32_t i = 0; i < 8; ++i)
        {
            fs::path candidate = cursor / "Render" / "Shaders";
            if (fs::exists(candidate / "DefaultLit.hlsl"))
            {
                return candidate;
            }

            if (!cursor.has_parent_path() || cursor == cursor.parent_path())
            {
                break;
            }
            cursor = cursor.parent_path();
        }

        return {};
    }

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

    class FakeBuffer final : public RVX::RHIBuffer
    {
    public:
        explicit FakeBuffer(const RVX::RHIBufferDesc& desc)
            : m_desc(desc)
            , m_storage(static_cast<size_t>(desc.size))
        {
        }

        RVX::uint64 GetSize() const override { return m_desc.size; }
        RVX::RHIBufferUsage GetUsage() const override { return m_desc.usage; }
        RVX::RHIMemoryType GetMemoryType() const override { return m_desc.memoryType; }
        RVX::uint32 GetStride() const override { return m_desc.stride; }
        void* Map() override { return m_storage.data(); }
        void Unmap() override {}

    private:
        RVX::RHIBufferDesc m_desc;
        std::vector<RVX::uint8> m_storage;
    };

    class FakeShader final : public RVX::RHIShader
    {
    public:
        explicit FakeShader(const RVX::RHIShaderDesc& desc)
            : m_stage(desc.stage)
        {
            if (desc.bytecode && desc.bytecodeSize > 0)
            {
                const auto* bytes = static_cast<const RVX::uint8*>(desc.bytecode);
                m_bytecode.assign(bytes, bytes + desc.bytecodeSize);
            }
        }

        RVX::RHIShaderStage GetStage() const override { return m_stage; }
        const std::vector<RVX::uint8>& GetBytecode() const override { return m_bytecode; }

    private:
        RVX::RHIShaderStage m_stage = RVX::RHIShaderStage::None;
        std::vector<RVX::uint8> m_bytecode;
    };

    class FakeDescriptorSetLayout final : public RVX::RHIDescriptorSetLayout
    {
    public:
        explicit FakeDescriptorSetLayout(const RVX::RHIDescriptorSetLayoutDesc& desc)
            : m_entries(desc.entries)
        {
        }

        const std::vector<RVX::RHIBindingLayoutEntry>& GetEntries() const override { return m_entries; }

    private:
        std::vector<RVX::RHIBindingLayoutEntry> m_entries;
    };

    class FakePipelineLayout final : public RVX::RHIPipelineLayout
    {
    };

    class FakePipeline final : public RVX::RHIPipeline
    {
    public:
        bool IsCompute() const override { return false; }
    };

    class FakeDescriptorSet final : public RVX::RHIDescriptorSet
    {
    public:
        bool Update(const std::vector<RVX::RHIDescriptorBinding>& bindings) override
        {
            m_bindings = bindings;
            return true;
        }

    private:
        std::vector<RVX::RHIDescriptorBinding> m_bindings;
    };

    class FakeDevice final : public RVX::IRHIDevice
    {
    public:
        explicit FakeDevice(RVX::RHIBackendType backend = RVX::RHIBackendType::DX12)
            : m_backend(backend)
        {
        }

        RVX::RHIBufferRef CreateBuffer(const RVX::RHIBufferDesc& desc) override
        {
            if (failBufferCreation)
                return {};
            return RVX::MakeRef<FakeBuffer>(desc);
        }

        RVX::RHITextureRef CreateTexture(const RVX::RHITextureDesc&) override { return {}; }
        RVX::RHITextureViewRef CreateTextureView(RVX::RHITexture*, const RVX::RHITextureViewDesc&) override { return {}; }
        RVX::RHISamplerRef CreateSampler(const RVX::RHISamplerDesc&) override { return {}; }

        RVX::RHIShaderRef CreateShader(const RVX::RHIShaderDesc& desc) override
        {
            ++shaderCreateCount;
            if (failShaderCreation || shaderCreateCount == failShaderCreationAtIndex)
                return {};
            return RVX::MakeRef<FakeShader>(desc);
        }

        RVX::RHIHeapRef CreateHeap(const RVX::RHIHeapDesc&) override { return {}; }
        RVX::RHITextureRef CreatePlacedTexture(RVX::RHIHeap*, RVX::uint64, const RVX::RHITextureDesc&) override { return {}; }
        RVX::RHIBufferRef CreatePlacedBuffer(RVX::RHIHeap*, RVX::uint64, const RVX::RHIBufferDesc&) override { return {}; }
        MemoryRequirements GetTextureMemoryRequirements(const RVX::RHITextureDesc&) override { return {}; }
        MemoryRequirements GetBufferMemoryRequirements(const RVX::RHIBufferDesc&) override { return {}; }

        RVX::RHIDescriptorSetLayoutRef CreateDescriptorSetLayout(const RVX::RHIDescriptorSetLayoutDesc& desc) override
        {
            capturedSetLayouts.push_back(desc);
            return RVX::MakeRef<FakeDescriptorSetLayout>(desc);
        }

        RVX::RHIPipelineLayoutRef CreatePipelineLayout(const RVX::RHIPipelineLayoutDesc& desc) override
        {
            capturedPipelineLayoutSetCount = static_cast<RVX::uint32>(desc.setLayouts.size());
            capturedPipelineLayoutSetCounts.push_back(capturedPipelineLayoutSetCount);
            if (failPipelineLayoutCreation)
                return {};
            return RVX::MakeRef<FakePipelineLayout>();
        }

        RVX::RHIPipelineRef CreateGraphicsPipeline(const RVX::RHIGraphicsPipelineDesc& desc) override
        {
            capturedGraphicsPipelines.push_back(desc);
            if (failPipelineCreation ||
                capturedGraphicsPipelines.size() == static_cast<size_t>(failPipelineCreationAtIndex))
                return {};
            return RVX::MakeRef<FakePipeline>();
        }

        RVX::RHIPipelineRef CreateComputePipeline(const RVX::RHIComputePipelineDesc&) override { return {}; }
        RVX::RHIDescriptorSetRef CreateDescriptorSet(const RVX::RHIDescriptorSetDesc&) override
        {
            return RVX::MakeRef<FakeDescriptorSet>();
        }
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

        bool failBufferCreation = false;
        bool failShaderCreation = false;
        RVX::uint32 failShaderCreationAtIndex = std::numeric_limits<RVX::uint32>::max();
        bool failPipelineLayoutCreation = false;
        bool failPipelineCreation = false;
        RVX::uint32 failPipelineCreationAtIndex = std::numeric_limits<RVX::uint32>::max();
        RVX::uint32 shaderCreateCount = 0;
        RVX::uint32 capturedPipelineLayoutSetCount = 0;
        std::vector<RVX::uint32> capturedPipelineLayoutSetCounts;
        std::vector<RVX::RHIDescriptorSetLayoutDesc> capturedSetLayouts;
        std::vector<RVX::RHIGraphicsPipelineDesc> capturedGraphicsPipelines;

    private:
        RVX::RHIBackendType m_backend = RVX::RHIBackendType::DX12;
        RVX::RHICapabilities m_capabilities;
    };

    const RVX::RHIBindingLayoutEntry* FindBinding(
        const RVX::RHIDescriptorSetLayoutDesc& desc,
        RVX::uint32 binding)
    {
        auto it = std::find_if(desc.entries.begin(), desc.entries.end(),
            [binding](const RVX::RHIBindingLayoutEntry& entry)
            {
                return entry.binding == binding;
            });
        return it == desc.entries.end() ? nullptr : &(*it);
    }

    bool HasCompilerAvailable()
    {
        auto shaderDir = FindShaderDirectory();
        return !shaderDir.empty();
    }

    void WriteTextFile(const fs::path& path, const std::string& contents)
    {
        fs::create_directories(path.parent_path());
        std::ofstream file(path, std::ios::trunc);
        file << contents;
    }

    std::string ReadTextFile(const fs::path& path)
    {
        std::ifstream file(path);
        std::ostringstream contents;
        contents << file.rdbuf();
        return contents.str();
    }

    void ReplaceManifestFieldValue(const fs::path& manifestPath,
                                   const std::string& key,
                                   const std::string& replacementValue)
    {
        std::string manifest = ReadTextFile(manifestPath);
        const std::string prefix = key + "=";
        const size_t fieldOffset = manifest.find(prefix);
        ASSERT_NE(fieldOffset, std::string::npos);

        const size_t valueOffset = fieldOffset + prefix.size();
        const size_t lineEnd = manifest.find('\n', valueOffset);
        ASSERT_NE(lineEnd, std::string::npos);

        manifest.replace(valueOffset, lineEnd - valueOffset, replacementValue);
        WriteTextFile(manifestPath, manifest);
    }

    RVX::PipelineCacheConfig ConfigWithManifest(const fs::path& manifestDirectory)
    {
        RVX::PipelineCacheConfig config;
        config.manifestDirectory = manifestDirectory;
        return config;
    }
}

TEST_F(PipelineCacheValidationFixture, NullDeviceFailsWithVisibleError)
{
    RVX::PipelineCache cache;
    EXPECT_FALSE(cache.Initialize(nullptr, ""));
    EXPECT_NE(cache.GetLastError().find("Invalid device"), std::string::npos);
}

TEST_F(PipelineCacheValidationFixture, MissingShaderFileFailsWithVisibleError)
{
    TempDirectory temp("rvx_pipeline_missing_shader");
    FakeDevice device;
    RVX::PipelineCache cache;

    EXPECT_FALSE(cache.Initialize(&device, temp.Path().string()));
    EXPECT_NE(cache.GetLastError().find("Shader file not found"), std::string::npos);
}

TEST_F(PipelineCacheValidationFixture, FailedShaderCreationFailsWithVisibleError)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    FakeDevice device;
    device.failShaderCreation = true;

    RVX::PipelineCache cache;
    EXPECT_FALSE(cache.Initialize(&device, FindShaderDirectory().string()));
    EXPECT_NE(cache.GetLastError().find("vertex shader"), std::string::npos);
}

TEST_F(PipelineCacheValidationFixture, MissingToneMappingShaderFailsWithVisibleError)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    const fs::path sourceDir = FindShaderDirectory();
    TempDirectory temp("rvx_pipeline_missing_tonemapping_shader");
    fs::copy_file(sourceDir / "DefaultLit.hlsl", temp.Path() / "DefaultLit.hlsl");
    fs::copy_file(sourceDir / "DepthOnly.hlsl", temp.Path() / "DepthOnly.hlsl");
    fs::copy(sourceDir / "Include", temp.Path() / "Include", fs::copy_options::recursive);
    fs::create_directories(temp.Path() / "PostProcess");

    FakeDevice device;
    RVX::PipelineCache cache;

    EXPECT_FALSE(cache.Initialize(&device, temp.Path().string()));
    EXPECT_NE(cache.GetLastError().find("ToneMapping shader file not found"), std::string::npos);
}

TEST_F(PipelineCacheValidationFixture, ReflectionBuildsDefaultLitLayouts)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    FakeDevice device;
    RVX::PipelineCache cache;

    ASSERT_TRUE(cache.Initialize(&device, FindShaderDirectory().string())) << cache.GetLastError();
    ASSERT_GE(device.capturedSetLayouts.size(), 4u);
    ASSERT_GE(device.capturedPipelineLayoutSetCounts.size(), 2u);
    EXPECT_EQ(device.capturedPipelineLayoutSetCounts[0], 3u);
    EXPECT_EQ(device.capturedPipelineLayoutSetCounts[1], 1u);

    const auto& frameLayout = device.capturedSetLayouts[0];
    const auto& objectLayout = device.capturedSetLayouts[1];
    const auto& materialLayout = device.capturedSetLayouts[2];
    const auto& postProcessLayout = device.capturedSetLayouts[3];

    const auto* frame = FindBinding(frameLayout, 0);
    ASSERT_NE(frame, nullptr);
    EXPECT_EQ(frame->type, RVX::RHIBindingType::UniformBuffer);
    EXPECT_TRUE(RVX::HasFlag(frame->visibility, RVX::RHIShaderStage::Vertex));
    EXPECT_TRUE(RVX::HasFlag(frame->visibility, RVX::RHIShaderStage::Pixel));

    const auto* object = FindBinding(objectLayout, 0);
    ASSERT_NE(object, nullptr);
    EXPECT_EQ(object->type, RVX::RHIBindingType::DynamicUniformBuffer);
    EXPECT_TRUE(object->isDynamic);

    const auto* materialConstants = FindBinding(materialLayout, 0);
    ASSERT_NE(materialConstants, nullptr);
    EXPECT_EQ(materialConstants->type, RVX::RHIBindingType::DynamicUniformBuffer);
    EXPECT_TRUE(materialConstants->isDynamic);

    for (RVX::uint32 binding = 1; binding <= 5; ++binding)
    {
        const auto* texture = FindBinding(materialLayout, binding);
        ASSERT_NE(texture, nullptr);
        EXPECT_EQ(texture->type, RVX::RHIBindingType::SampledTexture);
    }

    const auto* sampler = FindBinding(materialLayout, 6);
    ASSERT_NE(sampler, nullptr);
    EXPECT_EQ(sampler->type, RVX::RHIBindingType::Sampler);

    const auto* postProcessConstants = FindBinding(postProcessLayout, 0);
    ASSERT_NE(postProcessConstants, nullptr);
    EXPECT_EQ(postProcessConstants->type, RVX::RHIBindingType::UniformBuffer);
    EXPECT_TRUE(RVX::HasFlag(postProcessConstants->visibility, RVX::RHIShaderStage::Pixel));

    const auto* postProcessTexture = FindBinding(postProcessLayout, 1);
    ASSERT_NE(postProcessTexture, nullptr);
    EXPECT_EQ(postProcessTexture->type, RVX::RHIBindingType::SampledTexture);
    EXPECT_TRUE(RVX::HasFlag(postProcessTexture->visibility, RVX::RHIShaderStage::Pixel));

    const auto* postProcessSampler = FindBinding(postProcessLayout, 2);
    ASSERT_NE(postProcessSampler, nullptr);
    EXPECT_EQ(postProcessSampler->type, RVX::RHIBindingType::Sampler);
    EXPECT_TRUE(RVX::HasFlag(postProcessSampler->visibility, RVX::RHIShaderStage::Pixel));

    EXPECT_NE(cache.GetPostProcessSetLayout(), nullptr);
    EXPECT_NE(cache.GetPostProcessLayout(), nullptr);
}

TEST_F(PipelineCacheValidationFixture, PipelineStateHashesAreStableAndVariantAware)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    FakeDevice firstDevice;
    RVX::PipelineCache firstCache;
    ASSERT_TRUE(firstCache.Initialize(&firstDevice, FindShaderDirectory().string())) << firstCache.GetLastError();

    FakeDevice secondDevice;
    RVX::PipelineCache secondCache;
    ASSERT_TRUE(secondCache.Initialize(&secondDevice, FindShaderDirectory().string())) << secondCache.GetLastError();

    EXPECT_EQ(firstCache.GetPipelineStateHashForVariant(RVX::MaterialPipelineVariant::Opaque),
              secondCache.GetPipelineStateHashForVariant(RVX::MaterialPipelineVariant::Opaque));
    EXPECT_NE(firstCache.GetPipelineStateHashForVariant(RVX::MaterialPipelineVariant::Opaque), 0u);
    EXPECT_NE(firstCache.GetPipelineStateHashForVariant(RVX::MaterialPipelineVariant::Opaque),
              firstCache.GetPipelineStateHashForVariant(RVX::MaterialPipelineVariant::Transparent));
    EXPECT_EQ(firstCache.GetStats().pipelineCreateCount, 5u);
    EXPECT_EQ(firstCache.GetStats().pipelineCacheMissCount, 5u);
}

TEST_F(PipelineCacheValidationFixture, RenderTargetFormatChangesPipelineHash)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    FakeDevice firstDevice;
    RVX::PipelineCache firstCache;
    ASSERT_TRUE(firstCache.Initialize(&firstDevice, FindShaderDirectory().string())) << firstCache.GetLastError();

    FakeDevice secondDevice;
    RVX::PipelineCache secondCache;
    secondCache.SetRenderTargetFormat(RVX::RHIFormat::RGBA16_FLOAT);
    ASSERT_TRUE(secondCache.Initialize(&secondDevice, FindShaderDirectory().string())) << secondCache.GetLastError();

    EXPECT_NE(firstCache.GetPipelineStateHashForVariant(RVX::MaterialPipelineVariant::Opaque),
              secondCache.GetPipelineStateHashForVariant(RVX::MaterialPipelineVariant::Opaque));
    ASSERT_GE(secondDevice.capturedGraphicsPipelines.size(), 5u);
    EXPECT_EQ(secondDevice.capturedGraphicsPipelines.front().renderTargetFormats[0], RVX::RHIFormat::RGBA16_FLOAT);
    EXPECT_EQ(secondDevice.capturedGraphicsPipelines[4].renderTargetFormats[0], RVX::RHIFormat::RGBA16_FLOAT);
}

TEST_F(PipelineCacheValidationFixture, DefaultDepthFormatIsD32AndForwardZ)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    FakeDevice device;
    RVX::PipelineCache cache;
    ASSERT_TRUE(cache.Initialize(&device, FindShaderDirectory().string())) << cache.GetLastError();

    EXPECT_EQ(cache.GetConfig().depthStencilFormat, RVX::RHIFormat::D32_FLOAT);
    EXPECT_EQ(RVX::PipelineCache::GetDefaultDepthStencilFormat(), RVX::RHIFormat::D32_FLOAT);
    EXPECT_EQ(cache.GetDepthClearValue(), 1.0f);

    ASSERT_GE(device.capturedGraphicsPipelines.size(), 5u);
    const auto& opaqueDesc = device.capturedGraphicsPipelines[0];
    const auto& transparentDesc = device.capturedGraphicsPipelines[2];
    const auto& depthOnlyDesc = device.capturedGraphicsPipelines[3];
    const auto& toneMappingDesc = device.capturedGraphicsPipelines[4];

    EXPECT_EQ(opaqueDesc.depthStencilFormat, RVX::RHIFormat::D32_FLOAT);
    EXPECT_EQ(opaqueDesc.depthStencilState.depthCompareOp, RVX::RHICompareOp::Less);
    EXPECT_TRUE(opaqueDesc.depthStencilState.depthWriteEnable);

    EXPECT_EQ(transparentDesc.depthStencilFormat, RVX::RHIFormat::D32_FLOAT);
    EXPECT_EQ(transparentDesc.depthStencilState.depthCompareOp, RVX::RHICompareOp::Less);
    EXPECT_FALSE(transparentDesc.depthStencilState.depthWriteEnable);

    ASSERT_NE(cache.GetDepthOnlyPipeline(), nullptr);
    EXPECT_EQ(depthOnlyDesc.numRenderTargets, 0u);
    EXPECT_EQ(depthOnlyDesc.renderTargetFormats[0], RVX::RHIFormat::Unknown);
    EXPECT_EQ(depthOnlyDesc.depthStencilFormat, RVX::RHIFormat::D32_FLOAT);
    EXPECT_EQ(depthOnlyDesc.depthStencilState.depthCompareOp, RVX::RHICompareOp::Less);
    EXPECT_TRUE(depthOnlyDesc.depthStencilState.depthWriteEnable);
    EXPECT_EQ(depthOnlyDesc.pixelShader, nullptr);
    ASSERT_EQ(depthOnlyDesc.inputLayout.elements.size(), static_cast<size_t>(1));
    EXPECT_STREQ(depthOnlyDesc.inputLayout.elements[0].semanticName, "POSITION");

    ASSERT_NE(cache.GetToneMappingPipeline(), nullptr);
    EXPECT_EQ(toneMappingDesc.numRenderTargets, 1u);
    EXPECT_EQ(toneMappingDesc.renderTargetFormats[0], RVX::RHIFormat::RGBA8_UNORM);
    EXPECT_EQ(toneMappingDesc.depthStencilFormat, RVX::RHIFormat::Unknown);
    EXPECT_FALSE(toneMappingDesc.depthStencilState.depthTestEnable);
    EXPECT_FALSE(toneMappingDesc.depthStencilState.depthWriteEnable);
    EXPECT_NE(toneMappingDesc.vertexShader, nullptr);
    EXPECT_NE(toneMappingDesc.pixelShader, nullptr);
    EXPECT_TRUE(toneMappingDesc.inputLayout.elements.empty());
}

TEST_F(PipelineCacheValidationFixture, ReverseZOptInChangesDepthCompareAndClearConvention)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    FakeDevice device;
    RVX::PipelineCache cache;
    RVX::PipelineCacheConfig config;
    config.reverseZ = true;
    cache.SetConfig(config);

    ASSERT_TRUE(cache.Initialize(&device, FindShaderDirectory().string())) << cache.GetLastError();
    EXPECT_EQ(cache.GetDepthClearValue(), 0.0f);
    EXPECT_EQ(RVX::PipelineCache::GetDepthClearValue(true), 0.0f);
    EXPECT_EQ(RVX::PipelineCache::GetDepthClearValue(false), 1.0f);

    ASSERT_GE(device.capturedGraphicsPipelines.size(), 5u);
    const auto& opaqueDesc = device.capturedGraphicsPipelines[0];
    const auto& transparentDesc = device.capturedGraphicsPipelines[2];
    const auto& depthOnlyDesc = device.capturedGraphicsPipelines[3];

    EXPECT_EQ(opaqueDesc.depthStencilState.depthCompareOp, RVX::RHICompareOp::GreaterEqual);
    EXPECT_TRUE(opaqueDesc.depthStencilState.depthWriteEnable);

    EXPECT_EQ(transparentDesc.depthStencilState.depthCompareOp, RVX::RHICompareOp::GreaterEqual);
    EXPECT_FALSE(transparentDesc.depthStencilState.depthWriteEnable);

    EXPECT_EQ(depthOnlyDesc.depthStencilState.depthCompareOp, RVX::RHICompareOp::GreaterEqual);
    EXPECT_TRUE(depthOnlyDesc.depthStencilState.depthWriteEnable);
}

TEST_F(PipelineCacheValidationFixture, ManifestMissingIsColdInitAndSavesMetadata)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    TempDirectory temp("rvx_pipeline_manifest_cold");
    FakeDevice device;
    RVX::PipelineCache cache;
    cache.SetConfig(ConfigWithManifest(temp.Path()));

    ASSERT_TRUE(cache.Initialize(&device, FindShaderDirectory().string())) << cache.GetLastError();
    EXPECT_FALSE(cache.GetStats().manifestLoaded);
    EXPECT_FALSE(cache.GetStats().manifestValid);
    EXPECT_FALSE(cache.GetStats().manifestInvalidated);
    EXPECT_TRUE(fs::exists(temp.Path() / RVX::PipelineCache::GetManifestFileName()));
}

TEST_F(PipelineCacheValidationFixture, ManifestReloadsAsValidForSameInputs)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    TempDirectory temp("rvx_pipeline_manifest_reload");

    {
        FakeDevice device;
        RVX::PipelineCache cache;
        cache.SetConfig(ConfigWithManifest(temp.Path()));
        ASSERT_TRUE(cache.Initialize(&device, FindShaderDirectory().string())) << cache.GetLastError();
    }

    FakeDevice device;
    RVX::PipelineCache cache;
    cache.SetConfig(ConfigWithManifest(temp.Path()));
    ASSERT_TRUE(cache.Initialize(&device, FindShaderDirectory().string())) << cache.GetLastError();

    EXPECT_TRUE(cache.GetStats().manifestLoaded);
    EXPECT_TRUE(cache.GetStats().manifestValid);
    EXPECT_FALSE(cache.GetStats().manifestInvalidated);
}

TEST_F(PipelineCacheValidationFixture, ManifestInvalidatesWhenConfigChanges)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    TempDirectory temp("rvx_pipeline_manifest_stale");

    {
        FakeDevice device;
        RVX::PipelineCache cache;
        cache.SetConfig(ConfigWithManifest(temp.Path()));
        ASSERT_TRUE(cache.Initialize(&device, FindShaderDirectory().string())) << cache.GetLastError();
    }

    RVX::PipelineCacheConfig changedConfig = ConfigWithManifest(temp.Path());
    changedConfig.renderTargetFormat = RVX::RHIFormat::RGBA16_FLOAT;

    FakeDevice device;
    RVX::PipelineCache cache;
    cache.SetConfig(changedConfig);
    ASSERT_TRUE(cache.Initialize(&device, FindShaderDirectory().string())) << cache.GetLastError();

    EXPECT_TRUE(cache.GetStats().manifestLoaded);
    EXPECT_FALSE(cache.GetStats().manifestValid);
    EXPECT_TRUE(cache.GetStats().manifestInvalidated);
}

TEST_F(PipelineCacheValidationFixture, ManifestInvalidatesWhenToneMappingPipelineHashChanges)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    TempDirectory temp("rvx_pipeline_manifest_tonemapping_stale");
    const fs::path manifestPath = temp.Path() / RVX::PipelineCache::GetManifestFileName();
    RVX::uint64 firstToneMappingHash = 0;

    {
        FakeDevice device;
        RVX::PipelineCache cache;
        cache.SetConfig(ConfigWithManifest(temp.Path()));
        ASSERT_TRUE(cache.Initialize(&device, FindShaderDirectory().string())) << cache.GetLastError();
        firstToneMappingHash = cache.GetStats().toneMappingPipelineHash;
        ASSERT_NE(firstToneMappingHash, 0u);
    }

    const RVX::uint64 staleToneMappingHash = firstToneMappingHash == 1u ? 2u : 1u;
    ReplaceManifestFieldValue(manifestPath, "toneMappingPipelineHash", std::to_string(staleToneMappingHash));

    FakeDevice device;
    RVX::PipelineCache cache;
    cache.SetConfig(ConfigWithManifest(temp.Path()));
    ASSERT_TRUE(cache.Initialize(&device, FindShaderDirectory().string())) << cache.GetLastError();

    EXPECT_TRUE(cache.GetStats().manifestLoaded);
    EXPECT_FALSE(cache.GetStats().manifestValid);
    EXPECT_TRUE(cache.GetStats().manifestInvalidated);
}

TEST_F(PipelineCacheValidationFixture, CorruptManifestInvalidatesWithoutFailingInitialization)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    TempDirectory temp("rvx_pipeline_manifest_corrupt");
    WriteTextFile(temp.Path() / RVX::PipelineCache::GetManifestFileName(), "not a manifest\n");

    FakeDevice device;
    RVX::PipelineCache cache;
    cache.SetConfig(ConfigWithManifest(temp.Path()));

    ASSERT_TRUE(cache.Initialize(&device, FindShaderDirectory().string())) << cache.GetLastError();
    EXPECT_TRUE(cache.GetStats().manifestLoaded);
    EXPECT_FALSE(cache.GetStats().manifestValid);
    EXPECT_TRUE(cache.GetStats().manifestInvalidated);
}

TEST_F(PipelineCacheValidationFixture, ManifestRejectsInvalidReverseZValue)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    TempDirectory temp("rvx_pipeline_manifest_reversez");
    const fs::path manifestPath = temp.Path() / RVX::PipelineCache::GetManifestFileName();

    {
        FakeDevice device;
        RVX::PipelineCache cache;
        cache.SetConfig(ConfigWithManifest(temp.Path()));
        ASSERT_TRUE(cache.Initialize(&device, FindShaderDirectory().string())) << cache.GetLastError();
    }

    ReplaceManifestFieldValue(manifestPath, "reverseZ", "2");

    FakeDevice device;
    RVX::PipelineCache cache;
    cache.SetConfig(ConfigWithManifest(temp.Path()));

    ASSERT_TRUE(cache.Initialize(&device, FindShaderDirectory().string())) << cache.GetLastError();
    EXPECT_TRUE(cache.GetStats().manifestLoaded);
    EXPECT_FALSE(cache.GetStats().manifestValid);
    EXPECT_TRUE(cache.GetStats().manifestInvalidated);
}

TEST_F(PipelineCacheValidationFixture, ManifestWriteFailureDoesNotFailInitialization)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    TempDirectory temp("rvx_pipeline_manifest_write_failure");
    const fs::path fileInsteadOfDirectory = temp.Path() / "not_a_directory";
    WriteTextFile(fileInsteadOfDirectory, "manifest directory parent is a file");

    RVX::PipelineCacheConfig config;
    config.manifestDirectory = fileInsteadOfDirectory;

    FakeDevice device;
    RVX::PipelineCache cache;
    cache.SetConfig(config);

    EXPECT_TRUE(cache.Initialize(&device, FindShaderDirectory().string())) << cache.GetLastError();
    EXPECT_FALSE(fs::exists(fileInsteadOfDirectory / RVX::PipelineCache::GetManifestFileName()));
}

TEST_F(PipelineCacheValidationFixture, BackendPipelineCreationFailureIsVisible)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    FakeDevice device;
    device.failPipelineCreation = true;
    RVX::PipelineCache cache;

    EXPECT_FALSE(cache.Initialize(&device, FindShaderDirectory().string()));
    EXPECT_NE(cache.GetLastError().find("Backend failed to create pipeline"), std::string::npos);
}

TEST_F(PipelineCacheValidationFixture, ToneMappingPipelineCreationFailureIsVisible)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    FakeDevice device;
    device.failPipelineCreationAtIndex = 5;
    RVX::PipelineCache cache;

    EXPECT_FALSE(cache.Initialize(&device, FindShaderDirectory().string()));
    EXPECT_NE(cache.GetLastError().find("ToneMapping pipeline"), std::string::npos);
}
