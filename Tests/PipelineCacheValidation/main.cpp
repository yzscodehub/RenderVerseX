#include "Core/Log.h"
#include "Render/PipelineCache.h"
#include "Render/Renderer/ViewData.h"
#include "RHI/RHI.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <type_traits>
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
        const std::vector<RVX::uint8>& GetStorage() const { return m_storage; }

    private:
        RVX::RHIBufferDesc m_desc;
        std::vector<RVX::uint8> m_storage;
    };

    class FakeTexture final : public RVX::RHITexture
    {
    public:
        explicit FakeTexture(const RVX::RHITextureDesc& desc)
            : m_desc(desc)
        {
        }

        RVX::uint32 GetWidth() const override { return m_desc.width; }
        RVX::uint32 GetHeight() const override { return m_desc.height; }
        RVX::uint32 GetDepth() const override { return m_desc.depth; }
        RVX::uint32 GetMipLevels() const override { return m_desc.mipLevels; }
        RVX::uint32 GetArraySize() const override { return m_desc.arraySize; }
        RVX::RHIFormat GetFormat() const override { return m_desc.format; }
        RVX::RHITextureUsage GetUsage() const override { return m_desc.usage; }
        RVX::RHITextureDimension GetDimension() const override { return m_desc.dimension; }
        RVX::RHISampleCount GetSampleCount() const override { return m_desc.sampleCount; }

    private:
        RVX::RHITextureDesc m_desc;
    };

    class FakeTextureView final : public RVX::RHITextureView
    {
    public:
        FakeTextureView(RVX::RHITexture* texture, const RVX::RHITextureViewDesc& desc)
            : m_texture(texture)
            , m_desc(desc)
        {
            if (m_desc.format == RVX::RHIFormat::Unknown && m_texture)
            {
                m_desc.format = m_texture->GetFormat();
            }
        }

        RVX::RHITexture* GetTexture() const override { return m_texture; }
        RVX::RHIFormat GetFormat() const override { return m_desc.format; }
        const RVX::RHISubresourceRange& GetSubresourceRange() const override { return m_desc.subresourceRange; }

    private:
        RVX::RHITexture* m_texture = nullptr;
        RVX::RHITextureViewDesc m_desc;
    };

    class FakeSampler final : public RVX::RHISampler
    {
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
        explicit FakeDescriptorSet(const RVX::RHIDescriptorSetDesc& desc)
            : m_bindings(desc.bindings)
        {
        }

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
            auto buffer = RVX::MakeRef<FakeBuffer>(desc);
            capturedBufferDescs.push_back(desc);
            capturedBuffers.push_back(buffer.Get());
            return buffer;
        }

        RVX::RHITextureRef CreateTexture(const RVX::RHITextureDesc& desc) override
        {
            capturedTextureDescs.push_back(desc);
            return RVX::MakeRef<FakeTexture>(desc);
        }

        RVX::RHITextureViewRef CreateTextureView(RVX::RHITexture* texture, const RVX::RHITextureViewDesc& desc) override
        {
            capturedTextureViewDescs.push_back(desc);
            return RVX::MakeRef<FakeTextureView>(texture, desc);
        }

        RVX::RHISamplerRef CreateSampler(const RVX::RHISamplerDesc& desc) override
        {
            capturedSamplerDescs.push_back(desc);
            return RVX::MakeRef<FakeSampler>();
        }

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
        RVX::RHIDescriptorSetRef CreateDescriptorSet(const RVX::RHIDescriptorSetDesc& desc) override
        {
            capturedDescriptorSetDescs.push_back(desc);
            return RVX::MakeRef<FakeDescriptorSet>(desc);
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
        std::vector<RVX::RHIBufferDesc> capturedBufferDescs;
        std::vector<FakeBuffer*> capturedBuffers;
        std::vector<RVX::RHITextureDesc> capturedTextureDescs;
        std::vector<RVX::RHITextureViewDesc> capturedTextureViewDescs;
        std::vector<RVX::RHISamplerDesc> capturedSamplerDescs;
        std::vector<RVX::RHIDescriptorSetDesc> capturedDescriptorSetDescs;
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

    const RVX::RHIDescriptorBinding* FindDescriptorBinding(
        const RVX::RHIDescriptorSetDesc& desc,
        RVX::uint32 binding)
    {
        auto it = std::find_if(desc.bindings.begin(), desc.bindings.end(),
            [binding](const RVX::RHIDescriptorBinding& entry)
            {
                return entry.binding == binding;
            });
        return it == desc.bindings.end() ? nullptr : &(*it);
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

    fs::path CopyShaderDirectoryToTemp(const fs::path& destinationRoot)
    {
        const fs::path source = FindShaderDirectory();
        EXPECT_FALSE(source.empty());
        const fs::path destination = destinationRoot / "Shaders";
        fs::create_directories(destination);
        fs::copy(source,
                 destination,
                 fs::copy_options::recursive | fs::copy_options::overwrite_existing);
        return destination;
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

    const FakeBuffer* FindCapturedBuffer(const FakeDevice& device, const char* debugName)
    {
        for (size_t i = 0; i < device.capturedBufferDescs.size(); ++i)
        {
            const char* capturedName = device.capturedBufferDescs[i].debugName;
            if (capturedName && std::strcmp(capturedName, debugName) == 0)
            {
                return device.capturedBuffers[i];
            }
        }
        return nullptr;
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

TEST_F(PipelineCacheValidationFixture, MissingBloomShaderFailsWithVisibleError)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    const fs::path sourceDir = FindShaderDirectory();
    TempDirectory temp("rvx_pipeline_missing_bloom_shader");
    fs::copy_file(sourceDir / "DefaultLit.hlsl", temp.Path() / "DefaultLit.hlsl");
    fs::copy_file(sourceDir / "DepthOnly.hlsl", temp.Path() / "DepthOnly.hlsl");
    fs::copy(sourceDir / "Include", temp.Path() / "Include", fs::copy_options::recursive);
    fs::create_directories(temp.Path() / "PostProcess");
    fs::copy_file(sourceDir / "PostProcess" / "ToneMapping.hlsl",
                  temp.Path() / "PostProcess" / "ToneMapping.hlsl");

    FakeDevice device;
    RVX::PipelineCache cache;

    EXPECT_FALSE(cache.Initialize(&device, temp.Path().string()));
    EXPECT_NE(cache.GetLastError().find("Bloom shader file not found"), std::string::npos);
}

TEST_F(PipelineCacheValidationFixture, MissingFXAAShaderFailsWithVisibleError)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    const fs::path sourceDir = FindShaderDirectory();
    TempDirectory temp("rvx_pipeline_missing_fxaa_shader");
    fs::copy_file(sourceDir / "DefaultLit.hlsl", temp.Path() / "DefaultLit.hlsl");
    fs::copy_file(sourceDir / "DepthOnly.hlsl", temp.Path() / "DepthOnly.hlsl");
    fs::copy(sourceDir / "Include", temp.Path() / "Include", fs::copy_options::recursive);
    fs::create_directories(temp.Path() / "PostProcess");
    fs::copy_file(sourceDir / "PostProcess" / "ToneMapping.hlsl",
                  temp.Path() / "PostProcess" / "ToneMapping.hlsl");
    fs::copy_file(sourceDir / "PostProcess" / "Bloom.hlsl",
                  temp.Path() / "PostProcess" / "Bloom.hlsl");
    fs::copy_file(sourceDir / "PostProcess" / "ColorGrading.hlsl",
                  temp.Path() / "PostProcess" / "ColorGrading.hlsl");
    fs::copy_file(sourceDir / "PostProcess" / "ChromaticAberration.hlsl",
                  temp.Path() / "PostProcess" / "ChromaticAberration.hlsl");

    FakeDevice device;
    RVX::PipelineCache cache;

    EXPECT_FALSE(cache.Initialize(&device, temp.Path().string()));
    EXPECT_NE(cache.GetLastError().find("FXAA shader file not found"), std::string::npos);
}

TEST_F(PipelineCacheValidationFixture, MissingChromaticAberrationShaderFailsWithVisibleError)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    const fs::path sourceDir = FindShaderDirectory();
    TempDirectory temp("rvx_pipeline_missing_chromatic_aberration_shader");
    fs::copy_file(sourceDir / "DefaultLit.hlsl", temp.Path() / "DefaultLit.hlsl");
    fs::copy_file(sourceDir / "DepthOnly.hlsl", temp.Path() / "DepthOnly.hlsl");
    fs::copy(sourceDir / "Include", temp.Path() / "Include", fs::copy_options::recursive);
    fs::create_directories(temp.Path() / "PostProcess");
    fs::copy_file(sourceDir / "PostProcess" / "ToneMapping.hlsl",
                  temp.Path() / "PostProcess" / "ToneMapping.hlsl");
    fs::copy_file(sourceDir / "PostProcess" / "Bloom.hlsl",
                  temp.Path() / "PostProcess" / "Bloom.hlsl");
    fs::copy_file(sourceDir / "PostProcess" / "ColorGrading.hlsl",
                  temp.Path() / "PostProcess" / "ColorGrading.hlsl");

    FakeDevice device;
    RVX::PipelineCache cache;

    EXPECT_FALSE(cache.Initialize(&device, temp.Path().string()));
    EXPECT_NE(cache.GetLastError().find("ChromaticAberration shader file not found"), std::string::npos);
}

TEST_F(PipelineCacheValidationFixture, MissingColorGradingShaderFailsWithVisibleError)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    const fs::path sourceDir = FindShaderDirectory();
    TempDirectory temp("rvx_pipeline_missing_colorgrading_shader");
    fs::copy_file(sourceDir / "DefaultLit.hlsl", temp.Path() / "DefaultLit.hlsl");
    fs::copy_file(sourceDir / "DepthOnly.hlsl", temp.Path() / "DepthOnly.hlsl");
    fs::copy(sourceDir / "Include", temp.Path() / "Include", fs::copy_options::recursive);
    fs::create_directories(temp.Path() / "PostProcess");
    fs::copy_file(sourceDir / "PostProcess" / "ToneMapping.hlsl",
                  temp.Path() / "PostProcess" / "ToneMapping.hlsl");
    fs::copy_file(sourceDir / "PostProcess" / "Bloom.hlsl",
                  temp.Path() / "PostProcess" / "Bloom.hlsl");

    FakeDevice device;
    RVX::PipelineCache cache;

    EXPECT_FALSE(cache.Initialize(&device, temp.Path().string()));
    EXPECT_NE(cache.GetLastError().find("ColorGrading shader file not found"), std::string::npos);
}

TEST_F(PipelineCacheValidationFixture, MissingVignetteShaderFailsWithVisibleError)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    const fs::path sourceDir = FindShaderDirectory();
    TempDirectory temp("rvx_pipeline_missing_vignette_shader");
    fs::copy_file(sourceDir / "DefaultLit.hlsl", temp.Path() / "DefaultLit.hlsl");
    fs::copy_file(sourceDir / "DepthOnly.hlsl", temp.Path() / "DepthOnly.hlsl");
    fs::copy(sourceDir / "Include", temp.Path() / "Include", fs::copy_options::recursive);
    fs::create_directories(temp.Path() / "PostProcess");
    fs::copy_file(sourceDir / "PostProcess" / "ToneMapping.hlsl",
                  temp.Path() / "PostProcess" / "ToneMapping.hlsl");
    fs::copy_file(sourceDir / "PostProcess" / "Bloom.hlsl",
                  temp.Path() / "PostProcess" / "Bloom.hlsl");
    fs::copy_file(sourceDir / "PostProcess" / "ColorGrading.hlsl",
                  temp.Path() / "PostProcess" / "ColorGrading.hlsl");
    fs::copy_file(sourceDir / "PostProcess" / "ChromaticAberration.hlsl",
                  temp.Path() / "PostProcess" / "ChromaticAberration.hlsl");
    fs::copy_file(sourceDir / "PostProcess" / "FXAA.hlsl",
                  temp.Path() / "PostProcess" / "FXAA.hlsl");

    FakeDevice device;
    RVX::PipelineCache cache;

    EXPECT_FALSE(cache.Initialize(&device, temp.Path().string()));
    EXPECT_NE(cache.GetLastError().find("Vignette shader file not found"), std::string::npos);
}

TEST_F(PipelineCacheValidationFixture, MissingSkyboxShaderFailsWithVisibleError)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    const fs::path sourceDir = FindShaderDirectory();
    TempDirectory temp("rvx_pipeline_missing_skybox_shader");
    fs::copy_file(sourceDir / "DefaultLit.hlsl", temp.Path() / "DefaultLit.hlsl");
    fs::copy_file(sourceDir / "DepthOnly.hlsl", temp.Path() / "DepthOnly.hlsl");
    fs::copy(sourceDir / "Include", temp.Path() / "Include", fs::copy_options::recursive);
    fs::create_directories(temp.Path() / "PostProcess");
    fs::copy_file(sourceDir / "PostProcess" / "ToneMapping.hlsl",
                  temp.Path() / "PostProcess" / "ToneMapping.hlsl");
    fs::copy_file(sourceDir / "PostProcess" / "Bloom.hlsl",
                  temp.Path() / "PostProcess" / "Bloom.hlsl");
    fs::copy_file(sourceDir / "PostProcess" / "ColorGrading.hlsl",
                  temp.Path() / "PostProcess" / "ColorGrading.hlsl");
    fs::copy_file(sourceDir / "PostProcess" / "ChromaticAberration.hlsl",
                  temp.Path() / "PostProcess" / "ChromaticAberration.hlsl");
    fs::copy_file(sourceDir / "PostProcess" / "FXAA.hlsl",
                  temp.Path() / "PostProcess" / "FXAA.hlsl");
    fs::copy_file(sourceDir / "PostProcess" / "Vignette.hlsl",
                  temp.Path() / "PostProcess" / "Vignette.hlsl");

    FakeDevice device;
    RVX::PipelineCache cache;

    EXPECT_FALSE(cache.Initialize(&device, temp.Path().string()));
    EXPECT_NE(cache.GetLastError().find("Skybox shader file not found"), std::string::npos);
}

TEST_F(PipelineCacheValidationFixture, SkyboxShaderUsesFullscreenTriangleAndProceduralGradient)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    const std::string shaderSource = ReadTextFile(FindShaderDirectory() / "Skybox.hlsl");

    EXPECT_NE(shaderSource.find("SV_VertexID"), std::string::npos);
    EXPECT_NE(shaderSource.find("SkyboxGroundColor.a"), std::string::npos);
    EXPECT_NE(shaderSource.find("SkyboxZenithColor"), std::string::npos);
    EXPECT_NE(shaderSource.find("SkyboxHorizonColor"), std::string::npos);
    EXPECT_NE(shaderSource.find("TextureCube SkyboxCubemap"), std::string::npos);
    EXPECT_NE(shaderSource.find("SamplerState SkyboxSampler"), std::string::npos);
    EXPECT_NE(shaderSource.find("SkyboxTextureParams.x"), std::string::npos);
    EXPECT_NE(shaderSource.find("smoothstep"), std::string::npos);
}

TEST_F(PipelineCacheValidationFixture, ToneMappingShaderUsesSingleDisplayConversion)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    const std::string shaderSource =
        ReadTextFile(FindShaderDirectory() / "PostProcess" / "ToneMapping.hlsl");

    auto countOccurrences = [](const std::string& text, const std::string& needle)
    {
        size_t count = 0;
        size_t offset = 0;
        while ((offset = text.find(needle, offset)) != std::string::npos)
        {
            ++count;
            offset += needle.size();
        }
        return count;
    };

    EXPECT_EQ(shaderSource.find("LinearToSRGB"), std::string::npos);
    EXPECT_EQ(countOccurrences(shaderSource, "ApplyDisplayConversion("), static_cast<size_t>(2));
    EXPECT_EQ(countOccurrences(shaderSource, "pow("), static_cast<size_t>(1));
}

TEST_F(PipelineCacheValidationFixture, FXAAShaderUsesPostProcessDescriptorLayout)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    const std::string shaderSource =
        ReadTextFile(FindShaderDirectory() / "PostProcess" / "FXAA.hlsl");

    EXPECT_NE(shaderSource.find("cbuffer FXAAConstants : register(b0, space0)"), std::string::npos);
    EXPECT_NE(shaderSource.find("Texture2D<float4> InputTexture : register(t1, space0)"), std::string::npos);
    EXPECT_NE(shaderSource.find("SamplerState LinearSampler : register(s2, space0)"), std::string::npos);
}

TEST_F(PipelineCacheValidationFixture, VignetteShaderUsesPostProcessDescriptorLayout)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    const std::string shaderSource =
        ReadTextFile(FindShaderDirectory() / "PostProcess" / "Vignette.hlsl");

    EXPECT_NE(shaderSource.find("cbuffer VignetteConstants : register(b0, space0)"), std::string::npos);
    EXPECT_NE(shaderSource.find("Texture2D<float4> InputTexture : register(t1, space0)"), std::string::npos);
    EXPECT_NE(shaderSource.find("SamplerState LinearSampler : register(s2, space0)"), std::string::npos);
}

TEST_F(PipelineCacheValidationFixture, ColorGradingShaderUsesPostProcessDescriptorLayout)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    const std::string shaderSource =
        ReadTextFile(FindShaderDirectory() / "PostProcess" / "ColorGrading.hlsl");

    EXPECT_NE(shaderSource.find("cbuffer ColorGradingConstants : register(b0, space0)"), std::string::npos);
    EXPECT_NE(shaderSource.find("Texture2D<float4> InputTexture : register(t1, space0)"), std::string::npos);
    EXPECT_NE(shaderSource.find("SamplerState LinearSampler : register(s2, space0)"), std::string::npos);
    EXPECT_EQ(shaderSource.find("RWTexture2D"), std::string::npos);
}

TEST_F(PipelineCacheValidationFixture, ChromaticAberrationShaderUsesPostProcessDescriptorLayout)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    const std::string shaderSource =
        ReadTextFile(FindShaderDirectory() / "PostProcess" / "ChromaticAberration.hlsl");

    EXPECT_NE(shaderSource.find("cbuffer ChromaticAberrationConstants : register(b0, space0)"),
              std::string::npos);
    EXPECT_NE(shaderSource.find("Texture2D<float4> InputTexture : register(t1, space0)"), std::string::npos);
    EXPECT_NE(shaderSource.find("SamplerState LinearSampler : register(s2, space0)"), std::string::npos);
    EXPECT_NE(shaderSource.find("VSMain"), std::string::npos);
    EXPECT_NE(shaderSource.find("PSMain"), std::string::npos);
    EXPECT_EQ(shaderSource.find("RWTexture2D"), std::string::npos);
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
    ASSERT_GE(device.capturedSetLayouts.size(), 5u);
    ASSERT_GE(device.capturedPipelineLayoutSetCounts.size(), 3u);
    EXPECT_EQ(device.capturedPipelineLayoutSetCounts[0], 3u);
    EXPECT_EQ(device.capturedPipelineLayoutSetCounts[1], 1u);
    EXPECT_EQ(device.capturedPipelineLayoutSetCounts[2], 1u);

    const auto& frameLayout = device.capturedSetLayouts[0];
    const auto& objectLayout = device.capturedSetLayouts[1];
    const auto& materialLayout = device.capturedSetLayouts[2];
    const auto& postProcessLayout = device.capturedSetLayouts[3];
    const auto& skyboxLayout = device.capturedSetLayouts[4];

    const auto* frame = FindBinding(frameLayout, 0);
    ASSERT_NE(frame, nullptr);
    EXPECT_EQ(frame->type, RVX::RHIBindingType::UniformBuffer);
    EXPECT_TRUE(RVX::HasFlag(frame->visibility, RVX::RHIShaderStage::Vertex));
    EXPECT_TRUE(RVX::HasFlag(frame->visibility, RVX::RHIShaderStage::Pixel));
    const auto* frameShadowTexture = FindBinding(frameLayout, 1);
    ASSERT_NE(frameShadowTexture, nullptr);
    EXPECT_EQ(frameShadowTexture->type, RVX::RHIBindingType::SampledTexture);
    EXPECT_TRUE(RVX::HasFlag(frameShadowTexture->visibility, RVX::RHIShaderStage::Pixel));
    const auto* frameShadowSampler = FindBinding(frameLayout, 2);
    ASSERT_NE(frameShadowSampler, nullptr);
    EXPECT_EQ(frameShadowSampler->type, RVX::RHIBindingType::Sampler);
    EXPECT_TRUE(RVX::HasFlag(frameShadowSampler->visibility, RVX::RHIShaderStage::Pixel));

    ASSERT_FALSE(device.capturedDescriptorSetDescs.empty());
    const RVX::RHIDescriptorSetDesc& frameSetDesc = device.capturedDescriptorSetDescs.front();
    EXPECT_NE(FindDescriptorBinding(frameSetDesc, 0), nullptr);
    const auto* fallbackShadowTexture = FindDescriptorBinding(frameSetDesc, 1);
    ASSERT_NE(fallbackShadowTexture, nullptr);
    EXPECT_NE(fallbackShadowTexture->textureView, nullptr);
    const auto* fallbackShadowSampler = FindDescriptorBinding(frameSetDesc, 2);
    ASSERT_NE(fallbackShadowSampler, nullptr);
    EXPECT_NE(fallbackShadowSampler->sampler, nullptr);

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

    for (RVX::uint32 binding = 7; binding <= 9; ++binding)
    {
        const auto* iblTexture = FindBinding(materialLayout, binding);
        ASSERT_NE(iblTexture, nullptr);
        EXPECT_EQ(iblTexture->type, RVX::RHIBindingType::SampledTexture);
    }

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

    ASSERT_EQ(skyboxLayout.entries.size(), static_cast<size_t>(3));
    const auto* skyboxConstants = FindBinding(skyboxLayout, 0);
    ASSERT_NE(skyboxConstants, nullptr);
    EXPECT_EQ(skyboxConstants->type, RVX::RHIBindingType::UniformBuffer);
    EXPECT_TRUE(RVX::HasFlag(skyboxConstants->visibility, RVX::RHIShaderStage::Vertex));
    EXPECT_TRUE(RVX::HasFlag(skyboxConstants->visibility, RVX::RHIShaderStage::Pixel));
    const auto* skyboxTexture = FindBinding(skyboxLayout, 1);
    ASSERT_NE(skyboxTexture, nullptr);
    EXPECT_EQ(skyboxTexture->type, RVX::RHIBindingType::SampledTexture);
    EXPECT_TRUE(RVX::HasFlag(skyboxTexture->visibility, RVX::RHIShaderStage::Pixel));
    const auto* skyboxSampler = FindBinding(skyboxLayout, 2);
    ASSERT_NE(skyboxSampler, nullptr);
    EXPECT_EQ(skyboxSampler->type, RVX::RHIBindingType::Sampler);
    EXPECT_TRUE(RVX::HasFlag(skyboxSampler->visibility, RVX::RHIShaderStage::Pixel));
    EXPECT_NE(cache.GetSkyboxSetLayout(), nullptr);
    EXPECT_NE(cache.GetSkyboxLayout(), nullptr);
}

TEST_F(PipelineCacheValidationFixture, ViewConstantsLayoutMatchesDefaultLitCBufferPacking)
{
    EXPECT_TRUE(std::is_standard_layout_v<RVX::ViewConstants>);
    EXPECT_EQ(sizeof(RVX::Mat4), 64u);
    EXPECT_EQ(sizeof(RVX::Vec3), 12u);
    EXPECT_EQ(sizeof(RVX::Vec4), 16u);
    EXPECT_EQ(offsetof(RVX::ViewConstants, viewProjection), 0u);
    EXPECT_EQ(offsetof(RVX::ViewConstants, cameraPosition), 64u);
    EXPECT_EQ(offsetof(RVX::ViewConstants, time), 76u);
    EXPECT_EQ(offsetof(RVX::ViewConstants, lightDirection), 80u);
    EXPECT_EQ(offsetof(RVX::ViewConstants, directionalLightIntensity), 92u);
    EXPECT_EQ(offsetof(RVX::ViewConstants, iblDiffuseAmbient), 96u);
    EXPECT_EQ(offsetof(RVX::ViewConstants, iblSpecularAmbient), 112u);
    EXPECT_EQ(offsetof(RVX::ViewConstants, iblTextureParams), 128u);
    EXPECT_EQ(offsetof(RVX::ViewConstants, directionalShadowViewProjection), 144u);
    EXPECT_EQ(offsetof(RVX::ViewConstants, directionalShadowParams), 208u);
    EXPECT_EQ(offsetof(RVX::ViewConstants, directionalShadowReceiverParams), 224u);
    EXPECT_EQ(sizeof(RVX::ViewConstants), 240u);
}

TEST_F(PipelineCacheValidationFixture, ObjectConstantsLayoutMatchesDefaultLitCBufferPacking)
{
    EXPECT_TRUE(std::is_standard_layout_v<RVX::ObjectConstants>);
    EXPECT_EQ(offsetof(RVX::ObjectConstants, world), 0u);
    EXPECT_EQ(offsetof(RVX::ObjectConstants, normalMatrix), 64u);
    EXPECT_EQ(sizeof(RVX::ObjectConstants), 128u);
}

TEST_F(PipelineCacheValidationFixture, ObjectConstantBufferUsesAlignedObjectConstantsStride)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    FakeDevice device;
    RVX::PipelineCache cache;
    ASSERT_TRUE(cache.Initialize(&device, FindShaderDirectory().string())) << cache.GetLastError();

    constexpr RVX::uint64 kExpectedConstantBufferAlignment = 256;
    constexpr RVX::uint64 kExpectedDrawConstantSlots = 8192;
    const RVX::uint64 expectedStride =
        (sizeof(RVX::ObjectConstants) + kExpectedConstantBufferAlignment - 1u) &
        ~(kExpectedConstantBufferAlignment - 1u);

    const FakeBuffer* objectBuffer = FindCapturedBuffer(device, "ObjectConstantBuffer");
    ASSERT_NE(objectBuffer, nullptr);
    EXPECT_EQ(objectBuffer->GetSize(), expectedStride * kExpectedDrawConstantSlots);

    const auto objectDescIt = std::find_if(device.capturedDescriptorSetDescs.begin(),
                                           device.capturedDescriptorSetDescs.end(),
                                           [](const RVX::RHIDescriptorSetDesc& desc)
                                           {
                                               return desc.debugName &&
                                                      std::strcmp(desc.debugName, "DefaultObjectDescriptorSet") == 0;
                                           });
    ASSERT_NE(objectDescIt, device.capturedDescriptorSetDescs.end());
    const RVX::RHIDescriptorBinding* objectBinding = FindDescriptorBinding(*objectDescIt, 0);
    ASSERT_NE(objectBinding, nullptr);
    EXPECT_EQ(objectBinding->range, expectedStride);
}

TEST_F(PipelineCacheValidationFixture, UpdateObjectConstantsUploadsWorldAndNormalMatrices)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    FakeDevice device;
    RVX::PipelineCache cache;
    ASSERT_TRUE(cache.Initialize(&device, FindShaderDirectory().string())) << cache.GetLastError();

    RVX::Mat4 world = RVX::Mat4Identity();
    world[3][0] = 4.0f;
    world[3][1] = 5.0f;
    world[3][2] = 6.0f;

    RVX::Mat4 normalMatrix = RVX::Mat4Identity();
    normalMatrix[0][0] = 0.5f;
    normalMatrix[1][1] = 2.0f;
    normalMatrix[2][2] = 3.0f;

    cache.UpdateObjectConstants(world, normalMatrix);

    const FakeBuffer* objectBuffer = FindCapturedBuffer(device, "ObjectConstantBuffer");
    ASSERT_NE(objectBuffer, nullptr);
    ASSERT_GE(objectBuffer->GetStorage().size(), sizeof(RVX::ObjectConstants));

    RVX::ObjectConstants uploaded{};
    std::memcpy(&uploaded, objectBuffer->GetStorage().data(), sizeof(uploaded));
    EXPECT_FLOAT_EQ(uploaded.world[3][0], 4.0f);
    EXPECT_FLOAT_EQ(uploaded.world[3][1], 5.0f);
    EXPECT_FLOAT_EQ(uploaded.world[3][2], 6.0f);
    EXPECT_FLOAT_EQ(uploaded.normalMatrix[0][0], 0.5f);
    EXPECT_FLOAT_EQ(uploaded.normalMatrix[1][1], 2.0f);
    EXPECT_FLOAT_EQ(uploaded.normalMatrix[2][2], 3.0f);
}

TEST_F(PipelineCacheValidationFixture, DefaultLitUsesObjectNormalMatrix)
{
    const std::string shader = ReadTextFile(FindShaderDirectory() / "DefaultLit.hlsl");
    EXPECT_NE(shader.find("float4x4 NormalMatrix;"), std::string::npos);
    EXPECT_NE(shader.find("mul((float3x3)NormalMatrix, input.Normal)"), std::string::npos);
    EXPECT_EQ(shader.find("mul((float3x3)World, input.Normal)"), std::string::npos);
}

TEST_F(PipelineCacheValidationFixture, DrawPassesUploadRenderObjectNormalMatrix)
{
    const fs::path passesDir = FindShaderDirectory().parent_path() / "Private" / "Passes";

    const std::string opaquePass = ReadTextFile(passesDir / "OpaquePass.cpp");
    const std::string transparentPass = ReadTextFile(passesDir / "TransparentPass.cpp");
    const std::string depthPrepass = ReadTextFile(passesDir / "DepthPrepass.cpp");
    const std::string shadowPass = ReadTextFile(passesDir / "ShadowPass.cpp");

    EXPECT_NE(opaquePass.find("UpdateObjectConstants(obj.worldMatrix, obj.normalMatrix)"), std::string::npos);
    EXPECT_NE(transparentPass.find("UpdateObjectConstants(obj.worldMatrix, obj.normalMatrix)"), std::string::npos);
    EXPECT_NE(depthPrepass.find("UpdateObjectConstants(obj.worldMatrix, obj.normalMatrix)"), std::string::npos);
    EXPECT_NE(shadowPass.find("UpdateObjectConstants(obj.worldMatrix, obj.normalMatrix)"), std::string::npos);
}

TEST_F(PipelineCacheValidationFixture, UpdateViewConstantsUploadsDefaultIBLAmbientValues)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    FakeDevice device;
    RVX::PipelineCache cache;
    ASSERT_TRUE(cache.Initialize(&device, FindShaderDirectory().string())) << cache.GetLastError();

    RVX::ViewData view;
    view.cameraPosition = RVX::Vec3(1.0f, 2.0f, 3.0f);
    view.time = 4.0f;
    cache.UpdateViewConstants(view);

    const FakeBuffer* viewBuffer = FindCapturedBuffer(device, "ViewConstantBuffer");
    ASSERT_NE(viewBuffer, nullptr);
    ASSERT_GE(viewBuffer->GetStorage().size(), sizeof(RVX::ViewConstants));

    RVX::ViewConstants uploaded{};
    std::memcpy(&uploaded, viewBuffer->GetStorage().data(), sizeof(uploaded));
    EXPECT_NEAR(uploaded.lightDirection.x, 0.505076f, 0.00001f);
    EXPECT_NEAR(uploaded.lightDirection.y, -0.808122f, 0.00001f);
    EXPECT_NEAR(uploaded.lightDirection.z, 0.303046f, 0.00001f);
    EXPECT_FLOAT_EQ(uploaded.directionalLightIntensity, 4.0f);
    EXPECT_FLOAT_EQ(uploaded.iblDiffuseAmbient.x, 1.0f);
    EXPECT_FLOAT_EQ(uploaded.iblDiffuseAmbient.y, 1.0f);
    EXPECT_FLOAT_EQ(uploaded.iblDiffuseAmbient.z, 1.0f);
    EXPECT_FLOAT_EQ(uploaded.iblDiffuseAmbient.w, 0.12f);
    EXPECT_FLOAT_EQ(uploaded.iblSpecularAmbient.x, 1.0f);
    EXPECT_FLOAT_EQ(uploaded.iblSpecularAmbient.y, 1.0f);
    EXPECT_FLOAT_EQ(uploaded.iblSpecularAmbient.z, 1.0f);
    EXPECT_FLOAT_EQ(uploaded.iblSpecularAmbient.w, 0.04f);
    EXPECT_FLOAT_EQ(uploaded.iblTextureParams.x, 0.0f);
    EXPECT_FLOAT_EQ(uploaded.iblTextureParams.y, 1.0f);
    EXPECT_FLOAT_EQ(uploaded.iblTextureParams.z, 1.0f);
    EXPECT_FLOAT_EQ(uploaded.iblTextureParams.w, 0.08f);
    EXPECT_FLOAT_EQ(uploaded.directionalShadowViewProjection[0][0], 1.0f);
    EXPECT_FLOAT_EQ(uploaded.directionalShadowViewProjection[1][1], 1.0f);
    EXPECT_FLOAT_EQ(uploaded.directionalShadowViewProjection[2][2], 1.0f);
    EXPECT_FLOAT_EQ(uploaded.directionalShadowViewProjection[3][3], 1.0f);
    EXPECT_FLOAT_EQ(uploaded.directionalShadowParams.x, 0.0f);
    EXPECT_FLOAT_EQ(uploaded.directionalShadowParams.y, 0.005f);
    EXPECT_FLOAT_EQ(uploaded.directionalShadowParams.z, 1.0f);
    EXPECT_FLOAT_EQ(uploaded.directionalShadowParams.w, 0.0f);
}

TEST_F(PipelineCacheValidationFixture, UpdateViewConstantsUploadsCustomAndDisabledIBLAmbientValues)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    FakeDevice device;
    RVX::PipelineCache cache;
    ASSERT_TRUE(cache.Initialize(&device, FindShaderDirectory().string())) << cache.GetLastError();

    RVX::ViewData view;
    view.directionalLightDirection = RVX::Vec3(0.0f, -2.0f, 0.0f);
    view.directionalLightIntensity = 2.5f;
    view.iblDiffuseColor = RVX::Vec3(0.25f, 0.5f, 0.75f);
    view.iblDiffuseIntensity = 0.8f;
    view.iblSpecularColor = RVX::Vec3(0.1f, 0.2f, 0.3f);
    view.iblSpecularIntensity = 0.6f;
    view.iblAmbientEnabled = 2;
    view.textureIBLEnabled = 1;
    view.textureIBLPrefilteredMipLevels = 5;
    view.textureIBLIntensity = 1.7f;
    view.ambientFloorIntensity = 0.03f;
    cache.UpdateViewConstants(view);

    const FakeBuffer* viewBuffer = FindCapturedBuffer(device, "ViewConstantBuffer");
    ASSERT_NE(viewBuffer, nullptr);
    ASSERT_GE(viewBuffer->GetStorage().size(), sizeof(RVX::ViewConstants));

    RVX::ViewConstants uploaded{};
    std::memcpy(&uploaded, viewBuffer->GetStorage().data(), sizeof(uploaded));
    EXPECT_FLOAT_EQ(uploaded.lightDirection.x, 0.0f);
    EXPECT_FLOAT_EQ(uploaded.lightDirection.y, -1.0f);
    EXPECT_FLOAT_EQ(uploaded.lightDirection.z, 0.0f);
    EXPECT_FLOAT_EQ(uploaded.directionalLightIntensity, 2.5f);
    EXPECT_FLOAT_EQ(uploaded.iblDiffuseAmbient.x, 0.25f);
    EXPECT_FLOAT_EQ(uploaded.iblDiffuseAmbient.y, 0.5f);
    EXPECT_FLOAT_EQ(uploaded.iblDiffuseAmbient.z, 0.75f);
    EXPECT_FLOAT_EQ(uploaded.iblDiffuseAmbient.w, 0.8f);
    EXPECT_FLOAT_EQ(uploaded.iblSpecularAmbient.x, 0.1f);
    EXPECT_FLOAT_EQ(uploaded.iblSpecularAmbient.y, 0.2f);
    EXPECT_FLOAT_EQ(uploaded.iblSpecularAmbient.z, 0.3f);
    EXPECT_FLOAT_EQ(uploaded.iblSpecularAmbient.w, 0.6f);
    EXPECT_FLOAT_EQ(uploaded.iblTextureParams.x, 1.0f);
    EXPECT_FLOAT_EQ(uploaded.iblTextureParams.y, 5.0f);
    EXPECT_FLOAT_EQ(uploaded.iblTextureParams.z, 1.7f);
    EXPECT_FLOAT_EQ(uploaded.iblTextureParams.w, 0.03f);

    view.iblAmbientEnabled = 0;
    view.textureIBLEnabled = 0;
    cache.UpdateViewConstants(view);
    std::memcpy(&uploaded, viewBuffer->GetStorage().data(), sizeof(uploaded));
    EXPECT_FLOAT_EQ(uploaded.iblDiffuseAmbient.x, 0.25f);
    EXPECT_FLOAT_EQ(uploaded.iblDiffuseAmbient.y, 0.5f);
    EXPECT_FLOAT_EQ(uploaded.iblDiffuseAmbient.z, 0.75f);
    EXPECT_FLOAT_EQ(uploaded.iblDiffuseAmbient.w, 0.0f);
    EXPECT_FLOAT_EQ(uploaded.iblSpecularAmbient.x, 0.1f);
    EXPECT_FLOAT_EQ(uploaded.iblSpecularAmbient.y, 0.2f);
    EXPECT_FLOAT_EQ(uploaded.iblSpecularAmbient.z, 0.3f);
    EXPECT_FLOAT_EQ(uploaded.iblSpecularAmbient.w, 0.0f);
    EXPECT_FLOAT_EQ(uploaded.iblTextureParams.x, 0.0f);
    EXPECT_FLOAT_EQ(uploaded.iblTextureParams.y, 5.0f);
    EXPECT_FLOAT_EQ(uploaded.iblTextureParams.z, 1.7f);
    EXPECT_FLOAT_EQ(uploaded.iblTextureParams.w, 0.03f);
}

TEST_F(PipelineCacheValidationFixture, UpdateViewConstantsUploadsZeroAmbientFloorForTextureIBLReadyViews)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    FakeDevice device;
    RVX::PipelineCache cache;
    ASSERT_TRUE(cache.Initialize(&device, FindShaderDirectory().string())) << cache.GetLastError();

    RVX::ViewData view;
    view.textureIBLEnabled = 1;
    view.textureIBLPrefilteredMipLevels = 4;
    view.textureIBLIntensity = 1.0f;
    view.ambientFloorIntensity = 0.0f;
    cache.UpdateViewConstants(view);

    const FakeBuffer* viewBuffer = FindCapturedBuffer(device, "ViewConstantBuffer");
    ASSERT_NE(viewBuffer, nullptr);

    RVX::ViewConstants uploaded{};
    std::memcpy(&uploaded, viewBuffer->GetStorage().data(), sizeof(uploaded));
    EXPECT_FLOAT_EQ(uploaded.iblTextureParams.x, 1.0f);
    EXPECT_FLOAT_EQ(uploaded.iblTextureParams.w, 0.0f);
}

TEST_F(PipelineCacheValidationFixture, UpdateViewConstantsUploadsDirectionalShadowParamsAndBackendConvention)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    RVX::ViewData view;
    view.directionalShadowEnabled = 1;
    view.directionalShadowViewProjection = RVX::Mat4Identity();
    view.directionalShadowViewProjection[1][1] = 2.0f;
    view.directionalShadowViewProjection[1][0] = 0.25f;
    view.directionalShadowViewProjection[1][2] = -0.5f;
    view.directionalShadowViewProjection[3][2] = 0.75f;
    view.directionalShadowDepthBias = 0.0125f;
    view.directionalShadowStrength = 0.6f;
    view.directionalShadowInvMapSize = 1.0f / 512.0f;
    view.directionalShadowFilterRadiusTexels = 2.0f;
    view.directionalShadowNormalBias = 0.03125f;

    FakeDevice dxDevice(RVX::RHIBackendType::DX12);
    RVX::PipelineCache dxCache;
    ASSERT_TRUE(dxCache.Initialize(&dxDevice, FindShaderDirectory().string())) << dxCache.GetLastError();
    dxCache.UpdateViewConstants(view);

    const FakeBuffer* dxViewBuffer = FindCapturedBuffer(dxDevice, "ViewConstantBuffer");
    ASSERT_NE(dxViewBuffer, nullptr);

    RVX::ViewConstants uploaded{};
    std::memcpy(&uploaded, dxViewBuffer->GetStorage().data(), sizeof(uploaded));
    EXPECT_FLOAT_EQ(uploaded.directionalShadowViewProjection[1][0], 0.25f);
    EXPECT_FLOAT_EQ(uploaded.directionalShadowViewProjection[1][1], 2.0f);
    EXPECT_FLOAT_EQ(uploaded.directionalShadowViewProjection[1][2], -0.5f);
    EXPECT_FLOAT_EQ(uploaded.directionalShadowViewProjection[3][2], 0.75f);
    EXPECT_FLOAT_EQ(uploaded.directionalShadowParams.x, 1.0f);
    EXPECT_FLOAT_EQ(uploaded.directionalShadowParams.y, 0.0125f);
    EXPECT_FLOAT_EQ(uploaded.directionalShadowParams.z, 0.6f);
    EXPECT_FLOAT_EQ(uploaded.directionalShadowParams.w, 2.0f / 512.0f);
    EXPECT_FLOAT_EQ(uploaded.directionalShadowReceiverParams.x, 0.03125f);
    EXPECT_FLOAT_EQ(uploaded.directionalShadowReceiverParams.y, 0.0f);

    FakeDevice vkDevice(RVX::RHIBackendType::Vulkan);
    RVX::PipelineCache vkCache;
    ASSERT_TRUE(vkCache.Initialize(&vkDevice, FindShaderDirectory().string())) << vkCache.GetLastError();
    vkCache.UpdateViewConstants(view);

    const FakeBuffer* vkViewBuffer = FindCapturedBuffer(vkDevice, "ViewConstantBuffer");
    ASSERT_NE(vkViewBuffer, nullptr);
    std::memcpy(&uploaded, vkViewBuffer->GetStorage().data(), sizeof(uploaded));
    EXPECT_FLOAT_EQ(uploaded.directionalShadowViewProjection[1][0], -0.25f);
    EXPECT_FLOAT_EQ(uploaded.directionalShadowViewProjection[1][1], -2.0f);
    EXPECT_FLOAT_EQ(uploaded.directionalShadowViewProjection[1][2], 0.5f);
    EXPECT_FLOAT_EQ(uploaded.directionalShadowParams.x, 1.0f);
    EXPECT_FLOAT_EQ(uploaded.directionalShadowParams.w, 2.0f / 512.0f);
    EXPECT_FLOAT_EQ(uploaded.directionalShadowReceiverParams.x, 0.03125f);

    FakeDevice reverseZDevice(RVX::RHIBackendType::DX12);
    RVX::PipelineCache reverseZCache;
    RVX::PipelineCacheConfig config;
    config.reverseZ = true;
    reverseZCache.SetConfig(config);
    ASSERT_TRUE(reverseZCache.Initialize(&reverseZDevice, FindShaderDirectory().string()))
        << reverseZCache.GetLastError();
    reverseZCache.UpdateViewConstants(view);

    const FakeBuffer* reverseZViewBuffer = FindCapturedBuffer(reverseZDevice, "ViewConstantBuffer");
    ASSERT_NE(reverseZViewBuffer, nullptr);
    std::memcpy(&uploaded, reverseZViewBuffer->GetStorage().data(), sizeof(uploaded));
    EXPECT_FLOAT_EQ(uploaded.directionalShadowParams.x, 0.0f);
}

TEST_F(PipelineCacheValidationFixture, DirectionalShadowFrameResourcesReportFallbackReasons)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    FakeDevice device;
    RVX::PipelineCache cache;
    ASSERT_TRUE(cache.Initialize(&device, FindShaderDirectory().string())) << cache.GetLastError();

    RVX::DirectionalShadowFrameBindingResult result =
        cache.UpdateDirectionalShadowFrameResources({});
    EXPECT_FALSE(result.shadowSamplingEnabled);
    EXPECT_EQ(result.fallbackReason, RVX::DirectionalShadowFallbackReason::DisabledNoDirectionalLight);

    RVX::DirectionalShadowFrameResources resources;
    resources.enabled = true;
    result = cache.UpdateDirectionalShadowFrameResources(resources);
    EXPECT_FALSE(result.shadowSamplingEnabled);
    EXPECT_EQ(result.fallbackReason, RVX::DirectionalShadowFallbackReason::MissingShadowSRV);

    RVX::RHITextureDesc textureDesc = RVX::RHITextureDesc::DepthStencil(32, 32, RVX::RHIFormat::D32_FLOAT);
    RVX::RHITextureRef texture = device.CreateTexture(textureDesc);
    ASSERT_NE(texture, nullptr);
    RVX::RHITextureViewDesc viewDesc;
    viewDesc.format = texture->GetFormat();
    viewDesc.dimension = texture->GetDimension();
    viewDesc.subresourceRange = RVX::RHISubresourceRange::All();
    viewDesc.subresourceRange.aspect = RVX::RHITextureAspect::Depth;
    viewDesc.type = RVX::RHITextureViewType::ShaderResource;
    RVX::RHITextureViewRef shadowView = device.CreateTextureView(texture.Get(), viewDesc);
    ASSERT_NE(shadowView, nullptr);

    resources.shadowMapView = shadowView.Get();
    result = cache.UpdateDirectionalShadowFrameResources(resources);
    EXPECT_TRUE(result.shadowSamplingEnabled);
    EXPECT_EQ(result.fallbackReason, RVX::DirectionalShadowFallbackReason::None);

    RVX::PipelineCache reverseZCache;
    RVX::PipelineCacheConfig config;
    config.reverseZ = true;
    reverseZCache.SetConfig(config);
    ASSERT_TRUE(reverseZCache.Initialize(&device, FindShaderDirectory().string()))
        << reverseZCache.GetLastError();
    result = reverseZCache.UpdateDirectionalShadowFrameResources(resources);
    EXPECT_FALSE(result.shadowSamplingEnabled);
    EXPECT_EQ(result.fallbackReason, RVX::DirectionalShadowFallbackReason::ReverseZUnsupported);
}

TEST_F(PipelineCacheValidationFixture, UpdateViewConstantsSanitizesInvalidLightingControls)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    FakeDevice device;
    RVX::PipelineCache cache;
    ASSERT_TRUE(cache.Initialize(&device, FindShaderDirectory().string())) << cache.GetLastError();

    RVX::ViewData view;
    view.directionalLightDirection = RVX::Vec3(0.0f, 0.0f, 0.0f);
    view.directionalLightIntensity = -3.0f;
    view.ambientFloorIntensity = -1.0f;
    cache.UpdateViewConstants(view);

    const FakeBuffer* viewBuffer = FindCapturedBuffer(device, "ViewConstantBuffer");
    ASSERT_NE(viewBuffer, nullptr);

    RVX::ViewConstants uploaded{};
    std::memcpy(&uploaded, viewBuffer->GetStorage().data(), sizeof(uploaded));
    EXPECT_NEAR(uploaded.lightDirection.x, 0.505076f, 0.00001f);
    EXPECT_NEAR(uploaded.lightDirection.y, -0.808122f, 0.00001f);
    EXPECT_NEAR(uploaded.lightDirection.z, 0.303046f, 0.00001f);
    EXPECT_FLOAT_EQ(uploaded.directionalLightIntensity, 0.0f);
    EXPECT_FLOAT_EQ(uploaded.iblTextureParams.w, 0.0f);

    view.directionalShadowEnabled = 1;
    view.directionalShadowDepthBias = -0.25f;
    view.directionalShadowStrength = 5.0f;
    view.directionalShadowInvMapSize = 1.0f / 256.0f;
    view.directionalShadowFilterRadiusTexels = -2.0f;
    view.directionalShadowNormalBias = -3.0f;
    cache.UpdateViewConstants(view);
    std::memcpy(&uploaded, viewBuffer->GetStorage().data(), sizeof(uploaded));
    EXPECT_FLOAT_EQ(uploaded.directionalShadowParams.x, 1.0f);
    EXPECT_FLOAT_EQ(uploaded.directionalShadowParams.y, 0.0f);
    EXPECT_FLOAT_EQ(uploaded.directionalShadowParams.z, 1.0f);
    EXPECT_FLOAT_EQ(uploaded.directionalShadowParams.w, 0.0f);
    EXPECT_FLOAT_EQ(uploaded.directionalShadowReceiverParams.x, 0.0f);

    view.directionalLightDirection = RVX::Vec3(std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f);
    view.directionalLightIntensity = std::numeric_limits<float>::infinity();
    view.ambientFloorIntensity = std::numeric_limits<float>::quiet_NaN();
    view.directionalShadowDepthBias = std::numeric_limits<float>::quiet_NaN();
    view.directionalShadowStrength = std::numeric_limits<float>::quiet_NaN();
    view.directionalShadowInvMapSize = std::numeric_limits<float>::quiet_NaN();
    view.directionalShadowFilterRadiusTexels = std::numeric_limits<float>::infinity();
    view.directionalShadowNormalBias = std::numeric_limits<float>::quiet_NaN();
    cache.UpdateViewConstants(view);
    std::memcpy(&uploaded, viewBuffer->GetStorage().data(), sizeof(uploaded));
    EXPECT_NEAR(uploaded.lightDirection.x, 0.505076f, 0.00001f);
    EXPECT_NEAR(uploaded.lightDirection.y, -0.808122f, 0.00001f);
    EXPECT_NEAR(uploaded.lightDirection.z, 0.303046f, 0.00001f);
    EXPECT_FLOAT_EQ(uploaded.directionalLightIntensity, 4.0f);
    EXPECT_FLOAT_EQ(uploaded.iblTextureParams.w, 0.08f);
    EXPECT_FLOAT_EQ(uploaded.directionalShadowParams.y, 0.005f);
    EXPECT_FLOAT_EQ(uploaded.directionalShadowParams.z, 1.0f);
    EXPECT_FLOAT_EQ(uploaded.directionalShadowParams.w, 0.0f);
    EXPECT_FLOAT_EQ(uploaded.directionalShadowReceiverParams.x, 0.02f);
}

TEST_F(PipelineCacheValidationFixture, DefaultLitUsesIBLAmbientViewConstants)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    const std::string shader = ReadTextFile(FindShaderDirectory() / "DefaultLit.hlsl");
    EXPECT_NE(shader.find("IBLDiffuseAmbient"), std::string::npos);
    EXPECT_NE(shader.find("IBLSpecularAmbient"), std::string::npos);
    EXPECT_NE(shader.find("TextureCube IrradianceTexture : register(t7, space2);"), std::string::npos);
    EXPECT_NE(shader.find("TextureCube PrefilteredEnvironmentTexture : register(t8, space2);"), std::string::npos);
    EXPECT_NE(shader.find("Texture2D BRDFLUTTexture : register(t9, space2);"), std::string::npos);
    EXPECT_NE(shader.find("Texture2D<float> DirectionalShadowMapTexture : register(t1, space0);"),
              std::string::npos);
    EXPECT_NE(shader.find("SamplerState DirectionalShadowSampler : register(s2, space0);"), std::string::npos);
    EXPECT_NE(shader.find("float4 DirectionalShadowReceiverParams;"), std::string::npos);
    EXPECT_NE(shader.find("float SampleDirectionalShadow(float3 worldPos, float3 worldNormal)"), std::string::npos);
    EXPECT_NE(shader.find("float CompareDirectionalShadowDepth(float2 uv, float compareDepth)"), std::string::npos);
    EXPECT_NE(shader.find("float SampleDirectionalShadowPCF(float2 shadowUV, float compareDepth, float filterStep)"),
              std::string::npos);
    EXPECT_NE(shader.find("float DirectionalLightIntensity;"), std::string::npos);
    EXPECT_NE(shader.find("if (IBLTextureParams.x > 0.5)"), std::string::npos);
    EXPECT_NE(shader.find("IBLTextureParams.w"), std::string::npos);
    EXPECT_NE(shader.find("DirectionalShadowParams.w"), std::string::npos);
    EXPECT_NE(shader.find("float normalBias = max(DirectionalShadowReceiverParams.x, 0.0);"), std::string::npos);
    EXPECT_NE(shader.find("receiverNormal * normalBias"), std::string::npos);
    EXPECT_NE(shader.find("for (int y = -1; y <= 1; ++y)"), std::string::npos);
    EXPECT_NE(shader.find("for (int x = -1; x <= 1; ++x)"), std::string::npos);
    EXPECT_NE(shader.find("tapCount > 0.5 ? visibility / tapCount : 1.0"), std::string::npos);
    EXPECT_NE(shader.find("SampleDirectionalShadowPCF(shadowUV, compareDepth, DirectionalShadowParams.w)"),
              std::string::npos);
    EXPECT_NE(shader.find("float shadowVisibility = SampleDirectionalShadow(input.WorldPos, normal);"), std::string::npos);
    EXPECT_NE(shader.find("float3(DirectionalLightIntensity, DirectionalLightIntensity, DirectionalLightIntensity)"),
              std::string::npos);
    EXPECT_EQ(shader.find("float3(DirectionalLightIntensity, DirectionalLightIntensity, DirectionalLightIntensity),\n        1.0"),
              std::string::npos);
    EXPECT_EQ(shader.find("float storedDepth = DirectionalShadowMapTexture.SampleLevel(DirectionalShadowSampler, shadowUV, 0).r"),
              std::string::npos);
    EXPECT_EQ(shader.find("float lit = compareDepth <= storedDepth ? 1.0 : 0.0"), std::string::npos);
    EXPECT_EQ(shader.find("float3(4.0, 4.0, 4.0)"), std::string::npos);
    EXPECT_EQ(shader.find("ambientFloor = baseColor.rgb * 0.08"), std::string::npos);
    EXPECT_EQ(shader.find("ambientDiffuse = baseColor.rgb * (1.0 - fresnel) * (1.0 - metallic) * occlusion * 0.12;"),
              std::string::npos);
    EXPECT_EQ(shader.find("ambientSpecular = f0 * occlusion * (1.0 - clampedRoughness) * 0.04;"),
              std::string::npos);
}

TEST_F(PipelineCacheValidationFixture, SceneRendererClearsAmbientFloorWhenTextureIBLIsReady)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    const fs::path sceneRendererPath = FindShaderDirectory().parent_path() /
        "Private" / "Renderer" / "SceneRenderer.cpp";
    const std::string source = ReadTextFile(sceneRendererPath);
    EXPECT_NE(source.find("m_viewData.ambientFloorIntensity = 0.08f;"), std::string::npos);
    EXPECT_NE(source.find("m_viewData.ambientFloorIntensity = 0.0f;"), std::string::npos);
}

TEST_F(PipelineCacheValidationFixture, SceneRendererUsesSamePrimaryDirectionalLightForDefaultLitAndShadowPass)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    const fs::path sceneRendererPath = FindShaderDirectory().parent_path() /
        "Private" / "Renderer" / "SceneRenderer.cpp";
    const std::string source = ReadTextFile(sceneRendererPath);
    EXPECT_NE(source.find("m_viewData.directionalLightDirection = Vec3{0.5f, -0.8f, 0.3f};"),
              std::string::npos);
    EXPECT_NE(source.find("m_viewData.directionalLightDirection = light.direction;"), std::string::npos);
    EXPECT_NE(source.find("m_viewData.directionalLightIntensity = light.intensity;"), std::string::npos);
    EXPECT_NE(source.find("m_shadowPass->SetDirectionalLight(light.direction, light.color, light.intensity);"),
              std::string::npos);
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
    EXPECT_NE(firstCache.GetStats().toneMappingPipelineHash, 0u);
    EXPECT_NE(firstCache.GetStats().bloomPipelineHash, 0u);
    EXPECT_NE(firstCache.GetStats().colorGradingPipelineHash, 0u);
    EXPECT_NE(firstCache.GetStats().chromaticAberrationPipelineHash, 0u);
    EXPECT_NE(firstCache.GetStats().vignettePipelineHash, 0u);
    EXPECT_NE(firstCache.GetStats().fxaaPipelineHash, 0u);
    EXPECT_NE(firstCache.GetStats().skyboxPipelineHash, 0u);
    EXPECT_NE(firstCache.GetStats().toneMappingPipelineHash, firstCache.GetStats().bloomPipelineHash);
    EXPECT_NE(firstCache.GetStats().colorGradingPipelineHash, firstCache.GetStats().bloomPipelineHash);
    EXPECT_NE(firstCache.GetStats().chromaticAberrationPipelineHash, firstCache.GetStats().bloomPipelineHash);
    EXPECT_NE(firstCache.GetStats().vignettePipelineHash, firstCache.GetStats().bloomPipelineHash);
    EXPECT_NE(firstCache.GetStats().fxaaPipelineHash, firstCache.GetStats().bloomPipelineHash);
    EXPECT_NE(firstCache.GetStats().skyboxPipelineHash, firstCache.GetStats().toneMappingPipelineHash);
    EXPECT_EQ(firstCache.GetStats().pipelineCreateCount, 11u);
    EXPECT_EQ(firstCache.GetStats().pipelineCacheMissCount, 11u);
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
    ASSERT_GE(secondDevice.capturedGraphicsPipelines.size(), 11u);
    EXPECT_EQ(secondDevice.capturedGraphicsPipelines.front().renderTargetFormats[0], RVX::RHIFormat::RGBA16_FLOAT);
    EXPECT_EQ(secondDevice.capturedGraphicsPipelines[4].renderTargetFormats[0], RVX::RHIFormat::RGBA16_FLOAT);
    EXPECT_EQ(secondDevice.capturedGraphicsPipelines[5].renderTargetFormats[0], RVX::RHIFormat::RGBA16_FLOAT);
    EXPECT_EQ(secondDevice.capturedGraphicsPipelines[6].renderTargetFormats[0], RVX::RHIFormat::RGBA16_FLOAT);
    EXPECT_EQ(secondDevice.capturedGraphicsPipelines[7].renderTargetFormats[0], RVX::RHIFormat::RGBA16_FLOAT);
    EXPECT_EQ(secondDevice.capturedGraphicsPipelines[8].renderTargetFormats[0], RVX::RHIFormat::RGBA16_FLOAT);
    EXPECT_EQ(secondDevice.capturedGraphicsPipelines[9].renderTargetFormats[0], RVX::RHIFormat::RGBA16_FLOAT);
    EXPECT_EQ(secondDevice.capturedGraphicsPipelines[10].renderTargetFormats[0], RVX::RHIFormat::RGBA16_FLOAT);
}

TEST_F(PipelineCacheValidationFixture, SplitRenderTargetFormatsRouteSceneBloomAndToneMapping)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    FakeDevice device;
    RVX::PipelineCache cache;
    cache.SetRenderTargetFormats(RVX::RHIFormat::RGBA16_FLOAT,
                                 RVX::RHIFormat::RGBA16_FLOAT,
                                 RVX::RHIFormat::BGRA8_UNORM);
    ASSERT_TRUE(cache.Initialize(&device, FindShaderDirectory().string())) << cache.GetLastError();

    EXPECT_EQ(cache.GetConfig().renderTargetFormat, RVX::RHIFormat::RGBA16_FLOAT);
    EXPECT_EQ(cache.GetConfig().postProcessIntermediateFormat, RVX::RHIFormat::RGBA16_FLOAT);
    EXPECT_EQ(cache.GetConfig().toneMappingOutputFormat, RVX::RHIFormat::BGRA8_UNORM);
    EXPECT_EQ(cache.GetSceneRenderTargetFormat(), RVX::RHIFormat::RGBA16_FLOAT);
    EXPECT_EQ(cache.GetPostProcessIntermediateFormat(), RVX::RHIFormat::RGBA16_FLOAT);
    EXPECT_EQ(cache.GetToneMappingOutputFormat(), RVX::RHIFormat::BGRA8_UNORM);

    ASSERT_GE(device.capturedGraphicsPipelines.size(), 11u);
    EXPECT_EQ(device.capturedGraphicsPipelines[0].renderTargetFormats[0], RVX::RHIFormat::RGBA16_FLOAT);
    EXPECT_EQ(device.capturedGraphicsPipelines[1].renderTargetFormats[0], RVX::RHIFormat::RGBA16_FLOAT);
    EXPECT_EQ(device.capturedGraphicsPipelines[2].renderTargetFormats[0], RVX::RHIFormat::RGBA16_FLOAT);
    EXPECT_EQ(device.capturedGraphicsPipelines[4].renderTargetFormats[0], RVX::RHIFormat::RGBA16_FLOAT);
    EXPECT_EQ(device.capturedGraphicsPipelines[5].renderTargetFormats[0], RVX::RHIFormat::BGRA8_UNORM);
    EXPECT_EQ(device.capturedGraphicsPipelines[6].renderTargetFormats[0], RVX::RHIFormat::RGBA16_FLOAT);
    EXPECT_EQ(device.capturedGraphicsPipelines[7].renderTargetFormats[0], RVX::RHIFormat::BGRA8_UNORM);
    EXPECT_EQ(device.capturedGraphicsPipelines[8].renderTargetFormats[0], RVX::RHIFormat::BGRA8_UNORM);
    EXPECT_EQ(device.capturedGraphicsPipelines[9].renderTargetFormats[0], RVX::RHIFormat::BGRA8_UNORM);
    EXPECT_EQ(device.capturedGraphicsPipelines[10].renderTargetFormats[0], RVX::RHIFormat::BGRA8_UNORM);

    EXPECT_NE(cache.GetPipelineStateHashForVariant(RVX::MaterialPipelineVariant::Opaque), 0u);
    EXPECT_NE(cache.GetStats().skyboxPipelineHash, 0u);
    EXPECT_NE(cache.GetStats().toneMappingPipelineHash, 0u);
    EXPECT_NE(cache.GetStats().bloomPipelineHash, 0u);
    EXPECT_NE(cache.GetStats().colorGradingPipelineHash, 0u);
    EXPECT_NE(cache.GetStats().chromaticAberrationPipelineHash, 0u);
    EXPECT_NE(cache.GetStats().vignettePipelineHash, 0u);
    EXPECT_NE(cache.GetStats().fxaaPipelineHash, 0u);
    EXPECT_NE(cache.GetStats().toneMappingPipelineHash, cache.GetStats().bloomPipelineHash);
    EXPECT_NE(cache.GetStats().colorGradingPipelineHash, cache.GetStats().bloomPipelineHash);
    EXPECT_NE(cache.GetStats().chromaticAberrationPipelineHash, cache.GetStats().bloomPipelineHash);
    EXPECT_NE(cache.GetStats().vignettePipelineHash, cache.GetStats().bloomPipelineHash);
    EXPECT_NE(cache.GetStats().fxaaPipelineHash, cache.GetStats().bloomPipelineHash);
}

TEST_F(PipelineCacheValidationFixture, RuntimeOutputFormatRequestsCreateMatchingPipelines)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    FakeDevice device;
    RVX::PipelineCache cache;
    cache.SetRenderTargetFormats(RVX::RHIFormat::RGBA16_FLOAT,
                                 RVX::RHIFormat::RGBA16_FLOAT,
                                 RVX::RHIFormat::BGRA8_UNORM);
    ASSERT_TRUE(cache.Initialize(&device, FindShaderDirectory().string())) << cache.GetLastError();

    const size_t initialPipelineCount = device.capturedGraphicsPipelines.size();
    ASSERT_NE(cache.GetPipelineForVariant(RVX::MaterialPipelineVariant::Opaque, RVX::RHIFormat::RGBA8_UNORM), nullptr);
    ASSERT_NE(cache.GetSkyboxPipeline(RVX::RHIFormat::RGBA8_UNORM), nullptr);
    ASSERT_NE(cache.GetBloomPipeline(RVX::RHIFormat::RGBA8_UNORM), nullptr);
    ASSERT_NE(cache.GetVignettePipeline(RVX::RHIFormat::RGBA8_UNORM), nullptr);
    ASSERT_NE(cache.GetFXAAPipeline(RVX::RHIFormat::RGBA8_UNORM), nullptr);
    ASSERT_NE(cache.GetColorGradingPipeline(RVX::RHIFormat::RGBA8_UNORM), nullptr);
    ASSERT_NE(cache.GetChromaticAberrationPipeline(RVX::RHIFormat::RGBA8_UNORM), nullptr);

    ASSERT_GE(device.capturedGraphicsPipelines.size(), initialPipelineCount + 7u);
    EXPECT_EQ(device.capturedGraphicsPipelines[initialPipelineCount].renderTargetFormats[0],
              RVX::RHIFormat::RGBA8_UNORM);
    EXPECT_EQ(device.capturedGraphicsPipelines[initialPipelineCount + 1].renderTargetFormats[0],
              RVX::RHIFormat::RGBA8_UNORM);
    EXPECT_EQ(device.capturedGraphicsPipelines[initialPipelineCount + 2].renderTargetFormats[0],
              RVX::RHIFormat::RGBA8_UNORM);
    EXPECT_EQ(device.capturedGraphicsPipelines[initialPipelineCount + 3].renderTargetFormats[0],
              RVX::RHIFormat::RGBA8_UNORM);
    EXPECT_EQ(device.capturedGraphicsPipelines[initialPipelineCount + 4].renderTargetFormats[0],
              RVX::RHIFormat::RGBA8_UNORM);
    EXPECT_EQ(device.capturedGraphicsPipelines[initialPipelineCount + 5].renderTargetFormats[0],
              RVX::RHIFormat::RGBA8_UNORM);
    EXPECT_EQ(device.capturedGraphicsPipelines[initialPipelineCount + 6].renderTargetFormats[0],
              RVX::RHIFormat::RGBA8_UNORM);
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

    ASSERT_GE(device.capturedGraphicsPipelines.size(), 11u);
    const auto& opaqueDesc = device.capturedGraphicsPipelines[0];
    const auto& transparentDesc = device.capturedGraphicsPipelines[2];
    const auto& depthOnlyDesc = device.capturedGraphicsPipelines[3];
    const auto& skyboxDesc = device.capturedGraphicsPipelines[4];
    const auto& toneMappingDesc = device.capturedGraphicsPipelines[5];
    const auto& bloomDesc = device.capturedGraphicsPipelines[6];
    const auto& vignetteDesc = device.capturedGraphicsPipelines[7];
    const auto& fxaaDesc = device.capturedGraphicsPipelines[8];
    const auto& colorGradingDesc = device.capturedGraphicsPipelines[9];
    const auto& chromaticAberrationDesc = device.capturedGraphicsPipelines[10];

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

    ASSERT_NE(cache.GetSkyboxPipeline(), nullptr);
    EXPECT_EQ(skyboxDesc.numRenderTargets, 1u);
    EXPECT_EQ(skyboxDesc.renderTargetFormats[0], RVX::RHIFormat::RGBA8_UNORM);
    EXPECT_EQ(skyboxDesc.depthStencilFormat, RVX::RHIFormat::D32_FLOAT);
    EXPECT_EQ(skyboxDesc.depthStencilState.depthCompareOp, RVX::RHICompareOp::Less);
    EXPECT_TRUE(skyboxDesc.depthStencilState.depthTestEnable);
    EXPECT_FALSE(skyboxDesc.depthStencilState.depthWriteEnable);
    EXPECT_NE(skyboxDesc.vertexShader, nullptr);
    EXPECT_NE(skyboxDesc.pixelShader, nullptr);
    EXPECT_TRUE(skyboxDesc.inputLayout.elements.empty());

    ASSERT_NE(cache.GetToneMappingPipeline(), nullptr);
    EXPECT_EQ(toneMappingDesc.numRenderTargets, 1u);
    EXPECT_EQ(toneMappingDesc.renderTargetFormats[0], RVX::RHIFormat::RGBA8_UNORM);
    EXPECT_EQ(toneMappingDesc.depthStencilFormat, RVX::RHIFormat::Unknown);
    EXPECT_FALSE(toneMappingDesc.depthStencilState.depthTestEnable);
    EXPECT_FALSE(toneMappingDesc.depthStencilState.depthWriteEnable);
    EXPECT_NE(toneMappingDesc.vertexShader, nullptr);
    EXPECT_NE(toneMappingDesc.pixelShader, nullptr);
    EXPECT_TRUE(toneMappingDesc.inputLayout.elements.empty());

    ASSERT_NE(cache.GetBloomPipeline(), nullptr);
    EXPECT_EQ(bloomDesc.numRenderTargets, 1u);
    EXPECT_EQ(bloomDesc.renderTargetFormats[0], RVX::RHIFormat::RGBA8_UNORM);
    EXPECT_EQ(bloomDesc.depthStencilFormat, RVX::RHIFormat::Unknown);
    EXPECT_FALSE(bloomDesc.depthStencilState.depthTestEnable);
    EXPECT_FALSE(bloomDesc.depthStencilState.depthWriteEnable);
    EXPECT_NE(bloomDesc.vertexShader, nullptr);
    EXPECT_NE(bloomDesc.pixelShader, nullptr);
    EXPECT_TRUE(bloomDesc.inputLayout.elements.empty());

    ASSERT_NE(cache.GetVignettePipeline(), nullptr);
    EXPECT_EQ(vignetteDesc.numRenderTargets, 1u);
    EXPECT_EQ(vignetteDesc.renderTargetFormats[0], RVX::RHIFormat::RGBA8_UNORM);
    EXPECT_EQ(vignetteDesc.depthStencilFormat, RVX::RHIFormat::Unknown);
    EXPECT_FALSE(vignetteDesc.depthStencilState.depthTestEnable);
    EXPECT_FALSE(vignetteDesc.depthStencilState.depthWriteEnable);
    EXPECT_NE(vignetteDesc.vertexShader, nullptr);
    EXPECT_NE(vignetteDesc.pixelShader, nullptr);
    EXPECT_TRUE(vignetteDesc.inputLayout.elements.empty());

    ASSERT_NE(cache.GetFXAAPipeline(), nullptr);
    EXPECT_EQ(fxaaDesc.numRenderTargets, 1u);
    EXPECT_EQ(fxaaDesc.renderTargetFormats[0], RVX::RHIFormat::RGBA8_UNORM);
    EXPECT_EQ(fxaaDesc.depthStencilFormat, RVX::RHIFormat::Unknown);
    EXPECT_FALSE(fxaaDesc.depthStencilState.depthTestEnable);
    EXPECT_FALSE(fxaaDesc.depthStencilState.depthWriteEnable);
    EXPECT_NE(fxaaDesc.vertexShader, nullptr);
    EXPECT_NE(fxaaDesc.pixelShader, nullptr);
    EXPECT_TRUE(fxaaDesc.inputLayout.elements.empty());

    ASSERT_NE(cache.GetColorGradingPipeline(), nullptr);
    EXPECT_EQ(colorGradingDesc.numRenderTargets, 1u);
    EXPECT_EQ(colorGradingDesc.renderTargetFormats[0], RVX::RHIFormat::RGBA8_UNORM);
    EXPECT_EQ(colorGradingDesc.depthStencilFormat, RVX::RHIFormat::Unknown);
    EXPECT_FALSE(colorGradingDesc.depthStencilState.depthTestEnable);
    EXPECT_FALSE(colorGradingDesc.depthStencilState.depthWriteEnable);
    EXPECT_NE(colorGradingDesc.vertexShader, nullptr);
    EXPECT_NE(colorGradingDesc.pixelShader, nullptr);
    EXPECT_TRUE(colorGradingDesc.inputLayout.elements.empty());

    ASSERT_NE(cache.GetChromaticAberrationPipeline(), nullptr);
    EXPECT_EQ(chromaticAberrationDesc.numRenderTargets, 1u);
    EXPECT_EQ(chromaticAberrationDesc.renderTargetFormats[0], RVX::RHIFormat::RGBA8_UNORM);
    EXPECT_EQ(chromaticAberrationDesc.depthStencilFormat, RVX::RHIFormat::Unknown);
    EXPECT_FALSE(chromaticAberrationDesc.depthStencilState.depthTestEnable);
    EXPECT_FALSE(chromaticAberrationDesc.depthStencilState.depthWriteEnable);
    EXPECT_NE(chromaticAberrationDesc.vertexShader, nullptr);
    EXPECT_NE(chromaticAberrationDesc.pixelShader, nullptr);
    EXPECT_TRUE(chromaticAberrationDesc.inputLayout.elements.empty());
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

    ASSERT_GE(device.capturedGraphicsPipelines.size(), 11u);
    const auto& opaqueDesc = device.capturedGraphicsPipelines[0];
    const auto& transparentDesc = device.capturedGraphicsPipelines[2];
    const auto& depthOnlyDesc = device.capturedGraphicsPipelines[3];
    const auto& skyboxDesc = device.capturedGraphicsPipelines[4];

    EXPECT_EQ(opaqueDesc.depthStencilState.depthCompareOp, RVX::RHICompareOp::GreaterEqual);
    EXPECT_TRUE(opaqueDesc.depthStencilState.depthWriteEnable);

    EXPECT_EQ(transparentDesc.depthStencilState.depthCompareOp, RVX::RHICompareOp::GreaterEqual);
    EXPECT_FALSE(transparentDesc.depthStencilState.depthWriteEnable);

    EXPECT_EQ(depthOnlyDesc.depthStencilState.depthCompareOp, RVX::RHICompareOp::GreaterEqual);
    EXPECT_TRUE(depthOnlyDesc.depthStencilState.depthWriteEnable);

    EXPECT_EQ(skyboxDesc.depthStencilState.depthCompareOp, RVX::RHICompareOp::GreaterEqual);
    EXPECT_FALSE(skyboxDesc.depthStencilState.depthWriteEnable);
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

    const std::string manifest = ReadTextFile(temp.Path() / RVX::PipelineCache::GetManifestFileName());
    EXPECT_NE(manifest.find("version=10"), std::string::npos);
    EXPECT_NE(manifest.find("skyboxVertexShaderHash="), std::string::npos);
    EXPECT_NE(manifest.find("skyboxPixelShaderHash="), std::string::npos);
    EXPECT_NE(manifest.find("skyboxPipelineHash="), std::string::npos);
    EXPECT_NE(manifest.find("fxaaVertexShaderHash="), std::string::npos);
    EXPECT_NE(manifest.find("fxaaPixelShaderHash="), std::string::npos);
    EXPECT_NE(manifest.find("fxaaPipelineHash="), std::string::npos);
    EXPECT_NE(manifest.find("colorGradingVertexShaderHash="), std::string::npos);
    EXPECT_NE(manifest.find("colorGradingPixelShaderHash="), std::string::npos);
    EXPECT_NE(manifest.find("colorGradingPipelineHash="), std::string::npos);
    EXPECT_NE(manifest.find("chromaticAberrationVertexShaderHash="), std::string::npos);
    EXPECT_NE(manifest.find("chromaticAberrationPixelShaderHash="), std::string::npos);
    EXPECT_NE(manifest.find("chromaticAberrationPipelineHash="), std::string::npos);
    EXPECT_NE(manifest.find("vignetteVertexShaderHash="), std::string::npos);
    EXPECT_NE(manifest.find("vignettePixelShaderHash="), std::string::npos);
    EXPECT_NE(manifest.find("vignettePipelineHash="), std::string::npos);
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

TEST_F(PipelineCacheValidationFixture, ManifestInvalidatesWhenToneMappingOutputFormatChanges)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    TempDirectory temp("rvx_pipeline_manifest_tonemapping_format_stale");

    {
        FakeDevice device;
        RVX::PipelineCache cache;
        cache.SetConfig(ConfigWithManifest(temp.Path()));
        ASSERT_TRUE(cache.Initialize(&device, FindShaderDirectory().string())) << cache.GetLastError();
    }

    RVX::PipelineCacheConfig changedConfig = ConfigWithManifest(temp.Path());
    changedConfig.toneMappingOutputFormat = RVX::RHIFormat::BGRA8_UNORM;

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

TEST_F(PipelineCacheValidationFixture, ManifestInvalidatesWhenBloomPipelineHashChanges)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    TempDirectory temp("rvx_pipeline_manifest_bloom_stale");
    const fs::path manifestPath = temp.Path() / RVX::PipelineCache::GetManifestFileName();
    RVX::uint64 firstBloomHash = 0;

    {
        FakeDevice device;
        RVX::PipelineCache cache;
        cache.SetConfig(ConfigWithManifest(temp.Path()));
        ASSERT_TRUE(cache.Initialize(&device, FindShaderDirectory().string())) << cache.GetLastError();
        firstBloomHash = cache.GetStats().bloomPipelineHash;
        ASSERT_NE(firstBloomHash, 0u);
    }

    const RVX::uint64 staleBloomHash = firstBloomHash == 1u ? 2u : 1u;
    ReplaceManifestFieldValue(manifestPath, "bloomPipelineHash", std::to_string(staleBloomHash));

    FakeDevice device;
    RVX::PipelineCache cache;
    cache.SetConfig(ConfigWithManifest(temp.Path()));
    ASSERT_TRUE(cache.Initialize(&device, FindShaderDirectory().string())) << cache.GetLastError();

    EXPECT_TRUE(cache.GetStats().manifestLoaded);
    EXPECT_FALSE(cache.GetStats().manifestValid);
    EXPECT_TRUE(cache.GetStats().manifestInvalidated);
}

TEST_F(PipelineCacheValidationFixture, ManifestInvalidatesWhenColorGradingPipelineHashChanges)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    TempDirectory temp("rvx_pipeline_manifest_colorgrading_stale");
    const fs::path manifestPath = temp.Path() / RVX::PipelineCache::GetManifestFileName();
    RVX::uint64 firstColorGradingHash = 0;

    {
        FakeDevice device;
        RVX::PipelineCache cache;
        cache.SetConfig(ConfigWithManifest(temp.Path()));
        ASSERT_TRUE(cache.Initialize(&device, FindShaderDirectory().string())) << cache.GetLastError();
        firstColorGradingHash = cache.GetStats().colorGradingPipelineHash;
        ASSERT_NE(firstColorGradingHash, 0u);
    }

    const RVX::uint64 staleColorGradingHash = firstColorGradingHash == 1u ? 2u : 1u;
    ReplaceManifestFieldValue(manifestPath, "colorGradingPipelineHash", std::to_string(staleColorGradingHash));

    FakeDevice device;
    RVX::PipelineCache cache;
    cache.SetConfig(ConfigWithManifest(temp.Path()));
    ASSERT_TRUE(cache.Initialize(&device, FindShaderDirectory().string())) << cache.GetLastError();

    EXPECT_TRUE(cache.GetStats().manifestLoaded);
    EXPECT_FALSE(cache.GetStats().manifestValid);
    EXPECT_TRUE(cache.GetStats().manifestInvalidated);
}

TEST_F(PipelineCacheValidationFixture, ManifestInvalidatesWhenChromaticAberrationPipelineHashChanges)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    TempDirectory temp("rvx_pipeline_manifest_chromatic_aberration_stale");
    const fs::path manifestPath = temp.Path() / RVX::PipelineCache::GetManifestFileName();
    RVX::uint64 firstChromaticAberrationHash = 0;

    {
        FakeDevice device;
        RVX::PipelineCache cache;
        cache.SetConfig(ConfigWithManifest(temp.Path()));
        ASSERT_TRUE(cache.Initialize(&device, FindShaderDirectory().string())) << cache.GetLastError();
        firstChromaticAberrationHash = cache.GetStats().chromaticAberrationPipelineHash;
        ASSERT_NE(firstChromaticAberrationHash, 0u);
    }

    const RVX::uint64 staleChromaticAberrationHash = firstChromaticAberrationHash == 1u ? 2u : 1u;
    ReplaceManifestFieldValue(manifestPath,
                              "chromaticAberrationPipelineHash",
                              std::to_string(staleChromaticAberrationHash));

    FakeDevice device;
    RVX::PipelineCache cache;
    cache.SetConfig(ConfigWithManifest(temp.Path()));
    ASSERT_TRUE(cache.Initialize(&device, FindShaderDirectory().string())) << cache.GetLastError();

    EXPECT_TRUE(cache.GetStats().manifestLoaded);
    EXPECT_FALSE(cache.GetStats().manifestValid);
    EXPECT_TRUE(cache.GetStats().manifestInvalidated);
}

TEST_F(PipelineCacheValidationFixture, ManifestInvalidatesWhenVignettePipelineHashChanges)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    TempDirectory temp("rvx_pipeline_manifest_vignette_stale");
    const fs::path manifestPath = temp.Path() / RVX::PipelineCache::GetManifestFileName();
    RVX::uint64 firstVignetteHash = 0;

    {
        FakeDevice device;
        RVX::PipelineCache cache;
        cache.SetConfig(ConfigWithManifest(temp.Path()));
        ASSERT_TRUE(cache.Initialize(&device, FindShaderDirectory().string())) << cache.GetLastError();
        firstVignetteHash = cache.GetStats().vignettePipelineHash;
        ASSERT_NE(firstVignetteHash, 0u);
    }

    const RVX::uint64 staleVignetteHash = firstVignetteHash == 1u ? 2u : 1u;
    ReplaceManifestFieldValue(manifestPath, "vignettePipelineHash", std::to_string(staleVignetteHash));

    FakeDevice device;
    RVX::PipelineCache cache;
    cache.SetConfig(ConfigWithManifest(temp.Path()));
    ASSERT_TRUE(cache.Initialize(&device, FindShaderDirectory().string())) << cache.GetLastError();

    EXPECT_TRUE(cache.GetStats().manifestLoaded);
    EXPECT_FALSE(cache.GetStats().manifestValid);
    EXPECT_TRUE(cache.GetStats().manifestInvalidated);
}

TEST_F(PipelineCacheValidationFixture, ManifestInvalidatesWhenFXAAPipelineHashChanges)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    TempDirectory temp("rvx_pipeline_manifest_fxaa_stale");
    const fs::path manifestPath = temp.Path() / RVX::PipelineCache::GetManifestFileName();
    RVX::uint64 firstFXAAHash = 0;

    {
        FakeDevice device;
        RVX::PipelineCache cache;
        cache.SetConfig(ConfigWithManifest(temp.Path()));
        ASSERT_TRUE(cache.Initialize(&device, FindShaderDirectory().string())) << cache.GetLastError();
        firstFXAAHash = cache.GetStats().fxaaPipelineHash;
        ASSERT_NE(firstFXAAHash, 0u);
    }

    const RVX::uint64 staleFXAAHash = firstFXAAHash == 1u ? 2u : 1u;
    ReplaceManifestFieldValue(manifestPath, "fxaaPipelineHash", std::to_string(staleFXAAHash));

    FakeDevice device;
    RVX::PipelineCache cache;
    cache.SetConfig(ConfigWithManifest(temp.Path()));
    ASSERT_TRUE(cache.Initialize(&device, FindShaderDirectory().string())) << cache.GetLastError();

    EXPECT_TRUE(cache.GetStats().manifestLoaded);
    EXPECT_FALSE(cache.GetStats().manifestValid);
    EXPECT_TRUE(cache.GetStats().manifestInvalidated);
}

TEST_F(PipelineCacheValidationFixture, ManifestInvalidatesWhenSkyboxPipelineHashChanges)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    TempDirectory temp("rvx_pipeline_manifest_skybox_stale");
    const fs::path manifestPath = temp.Path() / RVX::PipelineCache::GetManifestFileName();
    RVX::uint64 firstSkyboxHash = 0;

    {
        FakeDevice device;
        RVX::PipelineCache cache;
        cache.SetConfig(ConfigWithManifest(temp.Path()));
        ASSERT_TRUE(cache.Initialize(&device, FindShaderDirectory().string())) << cache.GetLastError();
        firstSkyboxHash = cache.GetStats().skyboxPipelineHash;
        ASSERT_NE(firstSkyboxHash, 0u);
    }

    const RVX::uint64 staleSkyboxHash = firstSkyboxHash == 1u ? 2u : 1u;
    ReplaceManifestFieldValue(manifestPath, "skyboxPipelineHash", std::to_string(staleSkyboxHash));

    FakeDevice device;
    RVX::PipelineCache cache;
    cache.SetConfig(ConfigWithManifest(temp.Path()));
    ASSERT_TRUE(cache.Initialize(&device, FindShaderDirectory().string())) << cache.GetLastError();

    EXPECT_TRUE(cache.GetStats().manifestLoaded);
    EXPECT_FALSE(cache.GetStats().manifestValid);
    EXPECT_TRUE(cache.GetStats().manifestInvalidated);
}

TEST_F(PipelineCacheValidationFixture, ManifestInvalidatesWhenSkyboxShaderChanges)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    TempDirectory temp("rvx_pipeline_manifest_skybox_shader");
    const fs::path shaderDir = CopyShaderDirectoryToTemp(temp.Path());
    const fs::path manifestDir = temp.Path() / "Manifest";
    RVX::uint64 firstSkyboxHash = 0;

    {
        FakeDevice device;
        RVX::PipelineCache cache;
        cache.SetConfig(ConfigWithManifest(manifestDir));
        ASSERT_TRUE(cache.Initialize(&device, shaderDir.string()));
        firstSkyboxHash = cache.GetStats().skyboxPipelineHash;
        ASSERT_NE(firstSkyboxHash, 0u);
        EXPECT_FALSE(cache.GetStats().manifestInvalidated);
    }

    const fs::path skyboxShader = shaderDir / "Skybox.hlsl";
    std::string source = ReadTextFile(skyboxShader);
    source += "\n// PipelineCacheValidation skybox hash mutation\n";
    WriteTextFile(skyboxShader, source);

    FakeDevice device;
    RVX::PipelineCache cache;
    cache.SetConfig(ConfigWithManifest(manifestDir));
    ASSERT_TRUE(cache.Initialize(&device, shaderDir.string()));
    EXPECT_TRUE(cache.GetStats().manifestLoaded);
    EXPECT_TRUE(cache.GetStats().manifestInvalidated);
    EXPECT_NE(cache.GetStats().skyboxPipelineHash, 0u);
    EXPECT_NE(cache.GetStats().skyboxPipelineHash, firstSkyboxHash);
}

TEST_F(PipelineCacheValidationFixture, ManifestInvalidatesWhenFXAAShaderChanges)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    TempDirectory temp("rvx_pipeline_manifest_fxaa_shader");
    const fs::path shaderDir = CopyShaderDirectoryToTemp(temp.Path());
    const fs::path manifestDir = temp.Path() / "Manifest";
    RVX::uint64 firstFXAAHash = 0;

    {
        FakeDevice device;
        RVX::PipelineCache cache;
        cache.SetConfig(ConfigWithManifest(manifestDir));
        ASSERT_TRUE(cache.Initialize(&device, shaderDir.string()));
        firstFXAAHash = cache.GetStats().fxaaPipelineHash;
        ASSERT_NE(firstFXAAHash, 0u);
        EXPECT_FALSE(cache.GetStats().manifestInvalidated);
    }

    const fs::path fxaaShader = shaderDir / "PostProcess" / "FXAA.hlsl";
    std::string source = ReadTextFile(fxaaShader);
    source += "\n// PipelineCacheValidation fxaa hash mutation\n";
    WriteTextFile(fxaaShader, source);

    FakeDevice device;
    RVX::PipelineCache cache;
    cache.SetConfig(ConfigWithManifest(manifestDir));
    ASSERT_TRUE(cache.Initialize(&device, shaderDir.string()));
    EXPECT_TRUE(cache.GetStats().manifestLoaded);
    EXPECT_TRUE(cache.GetStats().manifestInvalidated);
    EXPECT_NE(cache.GetStats().fxaaPipelineHash, 0u);
    EXPECT_NE(cache.GetStats().fxaaPipelineHash, firstFXAAHash);
}

TEST_F(PipelineCacheValidationFixture, ManifestInvalidatesWhenColorGradingShaderChanges)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    TempDirectory temp("rvx_pipeline_manifest_colorgrading_shader");
    const fs::path shaderDir = CopyShaderDirectoryToTemp(temp.Path());
    const fs::path manifestDir = temp.Path() / "Manifest";
    RVX::uint64 firstColorGradingHash = 0;

    {
        FakeDevice device;
        RVX::PipelineCache cache;
        cache.SetConfig(ConfigWithManifest(manifestDir));
        ASSERT_TRUE(cache.Initialize(&device, shaderDir.string()));
        firstColorGradingHash = cache.GetStats().colorGradingPipelineHash;
        ASSERT_NE(firstColorGradingHash, 0u);
        EXPECT_FALSE(cache.GetStats().manifestInvalidated);
    }

    const fs::path colorGradingShader = shaderDir / "PostProcess" / "ColorGrading.hlsl";
    std::string source = ReadTextFile(colorGradingShader);
    source += "\n// PipelineCacheValidation color grading hash mutation\n";
    WriteTextFile(colorGradingShader, source);

    FakeDevice device;
    RVX::PipelineCache cache;
    cache.SetConfig(ConfigWithManifest(manifestDir));
    ASSERT_TRUE(cache.Initialize(&device, shaderDir.string()));
    EXPECT_TRUE(cache.GetStats().manifestLoaded);
    EXPECT_TRUE(cache.GetStats().manifestInvalidated);
    EXPECT_NE(cache.GetStats().colorGradingPipelineHash, 0u);
    EXPECT_NE(cache.GetStats().colorGradingPipelineHash, firstColorGradingHash);
}

TEST_F(PipelineCacheValidationFixture, ManifestInvalidatesWhenChromaticAberrationShaderChanges)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    TempDirectory temp("rvx_pipeline_manifest_chromatic_aberration_shader");
    const fs::path shaderDir = CopyShaderDirectoryToTemp(temp.Path());
    const fs::path manifestDir = temp.Path() / "Manifest";
    RVX::uint64 firstChromaticAberrationHash = 0;

    {
        FakeDevice device;
        RVX::PipelineCache cache;
        cache.SetConfig(ConfigWithManifest(manifestDir));
        ASSERT_TRUE(cache.Initialize(&device, shaderDir.string()));
        firstChromaticAberrationHash = cache.GetStats().chromaticAberrationPipelineHash;
        ASSERT_NE(firstChromaticAberrationHash, 0u);
        EXPECT_FALSE(cache.GetStats().manifestInvalidated);
    }

    const fs::path shader = shaderDir / "PostProcess" / "ChromaticAberration.hlsl";
    std::string source = ReadTextFile(shader);
    source += "\n// PipelineCacheValidation chromatic aberration hash mutation\n";
    WriteTextFile(shader, source);

    FakeDevice device;
    RVX::PipelineCache cache;
    cache.SetConfig(ConfigWithManifest(manifestDir));
    ASSERT_TRUE(cache.Initialize(&device, shaderDir.string()));
    EXPECT_TRUE(cache.GetStats().manifestLoaded);
    EXPECT_TRUE(cache.GetStats().manifestInvalidated);
    EXPECT_NE(cache.GetStats().chromaticAberrationPipelineHash, 0u);
    EXPECT_NE(cache.GetStats().chromaticAberrationPipelineHash, firstChromaticAberrationHash);
}

TEST_F(PipelineCacheValidationFixture, ManifestInvalidatesWhenVignetteShaderChanges)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    TempDirectory temp("rvx_pipeline_manifest_vignette_shader");
    const fs::path shaderDir = CopyShaderDirectoryToTemp(temp.Path());
    const fs::path manifestDir = temp.Path() / "Manifest";
    RVX::uint64 firstVignetteHash = 0;

    {
        FakeDevice device;
        RVX::PipelineCache cache;
        cache.SetConfig(ConfigWithManifest(manifestDir));
        ASSERT_TRUE(cache.Initialize(&device, shaderDir.string()));
        firstVignetteHash = cache.GetStats().vignettePipelineHash;
        ASSERT_NE(firstVignetteHash, 0u);
        EXPECT_FALSE(cache.GetStats().manifestInvalidated);
    }

    const fs::path vignetteShader = shaderDir / "PostProcess" / "Vignette.hlsl";
    std::string source = ReadTextFile(vignetteShader);
    source += "\n// PipelineCacheValidation vignette hash mutation\n";
    WriteTextFile(vignetteShader, source);

    FakeDevice device;
    RVX::PipelineCache cache;
    cache.SetConfig(ConfigWithManifest(manifestDir));
    ASSERT_TRUE(cache.Initialize(&device, shaderDir.string()));
    EXPECT_TRUE(cache.GetStats().manifestLoaded);
    EXPECT_TRUE(cache.GetStats().manifestInvalidated);
    EXPECT_NE(cache.GetStats().vignettePipelineHash, 0u);
    EXPECT_NE(cache.GetStats().vignettePipelineHash, firstVignetteHash);
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
    device.failPipelineCreationAtIndex = 6;
    RVX::PipelineCache cache;

    EXPECT_FALSE(cache.Initialize(&device, FindShaderDirectory().string()));
    EXPECT_NE(cache.GetLastError().find("ToneMapping pipeline"), std::string::npos);
}

TEST_F(PipelineCacheValidationFixture, SkyboxPipelineCreationFailureIsVisible)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    FakeDevice device;
    device.failPipelineCreationAtIndex = 5;
    RVX::PipelineCache cache;

    EXPECT_FALSE(cache.Initialize(&device, FindShaderDirectory().string()));
    EXPECT_NE(cache.GetLastError().find("Skybox pipeline"), std::string::npos);
}

TEST_F(PipelineCacheValidationFixture, BloomPipelineCreationFailureIsVisible)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    FakeDevice device;
    device.failPipelineCreationAtIndex = 7;
    RVX::PipelineCache cache;

    EXPECT_FALSE(cache.Initialize(&device, FindShaderDirectory().string()));
    EXPECT_NE(cache.GetLastError().find("Bloom pipeline"), std::string::npos);
}

TEST_F(PipelineCacheValidationFixture, VignettePipelineCreationFailureIsVisible)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    FakeDevice device;
    device.failPipelineCreationAtIndex = 8;
    RVX::PipelineCache cache;

    EXPECT_FALSE(cache.Initialize(&device, FindShaderDirectory().string()));
    EXPECT_NE(cache.GetLastError().find("Vignette pipeline"), std::string::npos);
}

TEST_F(PipelineCacheValidationFixture, FXAAPipelineCreationFailureIsVisible)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    FakeDevice device;
    device.failPipelineCreationAtIndex = 9;
    RVX::PipelineCache cache;

    EXPECT_FALSE(cache.Initialize(&device, FindShaderDirectory().string()));
    EXPECT_NE(cache.GetLastError().find("FXAA pipeline"), std::string::npos);
}

TEST_F(PipelineCacheValidationFixture, ColorGradingPipelineCreationFailureIsVisible)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    FakeDevice device;
    device.failPipelineCreationAtIndex = 10;
    RVX::PipelineCache cache;

    EXPECT_FALSE(cache.Initialize(&device, FindShaderDirectory().string()));
    EXPECT_NE(cache.GetLastError().find("ColorGrading pipeline"), std::string::npos);
}

TEST_F(PipelineCacheValidationFixture, ChromaticAberrationPipelineCreationFailureIsVisible)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    FakeDevice device;
    device.failPipelineCreationAtIndex = 11;
    RVX::PipelineCache cache;

    EXPECT_FALSE(cache.Initialize(&device, FindShaderDirectory().string()));
    EXPECT_NE(cache.GetLastError().find("ChromaticAberration pipeline"), std::string::npos);
}
