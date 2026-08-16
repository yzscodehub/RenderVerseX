#include "Core/Core.h"
#include "Common/RenderRuntimeTestHarness.h"
#include "Render/Graph/ResourceViewCache.h"
#include "Render/Material/MaterialBinder.h"
#include "Render/Material/MaterialClassification.h"
#include "Render/Material/MaterialSystem.h"
#include "Render/Material/MaterialTemplate.h"
#include "Resources/FrameConstantUploadArena.h"
#include "Resources/RenderSubmissionTracker.h"
#include "Resource/Loader/TextureLoader.h"
#include "Resource/Types/MaterialResource.h"
#include "Resource/Types/ShaderResource.h"
#include "Resource/Types/TextureResource.h"
#include "RHI/RHICommandContext.h"
#include "RHI/RHIDevice.h"
#include "RHI/RHIUpload.h"
#include "Geometry/Asset/Material.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

using namespace RVX;

namespace
{
    class FakeBuffer final : public RHIBuffer
    {
    public:
        explicit FakeBuffer(const RHIBufferDesc& desc,
                            bool mapSucceeds = true,
                            bool commitSucceeds = true)
            : m_desc(desc)
            , m_storage(static_cast<size_t>(desc.size))
            , m_mapSucceeds(mapSucceeds)
            , m_commitSucceeds(commitSucceeds)
        {
        }

        uint64 GetSize() const override { return m_desc.size; }
        RHIBufferUsage GetUsage() const override { return m_desc.usage; }
        RHIMemoryType GetMemoryType() const override { return m_desc.memoryType; }
        uint32 GetStride() const override { return m_desc.stride; }
        const std::string& GetDebugName() const { return m_debugName; }

        void* Map() override
        {
            if (!m_mapSucceeds || m_storage.empty() || m_isMapped)
                return nullptr;

            m_mappedStorage = m_storage;
            m_isMapped = true;
            return m_mappedStorage.data();
        }
        void Unmap() override
        {
            if (!m_isMapped)
            {
                return;
            }

            m_storage = std::move(m_mappedStorage);
            m_isMapped = false;
        }

        bool CommitMappedWrite() override
        {
            ++m_commitMappedWriteCount;
            if (!m_isMapped)
            {
                return false;
            }
            if (!m_commitSucceeds)
            {
                m_mappedStorage.clear();
                m_isMapped = false;
                return false;
            }

            m_storage = std::move(m_mappedStorage);
            m_isMapped = false;
            return true;
        }

        const std::vector<uint8>& GetStorage() const { return m_storage; }
        uint32 GetCommitMappedWriteCount() const { return m_commitMappedWriteCount; }
        void SetCommitSucceeds(bool succeeds) { m_commitSucceeds = succeeds; }

    private:
        RHIBufferDesc m_desc;
        std::string m_debugName = m_desc.debugName ? m_desc.debugName : "";
        std::vector<uint8> m_storage;
        std::vector<uint8> m_mappedStorage;
        bool m_mapSucceeds = true;
        bool m_commitSucceeds = true;
        bool m_isMapped = false;
        uint32 m_commitMappedWriteCount = 0;
    };

    class FakeTexture final : public RHITexture
    {
    public:
        explicit FakeTexture(const RHITextureDesc& desc)
            : m_desc(desc)
        {
        }

        uint32 GetWidth() const override { return m_desc.width; }
        uint32 GetHeight() const override { return m_desc.height; }
        uint32 GetDepth() const override { return m_desc.depth; }
        uint32 GetMipLevels() const override { return m_desc.mipLevels; }
        uint32 GetArraySize() const override { return m_desc.arraySize; }
        RHIFormat GetFormat() const override { return m_desc.format; }
        RHITextureUsage GetUsage() const override { return m_desc.usage; }
        RHITextureDimension GetDimension() const override { return m_desc.dimension; }
        RHISampleCount GetSampleCount() const override { return m_desc.sampleCount; }

    private:
        RHITextureDesc m_desc;
    };

    class FakeTextureView final : public RHITextureView
    {
    public:
        FakeTextureView(RHITexture* texture, const RHITextureViewDesc& desc)
            : RHITextureView(RHITextureRef(texture))
            , m_format(desc.format == RHIFormat::Unknown && texture ? texture->GetFormat() : desc.format)
            , m_range(desc.subresourceRange)
        {
        }

        RHIFormat GetFormat() const override { return m_format; }
        const RHISubresourceRange& GetSubresourceRange() const override { return m_range; }

    private:
        RHIFormat m_format = RHIFormat::Unknown;
        RHISubresourceRange m_range;
    };

    class FakeSampler final : public RHISampler
    {
    public:
        explicit FakeSampler(const RHISamplerDesc& desc)
            : m_desc(desc)
        {
        }

        const RHISamplerDesc& GetDesc() const { return m_desc; }

    private:
        RHISamplerDesc m_desc;
    };

    class FakeDescriptorSetLayout final : public RHIDescriptorSetLayout
    {
    public:
        FakeDescriptorSetLayout()
        {
            m_entries.push_back({0, RHIBindingType::DynamicUniformBuffer, RHIShaderStage::All, 1, true});
            m_entries.push_back({1, RHIBindingType::SampledTexture, RHIShaderStage::All, 1, false});
            m_entries.push_back({2, RHIBindingType::SampledTexture, RHIShaderStage::All, 1, false});
            m_entries.push_back({3, RHIBindingType::SampledTexture, RHIShaderStage::All, 1, false});
            m_entries.push_back({4, RHIBindingType::SampledTexture, RHIShaderStage::All, 1, false});
            m_entries.push_back({5, RHIBindingType::SampledTexture, RHIShaderStage::All, 1, false});
            m_entries.push_back({6, RHIBindingType::Sampler, RHIShaderStage::All, 1, false});
            m_entries.push_back({7, RHIBindingType::Sampler, RHIShaderStage::All, 1, false});
            m_entries.push_back({8, RHIBindingType::Sampler, RHIShaderStage::All, 1, false});
            m_entries.push_back({9, RHIBindingType::Sampler, RHIShaderStage::All, 1, false});
            m_entries.push_back({10, RHIBindingType::Sampler, RHIShaderStage::All, 1, false});
            m_entries.push_back({11, RHIBindingType::ShaderResourceBuffer, RHIShaderStage::All, 1, false});
        }

        const std::vector<RHIBindingLayoutEntry>& GetEntries() const override { return m_entries; }

    private:
        std::vector<RHIBindingLayoutEntry> m_entries;
    };

    class FakeDescriptorSet final : public RHIDescriptorSet
    {
    public:
        explicit FakeDescriptorSet(const RHIDescriptorSetDesc& desc)
            : bindings(desc.bindings)
        {
        }

        bool Update(const std::vector<RHIDescriptorBinding>& newBindings) override
        {
            bindings = newBindings;
            return true;
        }

        const RHIDescriptorBinding* FindBinding(uint32 binding) const
        {
            for (const RHIDescriptorBinding& entry : bindings)
            {
                if (entry.binding == binding)
                    return &entry;
            }
            return nullptr;
        }

        std::vector<RHIDescriptorBinding> bindings;
    };

    class FakeStagingBuffer final : public RHIStagingBuffer
    {
    public:
        explicit FakeStagingBuffer(const RHIStagingBufferDesc& desc)
            : m_desc(desc)
        {
            RHIBufferDesc bufferDesc;
            bufferDesc.size = desc.size;
            bufferDesc.usage = RHIBufferUsage::CopySrc;
            bufferDesc.memoryType = RHIMemoryType::Upload;
            bufferDesc.debugName = desc.debugName;
            m_buffer = RHIBufferRef(new FakeBuffer(bufferDesc));
        }

        void* Map(uint64 offset = 0, uint64 size = RVX_WHOLE_SIZE) override
        {
            (void)size;
            auto* fakeBuffer = static_cast<FakeBuffer*>(m_buffer.Get());
            auto* storage = fakeBuffer->GetStorage().data();
            return const_cast<uint8*>(storage + offset);
        }

        void Unmap() override {}
        uint64 GetSize() const override { return m_desc.size; }
        RHIBuffer* GetBuffer() const override { return m_buffer.Get(); }

    private:
        RHIStagingBufferDesc m_desc;
        RHIBufferRef m_buffer;
    };

    class FakeCommandContext final : public RHICommandContext
    {
    public:
        explicit FakeCommandContext(
            RHICommandQueueType queueType = RHICommandQueueType::Graphics)
            : m_queueType(queueType)
        {
        }

        RHICommandQueueType GetQueueType() const override { return m_queueType; }
        void Begin() override { ++beginCount; }
        void End() override { ++endCount; }
        void Reset() override {}
        void BeginEvent(const char*, uint32 = 0) override {}
        void EndEvent() override {}
        void SetMarker(const char*, uint32 = 0) override {}
        void BufferBarrier(const RHIBufferBarrier&) override {}
        void TextureBarrier(const RHITextureBarrier&) override { ++textureBarrierCount; }
        void Barriers(std::span<const RHIBufferBarrier>, std::span<const RHITextureBarrier>) override {}
        void BeginBarrier(const RHIBufferBarrier&) override {}
        void BeginBarrier(const RHITextureBarrier&) override {}
        void EndBarrier(const RHIBufferBarrier&) override {}
        void EndBarrier(const RHITextureBarrier&) override {}
        void BeginRenderPass(const RHIRenderPassDesc&) override {}
        void EndRenderPass() override {}
        void SetPipeline(RHIPipeline*) override {}
        void SetVertexBuffer(uint32, RHIBuffer*, uint64 = 0) override {}
        void SetVertexBuffers(uint32, std::span<RHIBuffer* const>, std::span<const uint64> = {}) override {}
        void SetIndexBuffer(RHIBuffer*, RHIFormat, uint64 = 0) override {}
        void SetDescriptorSet(uint32, RHIDescriptorSet*, std::span<const uint32> = {}) override {}
        void SetPushConstants(const void*, uint32, uint32 = 0) override {}
        void SetViewport(const RHIViewport&) override {}
        void SetViewports(std::span<const RHIViewport>) override {}
        void SetScissor(const RHIRect&) override {}
        void SetScissors(std::span<const RHIRect>) override {}
        void Draw(uint32, uint32 = 1, uint32 = 0, uint32 = 0) override {}
        void DrawIndexed(uint32, uint32 = 1, uint32 = 0, int32 = 0, uint32 = 0) override {}
        void DrawIndirect(RHIBuffer*, uint64, uint32, uint32) override {}
        void DrawIndexedIndirect(RHIBuffer*, uint64, uint32, uint32) override {}
        void Dispatch(uint32, uint32, uint32) override {}
        void DispatchIndirect(RHIBuffer*, uint64) override {}
        void CopyBuffer(RHIBuffer*, RHIBuffer*, uint64, uint64, uint64) override {}
        void CopyTexture(RHITexture*, RHITexture*, const RHITextureCopyDesc& = {}) override {}
        void CopyBufferToTexture(RHIBuffer*, RHITexture*, const RHIBufferTextureCopyDesc&) override
        {
            ++copyBufferToTextureCount;
        }
        void CopyTextureToBuffer(RHITexture*, RHIBuffer*, const RHIBufferTextureCopyDesc&) override {}
        void BeginQuery(RHIQueryPool*, uint32) override {}
        void EndQuery(RHIQueryPool*, uint32) override {}
        void WriteTimestamp(RHIQueryPool*, uint32) override {}
        void ResolveQueries(RHIQueryPool*, uint32, uint32, RHIBuffer*, uint64) override {}
        void ResetQueries(RHIQueryPool*, uint32, uint32) override {}
        void SetStencilReference(uint32) override {}
        void SetBlendConstants(const float[4]) override {}
        void SetDepthBias(float, float, float = 0.0f) override {}
        void SetDepthBounds(float, float) override {}
        void SetStencilReferenceSeparate(uint32, uint32) override {}
        void SetLineWidth(float) override {}
        void SignalFence(RHIFence*, uint64) override {}
        void WaitFence(RHIFence*, uint64) override {}

        uint32 beginCount = 0;
        uint32 endCount = 0;
        uint32 textureBarrierCount = 0;
        uint32 copyBufferToTextureCount = 0;

    private:
        RHICommandQueueType m_queueType = RHICommandQueueType::Graphics;
    };

    class FakeFence final : public RHIFence
    {
    public:
        explicit FakeFence(uint64 initialValue)
            : m_completedValue(initialValue)
        {
        }

        uint64 GetCompletedValue() const override { return m_completedValue; }
        void Signal(uint64 value) override { m_completedValue = value; }
        void SignalOnQueue(uint64 value, RHICommandQueueType) override { m_completedValue = value; }
        void Wait(uint64 value, uint64 = UINT64_MAX) override { m_completedValue = value; }

    private:
        uint64 m_completedValue = 0;
    };

    class FakeDevice final : public IRHIDevice
    {
    public:
        FakeDevice()
        {
            capabilities.backendType = RHIBackendType::DX12;
            capabilities.adapterName = "MaterialSystemValidation";
            capabilities.driverVersion = "1";
            capabilities.supportsComputePipeline = true;
            capabilities.supportsDescriptorSets = true;
            capabilities.supportsDynamicDescriptorOffsets = true;
            capabilities.maxDescriptorSets = 4;
            capabilities.supportsExplicitResourceBarriers = true;
            capabilities.supportsDefaultQueueFenceSignal = true;
            capabilities.supportsExplicitQueueFenceSignal = true;
            capabilities.supportsAsyncCompute = true;
            capabilities.dx12.resourceBindingTier = 2;
            capabilities.queueTopology.completionMode =
                RHIQueueCompletionMode::NativeTimeline;
            capabilities.queueTopology.logicalQueueDomains = {
                GPUQueueDomain::Graphics,
                GPUQueueDomain::Compute,
                GPUQueueDomain::Copy};
            capabilities.queueTopology.activeDomainCount = 3;
        }

        RHIBufferRef CreateBuffer(const RHIBufferDesc& desc) override
        {
            ++createdBufferCount;
            if (failBufferCreation)
                return nullptr;

            auto buffer = RHIBufferRef(new FakeBuffer(
                desc, bufferMapSucceeds, bufferCommitSucceeds));
            lastCreatedBuffer = static_cast<FakeBuffer*>(buffer.Get());
            retainedBuffers.push_back(buffer);
            return buffer;
        }

        RHITextureRef CreateTexture(const RHITextureDesc& desc) override
        {
            ++createdTextureCount;
            return RHITextureRef(new FakeTexture(desc));
        }

        RHITextureViewRef CreateTextureView(RHITexture* texture, const RHITextureViewDesc& desc = {}) override
        {
            ++createdTextureViewCount;
            return RHITextureViewRef(new FakeTextureView(texture, desc));
        }

        RHISamplerRef CreateSampler(const RHISamplerDesc& desc) override
        {
            ++createdSamplerCount;
            createdSamplerDescs.push_back(desc);
            return RHISamplerRef(new FakeSampler(desc));
        }

        RHIShaderRef CreateShader(const RHIShaderDesc&) override { return nullptr; }
        RHIHeapRef CreateHeap(const RHIHeapDesc&) override { return nullptr; }
        RHITextureRef CreatePlacedTexture(RHIHeap*, uint64, const RHITextureDesc&) override { return nullptr; }
        RHIBufferRef CreatePlacedBuffer(RHIHeap*, uint64, const RHIBufferDesc&) override { return nullptr; }
        MemoryRequirements GetTextureMemoryRequirements(const RHITextureDesc&) override { return {}; }
        MemoryRequirements GetBufferMemoryRequirements(const RHIBufferDesc&) override { return {}; }
        RHIDescriptorSetLayoutRef CreateDescriptorSetLayout(const RHIDescriptorSetLayoutDesc&) override { return nullptr; }
        RHIPipelineLayoutRef CreatePipelineLayout(const RHIPipelineLayoutDesc&) override { return nullptr; }
        RHIPipelineRef CreateGraphicsPipeline(const RHIGraphicsPipelineDesc&) override { return nullptr; }
        RHIPipelineRef CreateComputePipeline(const RHIComputePipelineDesc&) override { return nullptr; }

        RHIDescriptorSetRef CreateDescriptorSet(const RHIDescriptorSetDesc& desc) override
        {
            ++createdDescriptorSetCount;
            if (failDescriptorSetCreation)
                return nullptr;

            return RHIDescriptorSetRef(new FakeDescriptorSet(desc));
        }

        RHIQueryPoolRef CreateQueryPool(const RHIQueryPoolDesc&) override { return nullptr; }

        RHICommandContextRef CreateCommandContext(RHICommandQueueType type) override
        {
            ++createdCommandContextCount;
            lastCommandQueueType = type;
            auto context = RHICommandContextRef(new FakeCommandContext(type));
            lastCommandContext = static_cast<FakeCommandContext*>(context.Get());
            return context;
        }

        uint64 SubmitCommandContext(RHICommandContext*, RHIFence* signalFence = nullptr) override
        {
            ++submittedCommandContextCount;
            if (!signalFence)
                return 0;

            const uint64 value = m_nextFenceValue++;
            if (!deferFenceCompletion)
            {
                signalFence->Signal(value);
            }
            return value;
        }

        uint64 SubmitCommandContexts(std::span<RHICommandContext* const>, RHIFence* signalFence = nullptr) override
        {
            return SubmitCommandContext(nullptr, signalFence);
        }

        RHISwapChainRef CreateSwapChain(const RHISwapChainDesc&) override { return nullptr; }

        RHIFenceRef CreateFence(uint64 initialValue = 0) override
        {
            ++createdFenceCount;
            retainedFences.push_back(RHIFenceRef(new FakeFence(initialValue)));
            return retainedFences.back();
        }

        void WaitForFence(RHIFence* fence, uint64 value) override
        {
            if (fence)
                fence->Wait(value);
        }

        void WaitIdle() override
        {
            ++waitIdleCount;
        }

        void BeginFrame() override {}
        void EndFrame() override {}
        uint32 GetCurrentFrameIndex() const override { return 0; }

        RHIStagingBufferRef CreateStagingBuffer(const RHIStagingBufferDesc& desc) override
        {
            ++createdStagingBufferCount;
            return RHIStagingBufferRef(new FakeStagingBuffer(desc));
        }

        RHIRingBufferRef CreateRingBuffer(const RHIRingBufferDesc&) override { return nullptr; }
        RHIMemoryStats GetMemoryStats() const override { return {}; }
        void BeginResourceGroup(const char*) override {}
        void EndResourceGroup() override {}
        const RHICapabilities& GetCapabilities() const override { return capabilities; }
        RHIBackendType GetBackendType() const override { return backendType; }

        FakeBuffer* FindBuffer(const std::string& debugName) const
        {
            for (const RHIBufferRef& buffer : retainedBuffers)
            {
                auto* fakeBuffer = static_cast<FakeBuffer*>(buffer.Get());
                if (fakeBuffer && fakeBuffer->GetDebugName() == debugName)
                    return fakeBuffer;
            }
            return nullptr;
        }

        uint32 createdBufferCount = 0;
        uint32 createdTextureCount = 0;
        uint32 createdTextureViewCount = 0;
        uint32 createdSamplerCount = 0;
        uint32 createdDescriptorSetCount = 0;
        uint32 createdCommandContextCount = 0;
        uint32 submittedCommandContextCount = 0;
        uint32 createdStagingBufferCount = 0;
        uint32 createdFenceCount = 0;
        uint32 waitIdleCount = 0;
        bool failBufferCreation = false;
        bool failDescriptorSetCreation = false;
        bool bufferMapSucceeds = true;
        bool bufferCommitSucceeds = true;
        bool deferFenceCompletion = false;
        RHIBackendType backendType = RHIBackendType::DX12;
        RHICommandQueueType lastCommandQueueType = RHICommandQueueType::Graphics;
        FakeCommandContext* lastCommandContext = nullptr;
        FakeBuffer* lastCreatedBuffer = nullptr;
        std::vector<RHIBufferRef> retainedBuffers;
        std::vector<RHISamplerDesc> createdSamplerDescs;
        std::vector<RHIFenceRef> retainedFences;
        RHICapabilities capabilities;

    private:
        uint64 m_nextFenceValue = 1;
    };

    class LogEnvironment final : public ::testing::Environment
    {
    public:
        void SetUp() override
        {
            Log::Initialize();
        }

        void TearDown() override
        {
            Log::Shutdown();
        }
    };

    [[maybe_unused]] ::testing::Environment* const g_logEnvironment =
        ::testing::AddGlobalTestEnvironment(new LogEnvironment());

    Resource::TextureHandle CreateTextureResource(Resource::ResourceId id,
                                                  Resource::TextureFormat format = Resource::TextureFormat::RGBA8,
                                                  std::vector<uint8> pixels = {255, 255, 255, 255})
    {
        auto* texture = new Resource::TextureResource();
        texture->SetId(id);
        texture->SetName("MaterialSystemTexture");

        Resource::TextureMetadata metadata;
        metadata.width = 1;
        metadata.height = 1;
        metadata.format = format;
        metadata.isSRGB = false;

        texture->SetData(std::move(pixels), metadata);
        return Resource::TextureHandle(texture);
    }

    Resource::TextureHandle CreateIBLCubemapResource(Resource::ResourceId id, uint32 mipLevels = 1)
    {
        Resource::TextureMetadata metadata;
        metadata.width = 1;
        metadata.height = 1;
        metadata.mipLevels = std::max(1u, mipLevels);
        metadata.arrayLayers = 6;
        metadata.format = Resource::TextureFormat::RGBA8;
        metadata.isCubemap = true;
        metadata.isSRGB = false;

        const size_t dataSize = static_cast<size_t>(metadata.arrayLayers) *
                                static_cast<size_t>(metadata.mipLevels) *
                                4u;
        std::vector<uint8> pixels(dataSize, 0);
        auto* texture = new Resource::TextureResource();
        texture->SetId(id);
        texture->SetName("MaterialSystemIBLCubemap");
        texture->SetData(std::move(pixels), metadata);
        return Resource::TextureHandle(texture);
    }

    Resource::TextureHandle CreateIBLBRDFLUTResource(Resource::ResourceId id)
    {
        return CreateTextureResource(id, Resource::TextureFormat::RGBA8, {0, 0, 0, 255});
    }

    class TestShaderResource final : public Resource::ShaderResource
    {
    public:
        void MarkLoaded()
        {
            SetState(Resource::ResourceState::Loaded);
        }
    };

    Resource::ShaderHandle CreateShaderResource(Resource::ResourceId id,
                                                bool validContract = true)
    {
        auto* shader = new TestShaderResource();
        shader->SetId(id);
        shader->SetName("MaterialSystemShader");

        Resource::ShaderMetadata metadata;
        metadata.sourcePath = validContract ? "Shaders/Material.ps.hlsl" : "";
        metadata.backend = Resource::ShaderBackendType::DX12;
        metadata.stage = Resource::ShaderStage::Pixel;
        metadata.entryPoint = "main";
        metadata.targetProfile = "ps_6_0";
        metadata.sourceHash = 0x12345678ull;
        metadata.reflectionResourceCount = 4;

        shader->SetData({0x44, 0x58, 0x42, 0x43}, "", "", metadata);
        shader->MarkLoaded();
        return Resource::ShaderHandle(shader);
    }

    std::string ReadTextFile(const std::filesystem::path& path)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file.is_open())
        {
            return {};
        }

        return std::string(std::istreambuf_iterator<char>(file),
                           std::istreambuf_iterator<char>());
    }

    const char* ToCookedTextureFormatString(Resource::TextureFormat format)
    {
        switch (format)
        {
            case Resource::TextureFormat::BC1: return "BC1";
            case Resource::TextureFormat::BC3: return "BC3";
            case Resource::TextureFormat::BC5: return "BC5";
            default:                           return "RGBA8";
        }
    }

    const char* ToCookedTextureUsageString(Resource::TextureUsage usage)
    {
        switch (usage)
        {
            case Resource::TextureUsage::Normal: return "Normal";
            case Resource::TextureUsage::Data:   return "Data";
            case Resource::TextureUsage::Color:
            default:                             return "Color";
        }
    }

    std::filesystem::path WriteCookedTextureArtifact(const std::string& name,
                                                     Resource::TextureFormat format,
                                                     Resource::TextureUsage usage,
                                                     bool isSRGB,
                                                     const std::vector<uint8>& payload)
    {
        const std::filesystem::path path =
            std::filesystem::temp_directory_path() / (name + ".rva");

        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << "RVX_TEXTURE_PREBAKE_V1\n";
        file << "width=4\n";
        file << "height=4\n";
        file << "depth=1\n";
        file << "mipLevels=1\n";
        file << "arrayLayers=1\n";
        file << "format=" << ToCookedTextureFormatString(format) << "\n";
        file << "usage=" << ToCookedTextureUsageString(usage) << "\n";
        file << "srgb=" << (isSRGB ? 1 : 0) << "\n";
        file << "dataSize=" << payload.size() << "\n";
        file << "compression=" << ToCookedTextureFormatString(format) << "\n";
        file << "RVX_TEXTURE_DATA_BEGIN\n";
        file.write(reinterpret_cast<const char*>(payload.data()),
                   static_cast<std::streamsize>(payload.size()));
        file << "\nRVX_TEXTURE_PREBAKE_END\n";

        return path;
    }

    class ScopedTempFiles final
    {
    public:
        std::filesystem::path Track(std::filesystem::path path)
        {
            m_paths.push_back(path);
            return path;
        }

        ~ScopedTempFiles()
        {
            for (const std::filesystem::path& path : m_paths)
            {
                std::error_code error;
                std::filesystem::remove(path, error);
            }
        }

    private:
        std::vector<std::filesystem::path> m_paths;
    };

    void ConfigureMaterialWithAlbedo(Resource::MaterialResource& materialResource,
                                     const Resource::TextureHandle& albedo)
    {
        materialResource.SetId(201);
        materialResource.SetName("TestMaterialResource");
        materialResource.SetMaterialData(std::make_shared<Material>());
        materialResource.SetTexture("albedo", albedo);
    }

    void ConfigureMaterialWithAllTextures(Resource::MaterialResource& materialResource,
                                          const Resource::TextureHandle& texture)
    {
        materialResource.SetId(202);
        materialResource.SetName("FullyTexturedMaterialResource");
        materialResource.SetMaterialData(std::make_shared<Material>("FullyTexturedMaterialResource"));
        materialResource.SetTexture("albedo", texture);
        materialResource.SetTexture("normal", texture);
        materialResource.SetTexture("metallic_roughness", texture);
        materialResource.SetTexture("ao", texture);
        materialResource.SetTexture("emissive", texture);
    }

    const RHIDescriptorBinding* FindBinding(const RHIDescriptorSet* descriptorSet, uint32 binding)
    {
        const auto* fakeSet = static_cast<const FakeDescriptorSet*>(descriptorSet);
        return fakeSet ? fakeSet->FindBinding(binding) : nullptr;
    }

    bool Contains(const std::string& text, const char* expected)
    {
        return text.find(expected) != std::string::npos;
    }

    MaterialGPUConstants ReadMaterialConstants(const FakeBuffer& buffer)
    {
        MaterialGPUConstants constants;
        const std::vector<uint8>& storage = buffer.GetStorage();
        EXPECT_GE(storage.size(), sizeof(MaterialGPUConstants));
        std::memcpy(&constants, storage.data(), sizeof(MaterialGPUConstants));
        return constants;
    }

    MaterialSourceData MakeMaterialSourceData(const Material& material)
    {
        MaterialSourceData source;
        source.baseColorFactor = material.GetBaseColor();
        source.metallicFactor = material.GetMetallicFactor();
        source.roughnessFactor = material.GetRoughnessFactor();
        source.normalScale = material.GetNormalScale();
        source.occlusionStrength = material.GetOcclusionStrength();
        source.emissiveColor = material.GetEmissiveColor();
        source.emissiveStrength = material.GetEmissiveStrength();
        source.alphaCutoff = material.GetAlphaCutoff();
        source.doubleSided = material.IsDoubleSided();

        if (material.GetBaseColorTexture())
            source.textureFlags |= static_cast<uint32>(MaterialTextureFlags::HasBaseColor);
        if (material.GetNormalTexture())
            source.textureFlags |= static_cast<uint32>(MaterialTextureFlags::HasNormal);
        if (material.GetMetallicRoughnessTexture())
            source.textureFlags |= static_cast<uint32>(MaterialTextureFlags::HasMetallicRoughness);
        if (material.GetOcclusionTexture())
            source.textureFlags |= static_cast<uint32>(MaterialTextureFlags::HasOcclusion);
        if (material.GetEmissiveTexture())
            source.textureFlags |= static_cast<uint32>(MaterialTextureFlags::HasEmissive);

        switch (material.GetAlphaMode())
        {
        case Material::AlphaMode::Opaque:
            source.alphaMode = MaterialSourceAlphaMode::Opaque;
            break;
        case Material::AlphaMode::Mask:
            source.alphaMode = MaterialSourceAlphaMode::Mask;
            break;
        case Material::AlphaMode::Blend:
            source.alphaMode = MaterialSourceAlphaMode::Blend;
            break;
        }

        switch (material.GetWorkflow())
        {
        case MaterialWorkflow::MetallicRoughness:
            source.workflow = MaterialSourceWorkflow::MetallicRoughness;
            break;
        case MaterialWorkflow::SpecularGlossiness:
            source.workflow = MaterialSourceWorkflow::SpecularGlossiness;
            break;
        case MaterialWorkflow::Unlit:
            source.workflow = MaterialSourceWorkflow::Unlit;
            break;
        }

        return source;
    }

    TEST(FrameConstantUploadArenaValidation,
         Slots8191And8192UseDifferentPagesAndPreserveRecordedBytes)
    {
        FakeDevice device;
        FrameConstantUploadArena arena;
        ASSERT_TRUE(arena.Initialize(&device, 256, 8192, "FrameConstantArenaTest"));

        FrameConstantUploadAllocation slot8191;
        FrameConstantUploadAllocation slot8192;
        for (uint32 index = 0; index != 8193; ++index)
        {
            const uint32 marker = 0xA0000000u + index;
            FrameConstantUploadAllocation allocation;
            ASSERT_TRUE(arena.Allocate(&marker, sizeof(marker), allocation));
            if (index == 8191)
            {
                slot8191 = allocation;
            }
            else if (index == 8192)
            {
                slot8192 = allocation;
            }
        }

        ASSERT_TRUE(slot8191.IsValid());
        ASSERT_TRUE(slot8192.IsValid());
        EXPECT_NE(slot8191.pageIdentity, slot8192.pageIdentity);
        EXPECT_GT(slot8191.dynamicOffset, 0u);
        EXPECT_EQ(0u, slot8192.dynamicOffset);
        const auto* firstPage = static_cast<const FakeBuffer*>(slot8191.buffer.Get());
        ASSERT_NE(nullptr, firstPage);
        const std::vector<uint8>& firstPageBytes = firstPage->GetStorage();
        uint32 retainedMarker = 0;
        std::memcpy(&retainedMarker,
                    firstPageBytes.data() + slot8191.dynamicOffset,
                    sizeof(retainedMarker));
        EXPECT_EQ(0xA0001FFFu, retainedMarker);
    }

    TEST(FrameConstantUploadArenaValidation,
         CompletionTrackedPagesRejectUnissuedAndStaleTokensAndReuseOnlyAfterCompletion)
    {
        FakeDevice device;
        device.deferFenceCompletion = true;
        RenderSubmissionTracker tracker;
        ASSERT_TRUE(tracker.Initialize(&device));

        FrameConstantUploadArena arena;
        ASSERT_TRUE(arena.Initialize(&device, 256, 2, "FrameConstantArenaCompletion"));
        arena.SetSubmissionTracker(&tracker);
        const uint32 value = 1;
        FrameConstantUploadAllocation first;
        FrameConstantUploadAllocation firstPeer;
        ASSERT_TRUE(arena.Allocate(&value, sizeof(value), first));
        ASSERT_TRUE(arena.Allocate(&value, sizeof(value), firstPeer));
        EXPECT_EQ(first.pageIdentity, firstPeer.pageIdentity);
        EXPECT_NE(first.dynamicOffset, firstPeer.dynamicOffset);

        FakeCommandContext submittedContext;
        const GPUCompletionPoint firstSubmission = tracker.Submit(&submittedContext);
        ASSERT_EQ(GPUQueueDomain::Graphics, firstSubmission.domain);
        ASSERT_NE(0u, firstSubmission.value);
        GPUCompletionToken completion;
        ASSERT_TRUE(InsertGPUCompletionPoint(completion, firstSubmission));
        ASSERT_TRUE(arena.NotifySubmission(completion));

        FrameConstantUploadAllocation pending;
        ASSERT_TRUE(arena.Allocate(&value, sizeof(value), pending));
        EXPECT_NE(first.pageIdentity, pending.pageIdentity)
            << "pending completion must not reuse the submitted page";

        ASSERT_FALSE(device.retainedFences.empty());
        static_cast<FakeFence*>(device.retainedFences.front().Get())->Signal(
            firstSubmission.value);
        arena.PollCompletions();
        FrameConstantUploadAllocation completed;
        ASSERT_TRUE(arena.Allocate(&value, sizeof(value), completed));
        EXPECT_EQ(first.pageIdentity, completed.pageIdentity);
        EXPECT_EQ(0u, completed.dynamicOffset);

        FrameConstantUploadArena rollbackArena;
        ASSERT_TRUE(rollbackArena.Initialize(&device, 256, 2, "FrameConstantArenaRollback"));
        rollbackArena.SetSubmissionTracker(&tracker);
        FrameConstantUploadAllocation rejected;
        FrameConstantUploadAllocation rejectedPeer;
        ASSERT_TRUE(rollbackArena.Allocate(&value, sizeof(value), rejected));
        ASSERT_TRUE(rollbackArena.Allocate(&value, sizeof(value), rejectedPeer));
        EXPECT_EQ(rejected.pageIdentity, rejectedPeer.pageIdentity);
        rollbackArena.ReleaseUnsubmittedFrame();
        FrameConstantUploadAllocation retry;
        ASSERT_TRUE(rollbackArena.Allocate(&value, sizeof(value), retry));
        EXPECT_EQ(rejected.pageIdentity, retry.pageIdentity);
        EXPECT_EQ(0u, retry.dynamicOffset);

        FrameConstantUploadArena missingTrackerArena;
        ASSERT_TRUE(missingTrackerArena.Initialize(
            &device, 256, 2, "FrameConstantArenaMissingTracker"));
        FrameConstantUploadAllocation missingTracker;
        ASSERT_TRUE(missingTrackerArena.Allocate(
            &value, sizeof(value), missingTracker));
        EXPECT_FALSE(missingTrackerArena.NotifySubmission(completion));
        EXPECT_EQ(1u, missingTrackerArena.GetUnusablePageCount());

        FrameConstantUploadArena unissuedArena;
        ASSERT_TRUE(unissuedArena.Initialize(&device, 256, 2, "FrameConstantArenaUnissued"));
        unissuedArena.SetSubmissionTracker(&tracker);
        FrameConstantUploadAllocation unissued;
        ASSERT_TRUE(unissuedArena.Allocate(&value, sizeof(value), unissued));
        GPUCompletionToken unissuedCompletion;
        ASSERT_TRUE(InsertGPUCompletionPoint(
            unissuedCompletion,
            {GPUQueueDomain::Graphics, firstSubmission.value + 1u}));
        EXPECT_FALSE(unissuedArena.NotifySubmission(unissuedCompletion));
        EXPECT_EQ(1u, unissuedArena.GetUnusablePageCount());
        FrameConstantUploadAllocation afterInvalid;
        ASSERT_TRUE(unissuedArena.Allocate(&value, sizeof(value), afterInvalid));
        EXPECT_NE(unissued.pageIdentity, afterInvalid.pageIdentity);

        const GPUCompletionPoint secondSubmission = tracker.Submit(&submittedContext);
        ASSERT_EQ(GPUQueueDomain::Graphics, secondSubmission.domain);
        ASSERT_GT(secondSubmission.value, firstSubmission.value);
        FrameConstantUploadArena staleArena;
        ASSERT_TRUE(staleArena.Initialize(&device, 256, 2, "FrameConstantArenaStale"));
        staleArena.SetSubmissionTracker(&tracker);
        FrameConstantUploadAllocation stale;
        ASSERT_TRUE(staleArena.Allocate(&value, sizeof(value), stale));
        EXPECT_FALSE(staleArena.NotifySubmission(completion));
        EXPECT_EQ(1u, staleArena.GetUnusablePageCount());
        FrameConstantUploadAllocation afterStale;
        ASSERT_TRUE(staleArena.Allocate(&value, sizeof(value), afterStale));
        EXPECT_NE(stale.pageIdentity, afterStale.pageIdentity);

        FakeCommandContext computeContext(RHICommandQueueType::Compute);
        const GPUCompletionPoint computeSubmission = tracker.Submit(&computeContext);
        ASSERT_EQ(GPUQueueDomain::Compute, computeSubmission.domain);
        ASSERT_NE(0u, computeSubmission.value);
        FrameConstantUploadArena extraDomainArena;
        ASSERT_TRUE(extraDomainArena.Initialize(
            &device, 256, 2, "FrameConstantArenaExtraDomain"));
        extraDomainArena.SetSubmissionTracker(&tracker);
        FrameConstantUploadAllocation extraDomain;
        ASSERT_TRUE(extraDomainArena.Allocate(&value, sizeof(value), extraDomain));
        GPUCompletionToken extraDomainCompletion;
        ASSERT_TRUE(InsertGPUCompletionPoint(
            extraDomainCompletion, secondSubmission));
        ASSERT_TRUE(InsertGPUCompletionPoint(
            extraDomainCompletion, computeSubmission));
        EXPECT_FALSE(extraDomainArena.NotifySubmission(extraDomainCompletion));
        EXPECT_EQ(1u, extraDomainArena.GetUnusablePageCount());

        FakeDevice lostBeforeNotifyDevice;
        lostBeforeNotifyDevice.deferFenceCompletion = true;
        RenderSubmissionTracker lostBeforeNotifyTracker;
        ASSERT_TRUE(lostBeforeNotifyTracker.Initialize(&lostBeforeNotifyDevice));
        const GPUCompletionPoint lostBeforeNotifySubmission =
            lostBeforeNotifyTracker.Submit(&submittedContext);
        ASSERT_EQ(GPUQueueDomain::Graphics, lostBeforeNotifySubmission.domain);
        ASSERT_NE(0u, lostBeforeNotifySubmission.value);
        FrameConstantUploadArena lostBeforeNotifyArena;
        ASSERT_TRUE(lostBeforeNotifyArena.Initialize(
            &lostBeforeNotifyDevice, 256, 2, "FrameConstantArenaLostBeforeNotify"));
        lostBeforeNotifyArena.SetSubmissionTracker(&lostBeforeNotifyTracker);
        FrameConstantUploadAllocation lostBeforeNotify;
        ASSERT_TRUE(lostBeforeNotifyArena.Allocate(
            &value, sizeof(value), lostBeforeNotify));
        GPUCompletionToken lostBeforeNotifyCompletion;
        ASSERT_TRUE(InsertGPUCompletionPoint(
            lostBeforeNotifyCompletion, lostBeforeNotifySubmission));
        lostBeforeNotifyTracker.MarkDeviceLost();
        EXPECT_FALSE(lostBeforeNotifyArena.NotifySubmission(
            lostBeforeNotifyCompletion));
        EXPECT_EQ(1u, lostBeforeNotifyArena.GetUnusablePageCount());

        FrameConstantUploadArena lostArena;
        ASSERT_TRUE(lostArena.Initialize(&device, 256, 2, "FrameConstantArenaLost"));
        lostArena.SetSubmissionTracker(&tracker);
        FrameConstantUploadAllocation lost;
        ASSERT_TRUE(lostArena.Allocate(&value, sizeof(value), lost));
        GPUCompletionToken currentCompletion;
        ASSERT_TRUE(InsertGPUCompletionPoint(currentCompletion, secondSubmission));
        ASSERT_TRUE(lostArena.NotifySubmission(currentCompletion));
        tracker.MarkDeviceLost();
        lostArena.PollCompletions();
        FrameConstantUploadAllocation afterLost;
        ASSERT_TRUE(lostArena.Allocate(&value, sizeof(value), afterLost));
        EXPECT_NE(lost.pageIdentity, afterLost.pageIdentity);
        EXPECT_EQ(1u, lostArena.GetUnusablePageCount());
    }

    TEST(FrameConstantUploadArenaValidation,
         CompatibilityWaitIdleCompletionEvidenceRemainsUsable)
    {
        FakeDevice device;
        device.backendType = RHIBackendType::DX11;
        device.capabilities.backendType = RHIBackendType::DX11;
        device.capabilities.supportsAsyncCompute = false;
        device.capabilities.emulatesQueueFences = true;
        device.capabilities.queueTopology.completionMode =
            RHIQueueCompletionMode::CompatibilityWaitIdle;
        device.capabilities.queueTopology.logicalQueueDomains = {
            GPUQueueDomain::Graphics,
            GPUQueueDomain::Graphics,
            GPUQueueDomain::Graphics};
        device.capabilities.queueTopology.activeDomainCount = 1;

        RenderSubmissionTracker tracker;
        ASSERT_TRUE(tracker.Initialize(&device));
        FrameConstantUploadArena arena;
        ASSERT_TRUE(arena.Initialize(
            &device, 256, 2, "FrameConstantArenaCompatibility"));
        arena.SetSubmissionTracker(&tracker);
        const uint32 value = 1;
        FrameConstantUploadAllocation first;
        ASSERT_TRUE(arena.Allocate(&value, sizeof(value), first));

        FakeCommandContext submittedContext;
        const GPUCompletionPoint submission = tracker.Submit(&submittedContext);
        ASSERT_EQ(GPUQueueDomain::Graphics, submission.domain);
        ASSERT_NE(0u, submission.value);
        GPUCompletionToken completion;
        ASSERT_TRUE(InsertGPUCompletionPoint(completion, submission));
        ASSERT_EQ(GPUCompletionStatus::CompatibilityWaitIdle,
                  tracker.Wait(completion));
        ASSERT_TRUE(arena.NotifySubmission(completion));
        arena.PollCompletions();

        FrameConstantUploadAllocation reused;
        ASSERT_TRUE(arena.Allocate(&value, sizeof(value), reused));
        EXPECT_EQ(first.pageIdentity, reused.pageIdentity);
        EXPECT_EQ(0u, reused.dynamicOffset);
    }

    TEST(FrameConstantUploadArenaValidation, BufferAndMapFailuresAreFailClosed)
    {
        const uint32 value = 1;
        {
            FakeDevice device;
            device.failBufferCreation = true;
            FrameConstantUploadArena arena;
            ASSERT_TRUE(arena.Initialize(&device, 256, 2, "FrameConstantArenaCreateFail"));
            FrameConstantUploadAllocation allocation;
            EXPECT_FALSE(arena.Allocate(&value, sizeof(value), allocation));
            EXPECT_FALSE(allocation.IsValid());
        }
        {
            FakeDevice device;
            device.bufferMapSucceeds = false;
            FrameConstantUploadArena arena;
            ASSERT_TRUE(arena.Initialize(&device, 256, 2, "FrameConstantArenaMapFail"));
            FrameConstantUploadAllocation allocation;
            EXPECT_FALSE(arena.Allocate(&value, sizeof(value), allocation));
            EXPECT_FALSE(allocation.IsValid());
        }
    }

    TEST(FrameConstantUploadArenaValidation,
         CommitFailureDoesNotPublishSlotAndRetryUsesANewPage)
    {
        FakeDevice device;
        FrameConstantUploadArena arena;
        ASSERT_TRUE(arena.Initialize(&device, 256, 2, "FrameConstantArenaCommitFail"));

        const uint32 committedValue = 0xCAFE0001u;
        FrameConstantUploadAllocation committed;
        ASSERT_TRUE(arena.Allocate(&committedValue, sizeof(committedValue), committed));
        auto* committedBuffer = static_cast<FakeBuffer*>(committed.buffer.Get());
        ASSERT_NE(nullptr, committedBuffer);

        committedBuffer->SetCommitSucceeds(false);
        const uint32 failedValue = 0xCAFE0002u;
        FrameConstantUploadAllocation failed;
        EXPECT_FALSE(arena.Allocate(&failedValue, sizeof(failedValue), failed));
        EXPECT_FALSE(failed.IsValid());
        EXPECT_EQ(1u, arena.GetUnusablePageCount());

        uint32 retainedValue = 0;
        std::memcpy(&retainedValue,
                    committedBuffer->GetStorage().data() + committed.dynamicOffset,
                    sizeof(retainedValue));
        EXPECT_EQ(committedValue, retainedValue);

        const uint32 retryValue = 0xCAFE0003u;
        FrameConstantUploadAllocation retry;
        ASSERT_TRUE(arena.Allocate(&retryValue, sizeof(retryValue), retry));
        EXPECT_NE(committed.pageIdentity, retry.pageIdentity);
        EXPECT_EQ(0u, retry.dynamicOffset);
        EXPECT_EQ(2u, committedBuffer->GetCommitMappedWriteCount());
    }

    TEST(MaterialSystemValidation,
         PagedMaterialConstantsStartThe8193rdUpdateAtANewPageOffsetZero)
    {
        FakeDevice device;
        RenderRuntimeTestHarness gpuResources;
        ASSERT_TRUE(gpuResources.Initialize(&device));
        FakeDescriptorSetLayout materialLayout;
        MaterialSystem materialSystem;
        ASSERT_TRUE(materialSystem.Initialize(
            &device, &materialLayout, &gpuResources.GetRegistry()));

        MaterialBindingResult slot8191;
        MaterialBindingResult slot8192;
        for (uint32 index = 0; index != 8193; ++index)
        {
            MaterialBindingResult binding = materialSystem.PrepareMaterialBinding({}, nullptr);
            ASSERT_TRUE(binding.IsDrawable());
            ASSERT_TRUE(binding.constantBuffer);
            ASSERT_TRUE(binding.descriptorSetRef);
            if (index == 8191)
            {
                slot8191 = std::move(binding);
            }
            else if (index == 8192)
            {
                slot8192 = std::move(binding);
            }
        }

        ASSERT_TRUE(slot8191.constantBuffer);
        ASSERT_TRUE(slot8192.constantBuffer);
        EXPECT_GT(slot8191.dynamicOffsets[0], 0u);
        EXPECT_EQ(0u, slot8192.dynamicOffsets[0]);
        EXPECT_NE(slot8191.constantBuffer.Get(), slot8192.constantBuffer.Get());
        const RHIDescriptorBinding* pageBinding = FindBinding(slot8192.descriptorSet, 0);
        ASSERT_NE(nullptr, pageBinding);
        EXPECT_EQ(slot8192.constantBuffer.Get(), pageBinding->buffer);

        materialSystem.Shutdown();
        gpuResources.Shutdown();
    }

    TEST(MaterialSystemValidation, ClassifiesMaterialAlphaModes)
    {
        auto opaque = std::make_shared<Material>();
        opaque->SetAlphaMode(Material::AlphaMode::Opaque);
        EXPECT_EQ(MaterialRenderMode::Opaque,
                  ClassifyMaterialRenderMode(MaterialSourceAlphaMode::Opaque));

        auto masked = std::make_shared<Material>();
        masked->SetAlphaMode(Material::AlphaMode::Mask);
        EXPECT_EQ(MaterialRenderMode::Masked,
                  ClassifyMaterialRenderMode(MaterialSourceAlphaMode::Mask));

        auto transparent = std::make_shared<Material>();
        transparent->SetAlphaMode(Material::AlphaMode::Blend);
        EXPECT_EQ(MaterialRenderMode::Transparent,
                  ClassifyMaterialRenderMode(MaterialSourceAlphaMode::Blend));

        EXPECT_EQ(MaterialPipelineVariant::Opaque,
                  GetPipelineVariantForRenderMode(MaterialRenderMode::Opaque));
        EXPECT_EQ(MaterialPipelineVariant::Masked,
                  GetPipelineVariantForRenderMode(MaterialRenderMode::Masked));
        EXPECT_EQ(MaterialPipelineVariant::Transparent,
                  GetPipelineVariantForRenderMode(MaterialRenderMode::Transparent));
    }

    TEST(MaterialSystemValidation, MaterialTemplateCompileFailuresAreSpecificAndVisible)
    {
        {
            MaterialTemplate materialTemplate("null-device");
            materialTemplate.SetVertexShader("PBRLit.hlsl");
            materialTemplate.SetPixelShader("PBRLit.hlsl");

            EXPECT_FALSE(materialTemplate.Compile(nullptr));
            EXPECT_FALSE(materialTemplate.IsCompiled());
            EXPECT_EQ(materialTemplate.GetPipeline(), nullptr);
            EXPECT_TRUE(Contains(materialTemplate.GetLastCompileError(), "RHI device"));
        }

        {
            FakeDevice device;
            MaterialTemplate materialTemplate("missing-vertex");
            materialTemplate.SetPixelShader("PBRLit.hlsl");

            EXPECT_FALSE(materialTemplate.Compile(&device));
            EXPECT_FALSE(materialTemplate.IsCompiled());
            EXPECT_EQ(materialTemplate.GetPipeline(), nullptr);
            EXPECT_TRUE(Contains(materialTemplate.GetLastCompileError(), "vertex shader path"));
        }

        {
            FakeDevice device;
            MaterialTemplate materialTemplate("missing-pixel");
            materialTemplate.SetVertexShader("PBRLit.hlsl");

            EXPECT_FALSE(materialTemplate.Compile(&device));
            EXPECT_FALSE(materialTemplate.IsCompiled());
            EXPECT_EQ(materialTemplate.GetPipeline(), nullptr);
            EXPECT_TRUE(Contains(materialTemplate.GetLastCompileError(), "pixel shader path"));
        }

        {
            FakeDevice device;
            MaterialTemplate materialTemplate("missing-pipeline");
            materialTemplate.SetVertexShader("PBRLit.hlsl");
            materialTemplate.SetPixelShader("PBRLit.hlsl");

            EXPECT_FALSE(materialTemplate.Compile(&device));
            EXPECT_FALSE(materialTemplate.IsCompiled());
            EXPECT_EQ(materialTemplate.GetPipeline(), nullptr);
            EXPECT_TRUE(Contains(materialTemplate.GetLastCompileError(), "standalone material pipeline"));
        }
    }

    TEST(MaterialSystemValidation, MaterialBinderConvertToGPUUsesMaterialProperties)
    {
        Material material("gpu-material");
        material.SetBaseColor(0.25f, 0.5f, 0.75f, 0.9f);
        material.SetMetallicFactor(0.35f);
        material.SetRoughnessFactor(0.65f);
        material.SetNormalScale(0.8f);
        material.SetOcclusionStrength(0.7f);
        material.SetEmissiveColor({0.1f, 0.2f, 0.3f});
        material.SetEmissiveStrength(2.5f);
        material.SetAlphaMode(Material::AlphaMode::Blend);
        material.SetAlphaCutoff(0.42f);
        material.SetWorkflow(MaterialWorkflow::SpecularGlossiness);
        material.SetDoubleSided(true);

        const MaterialGPUConstants constants = MaterialBinder::ConvertToGPU(MakeMaterialSourceData(material));

        EXPECT_FLOAT_EQ(constants.baseColorFactor.x, 0.25f);
        EXPECT_FLOAT_EQ(constants.baseColorFactor.y, 0.5f);
        EXPECT_FLOAT_EQ(constants.baseColorFactor.z, 0.75f);
        EXPECT_FLOAT_EQ(constants.baseColorFactor.w, 0.9f);
        EXPECT_FLOAT_EQ(constants.metallicFactor, 0.35f);
        EXPECT_FLOAT_EQ(constants.roughnessFactor, 0.65f);
        EXPECT_FLOAT_EQ(constants.normalScale, 0.8f);
        EXPECT_FLOAT_EQ(constants.occlusionStrength, 0.7f);
        EXPECT_FLOAT_EQ(constants.emissiveColor.x, 0.1f);
        EXPECT_FLOAT_EQ(constants.emissiveColor.y, 0.2f);
        EXPECT_FLOAT_EQ(constants.emissiveColor.z, 0.3f);
        EXPECT_FLOAT_EQ(constants.emissiveStrength, 2.5f);
        EXPECT_FLOAT_EQ(constants.alphaCutoff, 0.42f);
        EXPECT_EQ(constants.alphaMode, static_cast<uint32>(MaterialGPUAlphaMode::Blend));
        EXPECT_EQ(constants.workflow, static_cast<uint32>(MaterialGPUWorkflow::SpecularGlossiness));
        EXPECT_EQ(constants.doubleSided, 1u);
    }

    TEST(MaterialSystemValidation, MaterialBinderConvertToGPUTextureFlagsReflectOptionalTextures)
    {
        Material material("texture-flags");

        MaterialGPUConstants constants = MaterialBinder::ConvertToGPU(MakeMaterialSourceData(material));
        EXPECT_EQ(constants.textureFlags, 0u);

        material.SetBaseColorTexture(TextureInfo("base-color.png"));
        material.SetNormalTexture(TextureInfo("normal.png"));
        material.SetMetallicRoughnessTexture(TextureInfo("mr.png"));
        material.SetOcclusionTexture(TextureInfo("ao.png"));
        material.SetEmissiveTexture(TextureInfo("emissive.png"));

        constants = MaterialBinder::ConvertToGPU(MakeMaterialSourceData(material));
        const uint32 expectedFlags =
            static_cast<uint32>(MaterialTextureFlags::HasBaseColor) |
            static_cast<uint32>(MaterialTextureFlags::HasNormal) |
            static_cast<uint32>(MaterialTextureFlags::HasMetallicRoughness) |
            static_cast<uint32>(MaterialTextureFlags::HasOcclusion) |
            static_cast<uint32>(MaterialTextureFlags::HasEmissive);
        EXPECT_EQ(constants.textureFlags, expectedFlags);

        material.ClearBaseColorTexture();
        material.ClearNormalTexture();
        material.ClearMetallicRoughnessTexture();
        material.ClearOcclusionTexture();
        material.ClearEmissiveTexture();

        constants = MaterialBinder::ConvertToGPU(MakeMaterialSourceData(material));
        EXPECT_EQ(constants.textureFlags, 0u);
    }

    TEST(MaterialSystemValidation, MaterialBinderBindUpdatesConstantsButReportsUnsupported)
    {
        FakeDevice device;
        MaterialBinder binder;
        binder.Initialize(&device, nullptr);
        ASSERT_TRUE(binder.IsInitialized());
        ASSERT_NE(device.lastCreatedBuffer, nullptr);

        Material material("bind-material");
        material.SetBaseColor(0.2f, 0.3f, 0.4f, 1.0f);
        material.SetRoughnessFactor(0.55f);

        FakeCommandContext ctx;
        binder.Bind(ctx, MakeMaterialSourceData(material));

        EXPECT_EQ(binder.GetLastBindStatus(), MaterialBindStatus::Unsupported);
        EXPECT_FALSE(binder.GetLastBindMessage().empty());
        EXPECT_TRUE(Contains(binder.GetLastBindMessage(), "R5b"));

        const MaterialGPUConstants constants = ReadMaterialConstants(*device.lastCreatedBuffer);
        EXPECT_FLOAT_EQ(constants.baseColorFactor.x, 0.2f);
        EXPECT_FLOAT_EQ(constants.baseColorFactor.y, 0.3f);
        EXPECT_FLOAT_EQ(constants.baseColorFactor.z, 0.4f);
        EXPECT_FLOAT_EQ(constants.roughnessFactor, 0.55f);

        binder.Shutdown();
    }

    TEST(MaterialSystemValidation, MaterialBinderDefaultFallbackIsObservable)
    {
        FakeDevice device;
        MaterialBinder binder;
        binder.Initialize(&device, nullptr);
        ASSERT_TRUE(binder.IsInitialized());
        ASSERT_NE(device.lastCreatedBuffer, nullptr);

        FakeCommandContext ctx;
        binder.Bind(ctx, 404);

        EXPECT_EQ(binder.GetLastBindStatus(), MaterialBindStatus::BoundDefaultMaterial);
        EXPECT_FALSE(binder.GetLastBindMessage().empty());
        EXPECT_TRUE(Contains(binder.GetLastBindMessage(), "default material"));

        const MaterialGPUConstants constants = ReadMaterialConstants(*device.lastCreatedBuffer);
        EXPECT_FLOAT_EQ(constants.baseColorFactor.x, 0.8f);
        EXPECT_FLOAT_EQ(constants.roughnessFactor, 0.5f);

        binder.Shutdown();
    }

    TEST(MaterialSystemValidation, MaterialBinderInitializationFailureKeepsErrorStatus)
    {
        FakeDevice device;
        device.failBufferCreation = true;

        MaterialBinder binder;
        binder.Initialize(&device, nullptr);

        EXPECT_FALSE(binder.IsInitialized());
        EXPECT_EQ(binder.GetLastBindStatus(), MaterialBindStatus::Error);
        EXPECT_FALSE(binder.GetLastBindMessage().empty());
        EXPECT_TRUE(Contains(binder.GetLastBindMessage(), "constant buffer"));
    }

    TEST(MaterialSystemValidation, MaterialBinderMapFailureKeepsErrorForMaterialAndDefaultBind)
    {
        {
            FakeDevice device;
            device.bufferMapSucceeds = false;

            MaterialBinder binder;
            binder.Initialize(&device, nullptr);
            ASSERT_TRUE(binder.IsInitialized());

            Material material("map-fail");
            FakeCommandContext ctx;
            binder.Bind(ctx, MakeMaterialSourceData(material));

            EXPECT_EQ(binder.GetLastBindStatus(), MaterialBindStatus::Error);
            EXPECT_FALSE(binder.GetLastBindMessage().empty());
            EXPECT_TRUE(Contains(binder.GetLastBindMessage(), "map material constant buffer"));

            binder.Shutdown();
        }

        {
            FakeDevice device;
            device.bufferMapSucceeds = false;

            MaterialBinder binder;
            binder.Initialize(&device, nullptr);
            ASSERT_TRUE(binder.IsInitialized());

            FakeCommandContext ctx;
            binder.BindDefault(ctx);

            EXPECT_EQ(binder.GetLastBindStatus(), MaterialBindStatus::Error);
            EXPECT_FALSE(binder.GetLastBindMessage().empty());
            EXPECT_TRUE(Contains(binder.GetLastBindMessage(), "map material constant buffer"));

            binder.Shutdown();
        }
    }

    TEST(MaterialSystemValidation, MaterialBindingReportsNotInitialized)
    {
        MaterialSystem materialSystem;

        const MaterialBindingResult result = materialSystem.PrepareMaterialBinding({}, nullptr);

        EXPECT_EQ(MaterialBindingStatus::NotInitialized, result.status);
        EXPECT_FALSE(result.IsDrawable());
        EXPECT_TRUE(result.IsError());
        EXPECT_TRUE(Contains(result.message, "not initialized"));
        EXPECT_EQ(MaterialBindingStatus::NotInitialized, materialSystem.GetLastBindingResult().status);
    }

    TEST(MaterialSystemValidation, MaterialBindingMapFailureIsVisibleError)
    {
        FakeDevice device;
        device.bufferMapSucceeds = false;

        RenderRuntimeTestHarness gpuResources;
        gpuResources.Initialize(&device);

        FakeDescriptorSetLayout materialLayout;
        MaterialSystem materialSystem;
        ASSERT_TRUE(materialSystem.Initialize(&device, &materialLayout, &gpuResources.GetRegistry()));

        const MaterialBindingResult result = materialSystem.PrepareMaterialBinding({}, nullptr);

        EXPECT_EQ(MaterialBindingStatus::Error, result.status);
        EXPECT_FALSE(result.IsDrawable());
        EXPECT_FALSE(result.constantsUpdated);
        EXPECT_TRUE(result.usedFallback);
        EXPECT_TRUE(Contains(result.message, "completion-tracked material constant page"));
        EXPECT_EQ(MaterialBindingStatus::Error, materialSystem.GetLastBindingResult().status);

        materialSystem.Shutdown();
        gpuResources.Shutdown();
    }

    TEST(MaterialSystemValidation, MaterialBindingDescriptorFailureFailsClosedForPagedOffset)
    {
        FakeDevice device;
        RenderRuntimeTestHarness gpuResources;
        gpuResources.Initialize(&device);

        FakeDescriptorSetLayout materialLayout;
        MaterialSystem materialSystem;
        ASSERT_TRUE(materialSystem.Initialize(&device, &materialLayout, &gpuResources.GetRegistry()));

        device.failDescriptorSetCreation = true;
        const MaterialBindingResult result = materialSystem.PrepareMaterialBinding({}, nullptr);

        EXPECT_EQ(MaterialBindingStatus::Error, result.status);
        EXPECT_FALSE(result.IsDrawable());
        EXPECT_TRUE(result.constantsUpdated);
        EXPECT_TRUE(result.usedFallback);
        EXPECT_EQ(nullptr, result.descriptorSet);
        EXPECT_TRUE(Contains(result.message, "page descriptor creation failed"));

        materialSystem.Shutdown();
        gpuResources.Shutdown();
    }

    TEST(MaterialSystemValidation, MaterialBindingNullMaterialUsesExplicitFallback)
    {
        FakeDevice device;
        RenderRuntimeTestHarness gpuResources;
        gpuResources.Initialize(&device);

        FakeDescriptorSetLayout materialLayout;
        MaterialSystem materialSystem;
        ASSERT_TRUE(materialSystem.Initialize(&device, &materialLayout, &gpuResources.GetRegistry()));

        const MaterialBindingResult result = materialSystem.PrepareMaterialBinding({}, nullptr);

        EXPECT_EQ(MaterialBindingStatus::Fallback, result.status);
        EXPECT_TRUE(result.IsDrawable());
        EXPECT_TRUE(result.usedFallback);
        EXPECT_NE(0u, result.descriptorContentKey);
        EXPECT_EQ(0u, result.descriptorRevision);
        EXPECT_TRUE(Contains(result.message, "fallback"));

        materialSystem.Shutdown();
        gpuResources.Shutdown();
    }

    TEST(MaterialSystemValidation, MaterialBindingResidentTexturesAreReady)
    {
        FakeDevice device;
        RenderRuntimeTestHarness gpuResources;
        ASSERT_TRUE(gpuResources.Initialize(&device));

        ResourceViewCache viewCache;
        viewCache.Initialize(&device);

        FakeDescriptorSetLayout materialLayout;
        MaterialSystem materialSystem;
        ASSERT_TRUE(materialSystem.Initialize(&device, &materialLayout, &gpuResources.GetRegistry()));

        auto texture = CreateTextureResource(306);
        Resource::MaterialResource materialResource;
        ConfigureMaterialWithAllTextures(materialResource, texture);

        gpuResources.UploadImmediate(texture.Get());
        ASSERT_TRUE(gpuResources.IsGPUReady(texture.GetId()));

        const MaterialBindingResult result = materialSystem.PrepareMaterialBinding(gpuResources.ResolveOrUpload(&materialResource), &viewCache);

        EXPECT_EQ(MaterialBindingStatus::Ready, result.status);
        EXPECT_TRUE(result.IsDrawable());
        EXPECT_TRUE(result.constantsUpdated);
        EXPECT_FALSE(result.usedFallback);
        EXPECT_NE(materialSystem.GetDefaultMaterialSet(), result.descriptorSet);
        const uint32 expectedFlags =
            static_cast<uint32>(MaterialTextureFlags::HasBaseColor) |
            static_cast<uint32>(MaterialTextureFlags::HasNormal) |
            static_cast<uint32>(MaterialTextureFlags::HasMetallicRoughness) |
            static_cast<uint32>(MaterialTextureFlags::HasOcclusion) |
            static_cast<uint32>(MaterialTextureFlags::HasEmissive);
        EXPECT_EQ(expectedFlags, result.textureFlags);
        EXPECT_TRUE(Contains(result.materialName, "render-material["));
        EXPECT_EQ(expectedFlags, materialSystem.GetLastBindingResult().textureFlags);
        EXPECT_EQ(result.materialName,
                  materialSystem.GetLastBindingResult().materialName);
        EXPECT_TRUE(Contains(result.message, "ready"));

        materialSystem.Shutdown();
        viewCache.Shutdown();
        gpuResources.Shutdown();
    }

    TEST(MaterialSystemValidation, MaterialBindingDefaultOptionsKeepReadyNormalTextureEnabled)
    {
        FakeDevice device;
        RenderRuntimeTestHarness gpuResources;
        gpuResources.Initialize(&device);

        ResourceViewCache viewCache;
        viewCache.Initialize(&device);

        FakeDescriptorSetLayout materialLayout;
        MaterialSystem materialSystem;
        ASSERT_TRUE(materialSystem.Initialize(&device, &materialLayout, &gpuResources.GetRegistry()));

        auto normalTexture = CreateTextureResource(320);
        Resource::MaterialResource materialResource;
        materialResource.SetId(220);
        materialResource.SetName("NormalOnlyMaterialResource");
        materialResource.SetMaterialData(std::make_shared<Material>("NormalOnlyMaterialResource"));
        materialResource.SetTexture("normal", normalTexture);

        gpuResources.UploadImmediate(normalTexture.Get());
        ASSERT_TRUE(gpuResources.IsGPUReady(normalTexture.GetId()));

        const MaterialBindingResult result = materialSystem.PrepareMaterialBinding(gpuResources.ResolveOrUpload(&materialResource), &viewCache);
        const uint32 normalFlag = static_cast<uint32>(MaterialTextureFlags::HasNormal);

        EXPECT_EQ(MaterialBindingStatus::Ready, result.status);
        EXPECT_TRUE(result.IsDrawable());
        EXPECT_FALSE(result.usedFallback);
        EXPECT_EQ(normalFlag, result.textureFlags & normalFlag);
        EXPECT_EQ(0u, result.fallbackTextureFlags & normalFlag);

        materialSystem.Shutdown();
        viewCache.Shutdown();
        gpuResources.Shutdown();
    }

    TEST(MaterialSystemValidation, MaterialBindingCanDisableNormalMapWhenTangentBasisIsUnavailable)
    {
        FakeDevice device;
        RenderRuntimeTestHarness gpuResources;
        gpuResources.Initialize(&device);

        ResourceViewCache viewCache;
        viewCache.Initialize(&device);

        FakeDescriptorSetLayout materialLayout;
        MaterialSystem materialSystem;
        ASSERT_TRUE(materialSystem.Initialize(&device, &materialLayout, &gpuResources.GetRegistry()));

        auto normalTexture = CreateTextureResource(321);
        Resource::MaterialResource materialResource;
        materialResource.SetId(221);
        materialResource.SetName("NormalMapDisabledMaterialResource");
        materialResource.SetMaterialData(std::make_shared<Material>("NormalMapDisabledMaterialResource"));
        materialResource.SetTexture("normal", normalTexture);

        gpuResources.UploadImmediate(normalTexture.Get());
        ASSERT_TRUE(gpuResources.IsGPUReady(normalTexture.GetId()));

        MaterialBindingOptions options;
        options.allowNormalMap = false;
        const MaterialBindingResult result =
            materialSystem.PrepareMaterialBinding(gpuResources.ResolveOrUpload(&materialResource), &viewCache, options);
        const uint32 normalFlag = static_cast<uint32>(MaterialTextureFlags::HasNormal);

        EXPECT_EQ(MaterialBindingStatus::Fallback, result.status);
        EXPECT_TRUE(result.IsDrawable());
        EXPECT_TRUE(result.usedFallback);
        EXPECT_EQ(0u, result.textureFlags & normalFlag);
        EXPECT_EQ(normalFlag, result.fallbackTextureFlags & normalFlag);
        EXPECT_TRUE(Contains(result.message, "normal map disabled"));
        ASSERT_NE(result.constantBuffer, nullptr);
        const MaterialGPUConstants constants =
            ReadMaterialConstants(*static_cast<FakeBuffer*>(result.constantBuffer.Get()));
        EXPECT_EQ(0u, constants.textureFlags & normalFlag);

        materialSystem.Shutdown();
        viewCache.Shutdown();
        gpuResources.Shutdown();
    }

    TEST(MaterialSystemValidation,
         MaterialBindingFallbackToReadyPublishesExactSemanticDescriptorEvidence)
    {
        FakeDevice device;
        RenderRuntimeTestHarness gpuResources;
        ASSERT_TRUE(gpuResources.Initialize(&device));

        ResourceViewCache viewCache;
        viewCache.Initialize(&device);

        FakeDescriptorSetLayout materialLayout;
        MaterialSystem materialSystem;
        ASSERT_TRUE(materialSystem.Initialize(
            &device, &materialLayout, &gpuResources.GetRegistry()));

        auto normalTexture = CreateTextureResource(322);
        Resource::MaterialResource materialResource;
        materialResource.SetId(223);
        materialResource.SetName("SemanticDescriptorEvidenceMaterial");
        materialResource.SetMaterialData(
            std::make_shared<Material>("SemanticDescriptorEvidenceMaterial"));
        materialResource.SetTexture("normal", normalTexture);
        ASSERT_TRUE(gpuResources.UploadImmediate(normalTexture.Get()));
        const RenderResourceHandle material =
            gpuResources.ResolveOrUpload(&materialResource);
        const RenderResourceHandle texture =
            gpuResources.GetHandle(normalTexture.GetId());
        ASSERT_TRUE(material.IsValid());
        ASSERT_TRUE(texture.IsValid());

        MaterialBindingOptions fallbackOptions;
        fallbackOptions.allowNormalMap = false;
        const MaterialBindingResult fallback =
            materialSystem.PrepareMaterialBinding(
                material, &viewCache, fallbackOptions);
        ASSERT_TRUE(fallback.IsDrawable());
        EXPECT_EQ(MaterialBindingStatus::Fallback, fallback.status);
        EXPECT_EQ(material, fallback.material);
        EXPECT_NE(0u, fallback.contentRevision);
        ASSERT_EQ(1u, fallback.textureEntries.size());
        EXPECT_EQ(MaterialUploadTextureSlot::Normal,
                  fallback.textureEntries[0].slot);
        EXPECT_EQ(texture, fallback.textureEntries[0].texture);
        EXPECT_NE(0u, fallback.textureEntries[0].contentRevision);
        EXPECT_TRUE(fallback.textureEntries[0].fallbackUsed);
        ASSERT_NE(0u, fallback.descriptorContentKey);
        ASSERT_NE(0u, fallback.descriptorRevision);

        const MaterialBindingResult ready = materialSystem.PrepareMaterialBinding(
            material, &viewCache);
        ASSERT_TRUE(ready.IsDrawable());
        EXPECT_EQ(MaterialBindingStatus::Ready, ready.status);
        EXPECT_EQ(material, ready.material);
        ASSERT_EQ(1u, ready.textureEntries.size());
        EXPECT_EQ(texture, ready.textureEntries[0].texture);
        EXPECT_FALSE(ready.textureEntries[0].fallbackUsed);
        EXPECT_NE(fallback.descriptorContentKey, ready.descriptorContentKey);
        EXPECT_GT(ready.descriptorRevision, fallback.descriptorRevision);

        materialSystem.Shutdown();
        viewCache.Shutdown();
        gpuResources.Shutdown();
    }

    TEST(MaterialSystemValidation,
         RasterMaterialSemanticKeyUsesActualResolvedTextureAndCanonicalConstants)
    {
        const auto makeReadyKey = [](
                                      Resource::ResourceId textureId,
                                      float32 roughness,
                                      float32 metallic,
                                      bool reserveUnrelatedTexture) -> uint64
        {
            FakeDevice device;
            RenderRuntimeTestHarness resources;
            if (!resources.Initialize(&device))
            {
                return 0;
            }
            ResourceViewCache viewCache;
            viewCache.Initialize(&device);
            FakeDescriptorSetLayout layout;
            MaterialSystem system;
            if (!system.Initialize(&device, &layout, &resources.GetRegistry()))
            {
                viewCache.Shutdown();
                resources.Shutdown();
                return 0;
            }
            if (reserveUnrelatedTexture)
            {
                auto unrelated = CreateTextureResource(9991u);
                if (!resources.UploadImmediate(unrelated.Get()))
                {
                    system.Shutdown();
                    viewCache.Shutdown();
                    resources.Shutdown();
                    return 0;
                }
            }
            auto texture = CreateTextureResource(textureId);
            auto materialData = std::make_shared<Material>("RasterSemanticMaterial");
            materialData->SetRoughnessFactor(roughness);
            materialData->SetMetallicFactor(metallic);
            Resource::MaterialResource material;
            material.SetId(9201u);
            material.SetName("RasterSemanticMaterial");
            material.SetMaterialData(materialData);
            material.SetTexture("albedo", texture);
            uint64 key = 0;
            if (resources.UploadImmediate(texture.Get()))
            {
                const MaterialBindingResult binding =
                    system.PrepareMaterialBinding(
                        resources.ResolveOrUpload(&material), &viewCache);
                if (binding.IsDrawable())
                {
                    key = binding.descriptorContentKey;
                }
            }
            system.Shutdown();
            viewCache.Shutdown();
            resources.Shutdown();
            return key;
        };

        // Independently allocated handles (and shifted physical slots) with
        // the same AssetIds must yield the same post-resolution evidence.
        const uint64 baseline = makeReadyKey(9202u, 0.25f, 0.0f, false);
        const uint64 sameAssetsDifferentAllocation =
            makeReadyKey(9202u, 0.25f, -0.0f, true);
        const uint64 changedTexture = makeReadyKey(9203u, 0.25f, 0.0f, true);
        const uint64 changedConstants = makeReadyKey(9202u, 0.75f, 0.0f, true);
        ASSERT_NE(0u, baseline);
        EXPECT_EQ(baseline, sameAssetsDifferentAllocation);
        EXPECT_NE(baseline, changedTexture);
        EXPECT_NE(baseline, changedConstants);
    }

    TEST(MaterialSystemValidation,
         InstanceBindingKeyUsesStableTextureAssetIdentity)
    {
        const auto makeBindingKey = [](
                                        Resource::ResourceId textureId,
                                        bool reserveUnrelatedTexture)
            -> MaterialInstanceBindingKey
        {
            FakeDevice device;
            RenderRuntimeTestHarness resources;
            if (!resources.Initialize(&device))
            {
                return {};
            }
            FakeDescriptorSetLayout layout;
            MaterialSystem system;
            if (!system.Initialize(&device, &layout, &resources.GetRegistry()))
            {
                resources.Shutdown();
                return {};
            }
            if (reserveUnrelatedTexture)
            {
                auto unrelated = CreateTextureResource(9992u);
                if (!resources.UploadImmediate(unrelated.Get()))
                {
                    system.Shutdown();
                    resources.Shutdown();
                    return {};
                }
            }

            auto texture = CreateTextureResource(textureId);
            Resource::MaterialResource material;
            material.SetId(9204u);
            material.SetName("StableInstanceBindingMaterial");
            material.SetMaterialData(
                std::make_shared<Material>("StableInstanceBindingMaterial"));
            material.SetTexture("albedo", texture);

            MaterialInstanceBindingKey key;
            if (resources.UploadImmediate(texture.Get()))
            {
                key = system.ResolveInstanceBindingKey(
                    resources.ResolveOrUpload(&material));
            }
            system.Shutdown();
            resources.Shutdown();
            return key;
        };

        const MaterialInstanceBindingKey baseline =
            makeBindingKey(9205u, false);
        const MaterialInstanceBindingKey sameAssetDifferentAllocation =
            makeBindingKey(9205u, true);
        const MaterialInstanceBindingKey changedTexture =
            makeBindingKey(9206u, true);

        ASSERT_TRUE(baseline.parameterTableCompatible);
        EXPECT_EQ(baseline, sameAssetDifferentAllocation);
        EXPECT_NE(baseline, changedTexture);
    }

    TEST(MaterialSystemValidation,
         RasterMaterialSemanticKeyCanonicalizesResolvedNormalFallbacks)
    {
        const auto makeNormalKey = [](
                                       Resource::ResourceId textureId,
                                       float32 normalScale,
                                       bool allowNormalMap) -> uint64
        {
            FakeDevice device;
            RenderRuntimeTestHarness resources;
            if (!resources.Initialize(&device))
            {
                return 0;
            }
            ResourceViewCache viewCache;
            viewCache.Initialize(&device);
            FakeDescriptorSetLayout layout;
            MaterialSystem system;
            if (!system.Initialize(&device, &layout, &resources.GetRegistry()))
            {
                viewCache.Shutdown();
                resources.Shutdown();
                return 0;
            }
            auto texture = CreateTextureResource(textureId);
            auto materialData = std::make_shared<Material>("RasterNormalSemantic");
            materialData->SetNormalScale(normalScale);
            Resource::MaterialResource material;
            material.SetId(9301u);
            material.SetName("RasterNormalSemantic");
            material.SetMaterialData(materialData);
            material.SetTexture("normal", texture);
            MaterialBindingOptions options;
            options.allowNormalMap = allowNormalMap;
            uint64 key = 0;
            if (resources.UploadImmediate(texture.Get()))
            {
                const MaterialBindingResult binding =
                    system.PrepareMaterialBinding(
                        resources.ResolveOrUpload(&material), &viewCache,
                        options);
                if (binding.IsDrawable())
                {
                    key = binding.descriptorContentKey;
                }
            }
            system.Shutdown();
            viewCache.Shutdown();
            resources.Shutdown();
            return key;
        };

        const uint64 disabledFirst = makeNormalKey(9302u, 0.25f, false);
        const uint64 disabledChanged = makeNormalKey(9303u, 2.0f, false);
        const uint64 enabledFirst = makeNormalKey(9302u, 0.25f, true);
        const uint64 enabledChanged = makeNormalKey(9303u, 2.0f, true);
        ASSERT_NE(0u, disabledFirst);
        ASSERT_NE(0u, enabledFirst);
        EXPECT_EQ(disabledFirst, disabledChanged);
        EXPECT_NE(enabledFirst, enabledChanged);
    }

    TEST(MaterialSystemValidation, MaterialBindingDoesNotReportFallbackForOmittedNormalTextureWhenNormalMapsDisabled)
    {
        FakeDevice device;
        RenderRuntimeTestHarness gpuResources;
        gpuResources.Initialize(&device);

        ResourceViewCache viewCache;
        viewCache.Initialize(&device);

        FakeDescriptorSetLayout materialLayout;
        MaterialSystem materialSystem;
        ASSERT_TRUE(materialSystem.Initialize(&device, &materialLayout, &gpuResources.GetRegistry()));

        Resource::MaterialResource materialResource;
        materialResource.SetId(222);
        materialResource.SetName("OmittedNormalMaterialResource");
        materialResource.SetMaterialData(std::make_shared<Material>("OmittedNormalMaterialResource"));

        MaterialBindingOptions options;
        options.allowNormalMap = false;
        const MaterialBindingResult result =
            materialSystem.PrepareMaterialBinding(gpuResources.ResolveOrUpload(&materialResource), &viewCache, options);
        const uint32 normalFlag = static_cast<uint32>(MaterialTextureFlags::HasNormal);

        EXPECT_EQ(MaterialBindingStatus::Ready, result.status);
        EXPECT_TRUE(result.IsDrawable());
        EXPECT_FALSE(result.usedFallback);
        EXPECT_EQ(0u, result.textureFlags & normalFlag);
        EXPECT_EQ(0u, result.fallbackTextureFlags & normalFlag);

        materialSystem.Shutdown();
        viewCache.Shutdown();
        gpuResources.Shutdown();
    }

    TEST(MaterialSystemValidation,
         RasterMaterialSemanticKeyUsesStableFallbackTokensForDefaultResources)
    {
        const auto makeFallbackKey = [](Resource::ResourceId textureId,
                                        bool bindDefaultTexture) -> uint64
        {
            FakeDevice device;
            RenderRuntimeTestHarness resources;
            if (!resources.Initialize(&device))
            {
                return 0;
            }
            ResourceViewCache viewCache;
            viewCache.Initialize(&device);
            FakeDescriptorSetLayout layout;
            MaterialSystem system;
            if (!system.Initialize(&device, &layout, &resources.GetRegistry()))
            {
                viewCache.Shutdown();
                resources.Shutdown();
                return 0;
            }
            Resource::MaterialResource material;
            material.SetId(9401u);
            material.SetName("RasterFallbackSemantic");
            material.SetMaterialData(
                std::make_shared<Material>("RasterFallbackSemantic"));
            Resource::TextureHandle texture;
            if (bindDefaultTexture)
            {
                texture = CreateTextureResource(textureId);
                texture->MarkDefaultFallback("semantic default");
                if (!resources.UploadImmediate(texture.Get()))
                {
                    system.Shutdown();
                    viewCache.Shutdown();
                    resources.Shutdown();
                    return 0;
                }
                material.SetTexture("albedo", texture);
            }
            const MaterialBindingResult binding = system.PrepareMaterialBinding(
                resources.ResolveOrUpload(&material), &viewCache);
            const uint64 key = binding.IsDrawable()
                ? binding.descriptorContentKey : 0;
            system.Shutdown();
            viewCache.Shutdown();
            resources.Shutdown();
            return key;
        };

        const uint64 firstDefault = makeFallbackKey(9402u, true);
        const uint64 replacementDefault = makeFallbackKey(9403u, true);
        const uint64 omitted = makeFallbackKey(0u, false);
        ASSERT_NE(0u, firstDefault);
        EXPECT_EQ(firstDefault, replacementDefault);
        EXPECT_EQ(firstDefault, omitted);
    }

    TEST(MaterialSystemValidation, MaterialBindingDefaultFallbackTextureDoesNotSetTextureFlag)
    {
        FakeDevice device;
        RenderRuntimeTestHarness gpuResources;
        gpuResources.Initialize(&device);

        ResourceViewCache viewCache;
        viewCache.Initialize(&device);

        FakeDescriptorSetLayout materialLayout;
        MaterialSystem materialSystem;
        ASSERT_TRUE(materialSystem.Initialize(&device, &materialLayout, &gpuResources.GetRegistry()));

        auto fallbackTexture = CreateTextureResource(307);
        fallbackTexture->MarkDefaultFallback("test default fallback");
        gpuResources.UploadImmediate(fallbackTexture.Get());
        ASSERT_TRUE(gpuResources.IsGPUReady(fallbackTexture.GetId()));

        Resource::MaterialResource fallbackMaterial;
        fallbackMaterial.SetId(203);
        fallbackMaterial.SetName("FallbackTextureMaterialResource");
        fallbackMaterial.SetMaterialData(std::make_shared<Material>("FallbackTextureMaterialResource"));
        fallbackMaterial.SetTexture("albedo", fallbackTexture);

        const MaterialBindingResult fallbackResult =
            materialSystem.PrepareMaterialBinding(gpuResources.ResolveOrUpload(&fallbackMaterial), &viewCache);

        EXPECT_EQ(MaterialBindingStatus::Fallback, fallbackResult.status);
        EXPECT_TRUE(fallbackResult.IsDrawable());
        EXPECT_TRUE(fallbackResult.usedFallback);
        EXPECT_EQ(0u, fallbackResult.textureFlags & static_cast<uint32>(MaterialTextureFlags::HasBaseColor));
        EXPECT_EQ(static_cast<uint32>(MaterialTextureFlags::HasBaseColor),
                  fallbackResult.fallbackTextureFlags & static_cast<uint32>(MaterialTextureFlags::HasBaseColor));

        Resource::MaterialResource omittedTextureMaterial;
        omittedTextureMaterial.SetId(204);
        omittedTextureMaterial.SetName("OmittedTextureMaterialResource");
        omittedTextureMaterial.SetMaterialData(std::make_shared<Material>("OmittedTextureMaterialResource"));

        const MaterialBindingResult omittedResult =
            materialSystem.PrepareMaterialBinding(gpuResources.ResolveOrUpload(&omittedTextureMaterial), &viewCache);

        EXPECT_EQ(MaterialBindingStatus::Ready, omittedResult.status);
        EXPECT_TRUE(omittedResult.IsDrawable());
        EXPECT_FALSE(omittedResult.usedFallback);
        EXPECT_EQ(0u, omittedResult.textureFlags);
        EXPECT_EQ(0u, omittedResult.fallbackTextureFlags);

        materialSystem.Shutdown();
        viewCache.Shutdown();
        gpuResources.Shutdown();
    }

    TEST(MaterialSystemValidation, MaterialBindingNonResidentTextureReportsFallback)
    {
        FakeDevice device;
        RenderRuntimeTestHarness gpuResources;
        gpuResources.Initialize(&device);

        ResourceViewCache viewCache;
        viewCache.Initialize(&device);

        FakeDescriptorSetLayout materialLayout;
        MaterialSystem materialSystem;
        ASSERT_TRUE(materialSystem.Initialize(&device, &materialLayout, &gpuResources.GetRegistry()));

        auto texture = CreateTextureResource(307);
        Resource::MaterialResource materialResource;
        ConfigureMaterialWithAlbedo(materialResource, texture);

        const MaterialBindingResult result = materialSystem.PrepareMaterialBinding(gpuResources.ResolveOrUpload(&materialResource), &viewCache);

        EXPECT_EQ(MaterialBindingStatus::Fallback, result.status);
        EXPECT_TRUE(result.IsDrawable());
        EXPECT_TRUE(result.usedFallback);
        EXPECT_TRUE(Contains(result.message, "fallback"));

        materialSystem.Shutdown();
        viewCache.Shutdown();
        gpuResources.Shutdown();
    }

    TEST(MaterialSystemValidation, UpdateMaterialConstantsReportsFallbackSuccessAndErrorFailure)
    {
        {
            FakeDevice device;
            RenderRuntimeTestHarness gpuResources;
            gpuResources.Initialize(&device);

            FakeDescriptorSetLayout materialLayout;
            MaterialSystem materialSystem;
            ASSERT_TRUE(materialSystem.Initialize(&device, &materialLayout, &gpuResources.GetRegistry()));

            EXPECT_TRUE(materialSystem.PrepareMaterialBinding({}, nullptr).IsDrawable());
            EXPECT_EQ(MaterialBindingStatus::Fallback, materialSystem.GetLastBindingResult().status);
            EXPECT_TRUE(materialSystem.GetLastBindingResult().constantsUpdated);

            materialSystem.Shutdown();
            gpuResources.Shutdown();
        }

        {
            FakeDevice device;
            device.bufferMapSucceeds = false;

            RenderRuntimeTestHarness gpuResources;
            gpuResources.Initialize(&device);

            FakeDescriptorSetLayout materialLayout;
            MaterialSystem materialSystem;
            ASSERT_TRUE(materialSystem.Initialize(&device, &materialLayout, &gpuResources.GetRegistry()));

            EXPECT_FALSE(materialSystem.PrepareMaterialBinding({}, nullptr).IsDrawable());
            EXPECT_EQ(MaterialBindingStatus::Error, materialSystem.GetLastBindingResult().status);
            EXPECT_FALSE(materialSystem.GetLastBindingResult().constantsUpdated);

            materialSystem.Shutdown();
            gpuResources.Shutdown();
        }

        {
            MaterialSystem materialSystem;

            EXPECT_FALSE(materialSystem.PrepareMaterialBinding({}, nullptr).IsDrawable());
            EXPECT_EQ(MaterialBindingStatus::NotInitialized, materialSystem.GetLastBindingResult().status);
        }
    }

    TEST(MaterialSystemValidation,
         MappedWriteCommitFailuresDoNotPublishMaterialStateAndRetriesSucceed)
    {
        FakeDevice device;
        RenderRuntimeTestHarness gpuResources;
        ASSERT_TRUE(gpuResources.Initialize(&device));
        RHIDescriptorSetLayoutRef materialLayout(
            new FakeDescriptorSetLayout());
        MaterialSystem materialSystem;
        ASSERT_TRUE(materialSystem.Initialize(
            &device, materialLayout.Get(), &gpuResources.GetRegistry()));

        auto materialData = std::make_shared<Material>("CommitFailureMaterial");
        materialData->SetBaseColor(Vec4(0.2f, 0.3f, 0.4f, 1.0f));
        Resource::MaterialResource resource;
        resource.SetId(901);
        resource.SetName("CommitFailureMaterial");
        resource.SetMaterialData(materialData);
        const RenderResourceHandle material = gpuResources.ResolveOrUpload(&resource);
        ASSERT_TRUE(material.IsValid());

        const MaterialBindingResult committedPage =
            materialSystem.PrepareMaterialBinding(material, nullptr);
        ASSERT_TRUE(committedPage.IsDrawable());
        ASSERT_TRUE(committedPage.constantBuffer);
        FakeBuffer* const page =
            static_cast<FakeBuffer*>(committedPage.constantBuffer.Get());
        ASSERT_NE(nullptr, page);
        const uint32 descriptorsBeforePageFailure = device.createdDescriptorSetCount;
        page->SetCommitSucceeds(false);
        const MaterialBindingResult pageFailure =
            materialSystem.PrepareMaterialBinding(material, nullptr);
        EXPECT_EQ(MaterialBindingStatus::Error, pageFailure.status);
        EXPECT_FALSE(pageFailure.IsDrawable());
        EXPECT_FALSE(pageFailure.constantsUpdated);
        EXPECT_EQ(nullptr, pageFailure.descriptorSet);
        EXPECT_FALSE(pageFailure.constantBuffer);
        EXPECT_FALSE(pageFailure.descriptorSetRef);
        EXPECT_EQ(descriptorsBeforePageFailure, device.createdDescriptorSetCount);
        EXPECT_TRUE(committedPage.IsDrawable());

        // The failed page is quarantined; retrying allocates a fresh page and
        // must not reinterpret the failed bytes as a successful update.
        page->SetCommitSucceeds(true);
        const MaterialBindingResult pageRetry =
            materialSystem.PrepareMaterialBinding(material, nullptr);
        EXPECT_TRUE(pageRetry.IsDrawable());
        EXPECT_TRUE(pageRetry.constantsUpdated);
        EXPECT_NE(nullptr, pageRetry.descriptorSet);
        ASSERT_TRUE(pageRetry.constantBuffer);
        EXPECT_NE(page, pageRetry.constantBuffer.Get());

        const std::array requests = {
            MaterialParameterTableEntryRequest{material, true}};
        MaterialParameterTableSnapshot committedTable;
        ASSERT_TRUE(materialSystem.CreateMaterialParameterTableSnapshot(
            requests, nullptr, committedTable));
        ASSERT_TRUE(committedTable.IsValid());
        const RHIBufferRef committedTableBuffer = committedTable.buffer;
        ASSERT_EQ(committedTable.slotCount,
                  committedTable.rasterMaterialSemanticKeysBySlot.size());
        ASSERT_LT(material.slot,
                  committedTable.rasterMaterialSemanticKeysBySlot.size());
        EXPECT_NE(0u,
                  committedTable.rasterMaterialSemanticKeysBySlot[
                      material.slot]);

        device.bufferCommitSucceeds = false;
        MaterialParameterTableSnapshot failedTable;
        EXPECT_FALSE(materialSystem.CreateMaterialParameterTableSnapshot(
            requests, nullptr, failedTable));
        EXPECT_FALSE(failedTable.IsValid());
        EXPECT_FALSE(failedTable.buffer);
        EXPECT_TRUE(failedTable.rasterMaterialSemanticKeysBySlot.empty());
        EXPECT_TRUE(committedTable.IsValid());
        EXPECT_EQ(committedTableBuffer.Get(), committedTable.buffer.Get());

        device.bufferCommitSucceeds = true;
        MaterialParameterTableSnapshot retriedTable;
        ASSERT_TRUE(materialSystem.CreateMaterialParameterTableSnapshot(
            requests, nullptr, retriedTable));
        EXPECT_TRUE(retriedTable.IsValid());
        EXPECT_NE(committedTable.buffer.Get(), retriedTable.buffer.Get());
        ASSERT_EQ(retriedTable.slotCount,
                  retriedTable.rasterMaterialSemanticKeysBySlot.size());
        EXPECT_EQ(committedTable.rasterMaterialSemanticKeysBySlot,
                  retriedTable.rasterMaterialSemanticKeysBySlot);

        MaterialBindingSnapshot committedSnapshot;
        ASSERT_TRUE(materialSystem.CreateMaterialBindingSnapshot(
            material, nullptr, {}, committedSnapshot));
        ASSERT_TRUE(committedSnapshot.IsDrawable());
        const RHIBufferRef committedSnapshotBuffer = committedSnapshot.constantBuffer;

        const uint32 descriptorsBeforeSnapshotFailure =
            device.createdDescriptorSetCount;
        device.bufferCommitSucceeds = false;
        MaterialBindingSnapshot failedSnapshot;
        EXPECT_FALSE(materialSystem.CreateMaterialBindingSnapshot(
            material, nullptr, {}, failedSnapshot));
        EXPECT_FALSE(failedSnapshot.IsDrawable());
        EXPECT_FALSE(failedSnapshot.constantBuffer);
        EXPECT_FALSE(failedSnapshot.descriptorSet);
        EXPECT_EQ(descriptorsBeforeSnapshotFailure,
                  device.createdDescriptorSetCount);
        EXPECT_TRUE(committedSnapshot.IsDrawable());
        EXPECT_EQ(committedSnapshotBuffer.Get(),
                  committedSnapshot.constantBuffer.Get());

        device.bufferCommitSucceeds = true;
        MaterialBindingSnapshot retriedSnapshot;
        ASSERT_TRUE(materialSystem.CreateMaterialBindingSnapshot(
            material, nullptr, {}, retriedSnapshot));
        EXPECT_TRUE(retriedSnapshot.IsDrawable());
        EXPECT_NE(committedSnapshot.constantBuffer.Get(),
                  retriedSnapshot.constantBuffer.Get());

        materialSystem.Shutdown();
        gpuResources.Shutdown();
    }

    TEST(MaterialSystemValidation, MaterialSetUsesResidentTextureViewForAlbedo)
    {
        FakeDevice device;
        RenderRuntimeTestHarness gpuResources;
        gpuResources.Initialize(&device);

        ResourceViewCache viewCache;
        viewCache.Initialize(&device);

        FakeDescriptorSetLayout materialSetLayout;
        MaterialSystem materialSystem;
        ASSERT_TRUE(materialSystem.Initialize(&device, &materialSetLayout, &gpuResources.GetRegistry()));

        Resource::TextureHandle albedo = CreateTextureResource(301);
        gpuResources.UploadImmediate(albedo.Get());
        ASSERT_EQ(RenderResourcePublicState::GPUReady, gpuResources.GetResourceState(albedo.GetId()));

        Resource::MaterialResource materialResource;
        ConfigureMaterialWithAlbedo(materialResource, albedo);
        RHIDescriptorSet* descriptorSet = materialSystem.PrepareMaterialBinding(gpuResources.ResolveOrUpload(&materialResource), &viewCache).descriptorSet;

        ASSERT_NE(nullptr, descriptorSet);
        ASSERT_NE(materialSystem.GetDefaultMaterialSet(), descriptorSet);

        const RHIDescriptorBinding* albedoBinding = FindBinding(descriptorSet, 1);
        ASSERT_NE(nullptr, albedoBinding);
        ASSERT_NE(nullptr, albedoBinding->textureView);
        EXPECT_EQ(gpuResources.GetTexture(albedo.GetId()), albedoBinding->textureView->GetTexture());

        const RHIDescriptorBinding* normalBinding = FindBinding(descriptorSet, 2);
        ASSERT_NE(nullptr, normalBinding);
        ASSERT_NE(nullptr, normalBinding->textureView);
        EXPECT_NE(albedoBinding->textureView, normalBinding->textureView);

        const RHIDescriptorBinding* samplerBinding = FindBinding(descriptorSet, 6);
        ASSERT_NE(nullptr, samplerBinding);
        EXPECT_NE(nullptr, samplerBinding->sampler);

        materialSystem.Shutdown();
        viewCache.Shutdown();
        gpuResources.Shutdown();
    }

    TEST(MaterialSystemValidation, MaterialSetUsesCookedBlockCompressedTextureViews)
    {
        ScopedTempFiles tempFiles;
        const std::filesystem::path albedoPath = tempFiles.Track(WriteCookedTextureArtifact(
            "rvx_material_system_cooked_bc1_albedo",
            Resource::TextureFormat::BC1,
            Resource::TextureUsage::Color,
            true,
            std::vector<uint8>(8, 0x11)));
        const std::filesystem::path metallicRoughnessPath = tempFiles.Track(WriteCookedTextureArtifact(
            "rvx_material_system_cooked_bc3_metallic_roughness",
            Resource::TextureFormat::BC3,
            Resource::TextureUsage::Data,
            false,
            std::vector<uint8>(16, 0x22)));
        const std::filesystem::path normalPath = tempFiles.Track(WriteCookedTextureArtifact(
            "rvx_material_system_cooked_bc5_normal",
            Resource::TextureFormat::BC5,
            Resource::TextureUsage::Normal,
            false,
            std::vector<uint8>(16, 0x33)));

        Resource::TextureLoader loader(nullptr);
        Resource::TextureHandle albedo(loader.LoadFromFile(albedoPath.string()));
        ASSERT_TRUE(albedo) << loader.GetLastLoadError();
        Resource::TextureHandle metallicRoughness(loader.LoadFromFile(metallicRoughnessPath.string()));
        ASSERT_TRUE(metallicRoughness) << loader.GetLastLoadError();
        Resource::TextureHandle normal(loader.LoadFromFile(normalPath.string()));
        ASSERT_TRUE(normal) << loader.GetLastLoadError();

        EXPECT_EQ(Resource::TextureFormat::BC1, albedo->GetFormat());
        EXPECT_TRUE(albedo->IsSRGB());
        EXPECT_EQ(Resource::TextureFormat::BC3, metallicRoughness->GetFormat());
        EXPECT_FALSE(metallicRoughness->IsSRGB());
        EXPECT_EQ(Resource::TextureFormat::BC5, normal->GetFormat());
        EXPECT_EQ(Resource::TextureUsage::Normal, normal->GetUsage());

        FakeDevice device;
        RenderRuntimeTestHarness gpuResources;
        gpuResources.Initialize(&device);

        ResourceViewCache viewCache;
        viewCache.Initialize(&device);

        FakeDescriptorSetLayout materialSetLayout;
        MaterialSystem materialSystem;
        ASSERT_TRUE(materialSystem.Initialize(&device, &materialSetLayout, &gpuResources.GetRegistry()));

        gpuResources.UploadImmediate(albedo.Get());
        gpuResources.UploadImmediate(metallicRoughness.Get());
        gpuResources.UploadImmediate(normal.Get());
        ASSERT_EQ(RenderResourcePublicState::GPUReady, gpuResources.GetResourceState(albedo.GetId()));
        ASSERT_EQ(RenderResourcePublicState::GPUReady, gpuResources.GetResourceState(metallicRoughness.GetId()));
        ASSERT_EQ(RenderResourcePublicState::GPUReady, gpuResources.GetResourceState(normal.GetId()));

        Resource::MaterialResource materialResource;
        materialResource.SetId(223);
        materialResource.SetName("CookedBlockCompressedMaterialResource");
        materialResource.SetMaterialData(std::make_shared<Material>("CookedBlockCompressedMaterialResource"));
        materialResource.SetTexture("albedo", albedo);
        materialResource.SetTexture("metallic_roughness", metallicRoughness);
        materialResource.SetTexture("normal", normal);

        const MaterialBindingResult result = materialSystem.PrepareMaterialBinding(gpuResources.ResolveOrUpload(&materialResource), &viewCache);

        ASSERT_EQ(MaterialBindingStatus::Ready, result.status);
        ASSERT_NE(nullptr, result.descriptorSet);
        EXPECT_FALSE(result.usedFallback);
        const uint32 expectedFlags =
            static_cast<uint32>(MaterialTextureFlags::HasBaseColor) |
            static_cast<uint32>(MaterialTextureFlags::HasNormal) |
            static_cast<uint32>(MaterialTextureFlags::HasMetallicRoughness);
        EXPECT_EQ(expectedFlags, result.textureFlags);
        EXPECT_EQ(0u, result.fallbackTextureFlags);

        const RHIDescriptorBinding* albedoBinding = FindBinding(result.descriptorSet, 1);
        const RHIDescriptorBinding* normalBinding = FindBinding(result.descriptorSet, 2);
        const RHIDescriptorBinding* metallicRoughnessBinding = FindBinding(result.descriptorSet, 3);
        ASSERT_NE(nullptr, albedoBinding);
        ASSERT_NE(nullptr, normalBinding);
        ASSERT_NE(nullptr, metallicRoughnessBinding);
        ASSERT_NE(nullptr, albedoBinding->textureView);
        ASSERT_NE(nullptr, normalBinding->textureView);
        ASSERT_NE(nullptr, metallicRoughnessBinding->textureView);

        EXPECT_EQ(gpuResources.GetTexture(albedo.GetId()), albedoBinding->textureView->GetTexture());
        EXPECT_EQ(RHIFormat::BC1_UNORM_SRGB, albedoBinding->textureView->GetFormat());
        EXPECT_EQ(gpuResources.GetTexture(normal.GetId()), normalBinding->textureView->GetTexture());
        EXPECT_EQ(RHIFormat::BC5_UNORM, normalBinding->textureView->GetFormat());
        EXPECT_EQ(gpuResources.GetTexture(metallicRoughness.GetId()),
                  metallicRoughnessBinding->textureView->GetTexture());
        EXPECT_EQ(RHIFormat::BC3_UNORM, metallicRoughnessBinding->textureView->GetFormat());

        ASSERT_NE(result.constantBuffer, nullptr);
        const MaterialGPUConstants constants =
            ReadMaterialConstants(*static_cast<FakeBuffer*>(result.constantBuffer.Get()));
        EXPECT_EQ(expectedFlags, constants.textureFlags);

        materialSystem.Shutdown();
        viewCache.Shutdown();
        gpuResources.Shutdown();
    }

    TEST(MaterialSystemValidation, MaterialSystemCreatesExplicitMipFilteredMaterialSampler)
    {
        FakeDevice device;
        RenderRuntimeTestHarness gpuResources;
        gpuResources.Initialize(&device);

        FakeDescriptorSetLayout materialSetLayout;
        MaterialSystem materialSystem;
        ASSERT_TRUE(materialSystem.Initialize(&device, &materialSetLayout, &gpuResources.GetRegistry()));

        ASSERT_FALSE(device.createdSamplerDescs.empty());
        const RHISamplerDesc& samplerDesc = device.createdSamplerDescs.back();
        EXPECT_EQ(RHIFilterMode::Linear, samplerDesc.minFilter);
        EXPECT_EQ(RHIFilterMode::Linear, samplerDesc.magFilter);
        EXPECT_EQ(RHIFilterMode::Linear, samplerDesc.mipFilter);
        EXPECT_EQ(RHIAddressMode::Repeat, samplerDesc.addressU);
        EXPECT_EQ(RHIAddressMode::Repeat, samplerDesc.addressV);
        EXPECT_EQ(RHIAddressMode::Repeat, samplerDesc.addressW);
        EXPECT_TRUE(samplerDesc.anisotropyEnable);
        EXPECT_FLOAT_EQ(8.0f, samplerDesc.maxAnisotropy);

        materialSystem.Shutdown();
        gpuResources.Shutdown();
    }

    TEST(MaterialSystemValidation, MaterialSetPreservesPerTextureSamplerSemantics)
    {
        FakeDevice device;
        RenderRuntimeTestHarness gpuResources;
        gpuResources.Initialize(&device);

        ResourceViewCache viewCache;
        viewCache.Initialize(&device);

        FakeDescriptorSetLayout materialSetLayout;
        MaterialSystem materialSystem;
        ASSERT_TRUE(materialSystem.Initialize(
            &device, &materialSetLayout, &gpuResources.GetRegistry()));

        const Resource::TextureHandle texture = CreateTextureResource(502);
        gpuResources.UploadImmediate(texture.Get());
        ASSERT_TRUE(gpuResources.IsGPUReady(texture.GetId()));

        auto material = std::make_shared<Material>("SamplerSemanticMaterial");
        TextureInfo baseColor("base-color.png");
        baseColor.wrapS = TextureInfo::WrapMode::ClampToEdge;
        baseColor.wrapT = TextureInfo::WrapMode::MirrorRepeat;
        baseColor.minFilter = TextureInfo::FilterMode::Nearest;
        baseColor.magFilter = TextureInfo::FilterMode::Nearest;
        material->SetBaseColorTexture(baseColor);

        TextureInfo normal("normal.png");
        normal.minFilter = TextureInfo::FilterMode::Linear;
        material->SetNormalTexture(normal);

        TextureInfo metallicRoughness("metallic-roughness.png");
        metallicRoughness.minFilter =
            TextureInfo::FilterMode::LinearMipmapNearest;
        material->SetMetallicRoughnessTexture(metallicRoughness);

        TextureInfo occlusion("occlusion.png");
        occlusion.minFilter = TextureInfo::FilterMode::NearestMipmapLinear;
        material->SetOcclusionTexture(occlusion);

        TextureInfo emissive("emissive.png");
        emissive.minFilter = TextureInfo::FilterMode::LinearMipmapLinear;
        material->SetEmissiveTexture(emissive);

        Resource::MaterialResource materialResource;
        materialResource.SetId(503);
        materialResource.SetName("SamplerSemanticMaterialResource");
        materialResource.SetMaterialData(material);
        materialResource.SetTexture("albedo", texture);
        materialResource.SetTexture("normal", texture);
        materialResource.SetTexture("metallic_roughness", texture);
        materialResource.SetTexture("ao", texture);
        materialResource.SetTexture("emissive", texture);

        const MaterialBindingResult result = materialSystem.PrepareMaterialBinding(
            gpuResources.ResolveOrUpload(&materialResource), &viewCache);
        ASSERT_EQ(MaterialBindingStatus::Ready, result.status);
        ASSERT_TRUE(result.IsDrawable());

        const auto samplerAt = [&result](uint32 binding) -> const FakeSampler*
        {
            const RHIDescriptorBinding* descriptor =
                FindBinding(result.descriptorSet, binding);
            return descriptor
                ? static_cast<const FakeSampler*>(descriptor->sampler)
                : nullptr;
        };

        const FakeSampler* baseColorSampler = samplerAt(6);
        ASSERT_NE(nullptr, baseColorSampler);
        const RHISamplerDesc& baseColorDesc = baseColorSampler->GetDesc();
        EXPECT_EQ(RHIFilterMode::Nearest, baseColorDesc.minFilter);
        EXPECT_EQ(RHIFilterMode::Nearest, baseColorDesc.magFilter);
        EXPECT_EQ(RHIFilterMode::Nearest, baseColorDesc.mipFilter);
        EXPECT_FLOAT_EQ(0.0f, baseColorDesc.maxLod);
        EXPECT_EQ(RHIAddressMode::ClampToEdge, baseColorDesc.addressU);
        EXPECT_EQ(RHIAddressMode::MirrorRepeat, baseColorDesc.addressV);

        const FakeSampler* normalSampler = samplerAt(7);
        ASSERT_NE(nullptr, normalSampler);
        const RHISamplerDesc& normalDesc = normalSampler->GetDesc();
        EXPECT_EQ(RHIFilterMode::Linear, normalDesc.minFilter);
        EXPECT_EQ(RHIFilterMode::Nearest, normalDesc.mipFilter);
        EXPECT_FLOAT_EQ(0.0f, normalDesc.maxLod);

        const FakeSampler* metallicRoughnessSampler = samplerAt(8);
        ASSERT_NE(nullptr, metallicRoughnessSampler);
        const RHISamplerDesc& metallicRoughnessDesc =
            metallicRoughnessSampler->GetDesc();
        EXPECT_EQ(RHIFilterMode::Linear, metallicRoughnessDesc.minFilter);
        EXPECT_EQ(RHIFilterMode::Nearest, metallicRoughnessDesc.mipFilter);
        EXPECT_GT(metallicRoughnessDesc.maxLod, 0.0f);

        const FakeSampler* occlusionSampler = samplerAt(9);
        ASSERT_NE(nullptr, occlusionSampler);
        const RHISamplerDesc& occlusionDesc = occlusionSampler->GetDesc();
        EXPECT_EQ(RHIFilterMode::Nearest, occlusionDesc.minFilter);
        EXPECT_EQ(RHIFilterMode::Linear, occlusionDesc.mipFilter);

        const FakeSampler* emissiveSampler = samplerAt(10);
        ASSERT_NE(nullptr, emissiveSampler);
        const RHISamplerDesc& emissiveDesc = emissiveSampler->GetDesc();
        EXPECT_EQ(RHIFilterMode::Linear, emissiveDesc.minFilter);
        EXPECT_EQ(RHIFilterMode::Linear, emissiveDesc.mipFilter);

        materialSystem.Shutdown();
        viewCache.Shutdown();
        gpuResources.Shutdown();
    }

    TEST(MaterialSystemValidation, MaterialDescriptorSetsKeepEnvironmentIBLFrameOwned)
    {
        FakeDevice device;
        RenderRuntimeTestHarness gpuResources;
        gpuResources.Initialize(&device);

        ResourceViewCache viewCache;
        viewCache.Initialize(&device);

        FakeDescriptorSetLayout materialSetLayout;
        MaterialSystem materialSystem;
        ASSERT_TRUE(materialSystem.Initialize(
            &device, &materialSetLayout, &gpuResources.GetRegistry()));

        RHIDescriptorSet* defaultSet = materialSystem.GetDefaultMaterialSet();
        ASSERT_NE(nullptr, defaultSet);
        for (uint32 binding = 6; binding <= 10; ++binding)
        {
            const RHIDescriptorBinding* sampler = FindBinding(defaultSet, binding);
            ASSERT_NE(nullptr, sampler);
            EXPECT_NE(nullptr, sampler->sampler);
            EXPECT_EQ(nullptr, sampler->textureView);
        }
        const RHIDescriptorBinding* defaultParameterTable =
            FindBinding(defaultSet, 11);
        ASSERT_NE(nullptr, defaultParameterTable);
        ASSERT_NE(nullptr, defaultParameterTable->buffer);
        EXPECT_TRUE(HasFlag(defaultParameterTable->buffer->GetUsage(),
                            RHIBufferUsage::Structured));
        EXPECT_TRUE(HasFlag(defaultParameterTable->buffer->GetUsage(),
                            RHIBufferUsage::ShaderResource));
        EXPECT_EQ(nullptr, FindBinding(defaultSet, 12));
        EXPECT_EQ(nullptr, FindBinding(defaultSet, 13));

        Resource::MaterialResource materialResource;
        materialResource.SetId(501);
        materialResource.SetName("FrameOwnedIBLMaterial");
        materialResource.SetMaterialData(std::make_shared<Material>());
        const MaterialBindingResult result =
            materialSystem.PrepareMaterialBinding(
                gpuResources.ResolveOrUpload(&materialResource),
                &viewCache);
        ASSERT_TRUE(result.IsDrawable());
        for (uint32 binding = 6; binding <= 10; ++binding)
        {
            const RHIDescriptorBinding* sampler =
                FindBinding(result.descriptorSet, binding);
            ASSERT_NE(nullptr, sampler);
            EXPECT_NE(nullptr, sampler->sampler);
            EXPECT_EQ(nullptr, sampler->textureView);
        }
        const RHIDescriptorBinding* parameterTable =
            FindBinding(result.descriptorSet, 11);
        ASSERT_NE(nullptr, parameterTable);
        EXPECT_EQ(defaultParameterTable->buffer, parameterTable->buffer);
        EXPECT_EQ(nullptr, FindBinding(result.descriptorSet, 12));
        EXPECT_EQ(nullptr, FindBinding(result.descriptorSet, 13));

        materialSystem.Shutdown();
        viewCache.Shutdown();
        gpuResources.Shutdown();
    }

    TEST(MaterialSystemValidation,
         MaterialParameterTableUsesStableResourceSlotsAndExactParameters)
    {
        FakeDevice device;
        RenderRuntimeTestHarness gpuResources;
        gpuResources.Initialize(&device);
        ResourceViewCache viewCache;
        viewCache.Initialize(&device);
        FakeDescriptorSetLayout materialSetLayout;
        MaterialSystem materialSystem;
        ASSERT_TRUE(materialSystem.Initialize(
            &device, &materialSetLayout, &gpuResources.GetRegistry()));

        auto firstMaterial = std::make_shared<Material>("FirstTableMaterial");
        firstMaterial->SetBaseColor(Vec4(0.2f, 0.3f, 0.4f, 1.0f));
        firstMaterial->SetMetallicFactor(0.25f);
        firstMaterial->SetRoughnessFactor(0.75f);
        Resource::MaterialResource firstResource;
        firstResource.SetId(801);
        firstResource.SetName("FirstTableMaterial");
        firstResource.SetMaterialData(firstMaterial);

        auto secondMaterial = std::make_shared<Material>("SecondTableMaterial");
        secondMaterial->SetBaseColor(Vec4(0.8f, 0.7f, 0.6f, 1.0f));
        secondMaterial->SetMetallicFactor(0.9f);
        secondMaterial->SetRoughnessFactor(0.1f);
        Resource::MaterialResource secondResource;
        secondResource.SetId(802);
        secondResource.SetName("SecondTableMaterial");
        secondResource.SetMaterialData(secondMaterial);

        const RenderResourceHandle first =
            gpuResources.ResolveOrUpload(&firstResource);
        const RenderResourceHandle second =
            gpuResources.ResolveOrUpload(&secondResource);
        ASSERT_TRUE(first.IsValid());
        ASSERT_TRUE(second.IsValid());
        const MaterialInstanceBindingKey firstKey =
            materialSystem.ResolveInstanceBindingKey(first);
        const MaterialInstanceBindingKey secondKey =
            materialSystem.ResolveInstanceBindingKey(second);
        EXPECT_TRUE(firstKey.parameterTableCompatible);
        EXPECT_EQ(firstKey, secondKey);

        const std::array requests = {
            MaterialParameterTableEntryRequest{first, true},
            MaterialParameterTableEntryRequest{second, true}};
        MaterialParameterTableSnapshot snapshot;
        ASSERT_TRUE(materialSystem.CreateMaterialParameterTableSnapshot(
            requests, &viewCache, snapshot));
        EXPECT_EQ(snapshot.materialCount, 2u);
        EXPECT_EQ(snapshot.slotCount, std::max(first.slot, second.slot) + 1u);
        ASSERT_EQ(snapshot.buffer->GetStride(), sizeof(MaterialGPUConstants));
        ASSERT_EQ(snapshot.slotCount,
                  snapshot.rasterMaterialSemanticKeysBySlot.size());
        ASSERT_LT(first.slot, snapshot.rasterMaterialSemanticKeysBySlot.size());
        ASSERT_LT(second.slot, snapshot.rasterMaterialSemanticKeysBySlot.size());
        EXPECT_NE(0u, snapshot.rasterMaterialSemanticKeysBySlot[first.slot]);
        EXPECT_NE(0u, snapshot.rasterMaterialSemanticKeysBySlot[second.slot]);

        auto* buffer = static_cast<FakeBuffer*>(snapshot.buffer.Get());
        const std::vector<uint8>& bytes = buffer->GetStorage();
        MaterialGPUConstants firstConstants{};
        MaterialGPUConstants secondConstants{};
        std::memcpy(&firstConstants,
                    bytes.data() + first.slot * sizeof(MaterialGPUConstants),
                    sizeof(firstConstants));
        std::memcpy(&secondConstants,
                    bytes.data() + second.slot * sizeof(MaterialGPUConstants),
                    sizeof(secondConstants));
        EXPECT_FLOAT_EQ(firstConstants.metallicFactor, 0.25f);
        EXPECT_FLOAT_EQ(firstConstants.roughnessFactor, 0.75f);
        EXPECT_FLOAT_EQ(secondConstants.metallicFactor, 0.9f);
        EXPECT_FLOAT_EQ(secondConstants.roughnessFactor, 0.1f);

        MaterialBindingOptions firstTableOptions;
        firstTableOptions.materialParameterTable = snapshot.buffer.Get();
        const MaterialBindingResult firstTableBinding =
            materialSystem.PrepareMaterialBinding(
                first, &viewCache, firstTableOptions);
        ASSERT_TRUE(firstTableBinding.IsDrawable());
        EXPECT_EQ(snapshot.rasterMaterialSemanticKeysBySlot[first.slot],
                  firstTableBinding.descriptorContentKey);
        const RHIDescriptorBinding* firstTableDescriptor =
            FindBinding(firstTableBinding.descriptorSet, 11);
        ASSERT_NE(nullptr, firstTableDescriptor);
        EXPECT_EQ(snapshot.buffer.Get(), firstTableDescriptor->buffer);

        MaterialParameterTableSnapshot nextFrameSnapshot;
        ASSERT_TRUE(materialSystem.CreateMaterialParameterTableSnapshot(
            requests, &viewCache, nextFrameSnapshot));
        ASSERT_NE(snapshot.buffer.Get(), nextFrameSnapshot.buffer.Get());
        materialSystem.BeginFrame();
        MaterialBindingOptions nextFrameOptions;
        nextFrameOptions.materialParameterTable =
            nextFrameSnapshot.buffer.Get();
        const MaterialBindingResult nextFrameBinding =
            materialSystem.PrepareMaterialBinding(
                first, &viewCache, nextFrameOptions);
        ASSERT_TRUE(nextFrameBinding.IsDrawable());
        const RHIDescriptorBinding* nextFrameTableDescriptor =
            FindBinding(nextFrameBinding.descriptorSet, 11);
        ASSERT_NE(nullptr, nextFrameTableDescriptor);
        EXPECT_EQ(nextFrameSnapshot.buffer.Get(),
                  nextFrameTableDescriptor->buffer);
        EXPECT_NE(firstTableBinding.descriptorSet,
                  nextFrameBinding.descriptorSet);

        const std::array conflicting = {
            MaterialParameterTableEntryRequest{first, true},
            MaterialParameterTableEntryRequest{
                RenderResourceHandle{first.slot, first.generation + 1u}, true}};
        EXPECT_FALSE(materialSystem.CreateMaterialParameterTableSnapshot(
            conflicting, &viewCache, snapshot));

        materialSystem.Shutdown();
        viewCache.Shutdown();
        gpuResources.Shutdown();
    }


    TEST(MaterialSystemValidation, MaterialSetFallsBackWhenTextureIsNotGPUReady)
    {
        FakeDevice device;
        RenderRuntimeTestHarness gpuResources;
        gpuResources.Initialize(&device);

        ResourceViewCache viewCache;
        viewCache.Initialize(&device);

        FakeDescriptorSetLayout materialSetLayout;
        MaterialSystem materialSystem;
        ASSERT_TRUE(materialSystem.Initialize(&device, &materialSetLayout, &gpuResources.GetRegistry()));

        Resource::TextureHandle albedo = CreateTextureResource(302);
        ASSERT_NE(RenderResourcePublicState::GPUReady, gpuResources.GetResourceState(albedo.GetId()));

        Resource::MaterialResource materialResource;
        ConfigureMaterialWithAlbedo(materialResource, albedo);
        RHIDescriptorSet* descriptorSet = materialSystem.PrepareMaterialBinding(gpuResources.ResolveOrUpload(&materialResource), &viewCache).descriptorSet;

        ASSERT_NE(nullptr, descriptorSet);

        const RHIDescriptorBinding* defaultAlbedoBinding = FindBinding(materialSystem.GetDefaultMaterialSet(), 1);
        const RHIDescriptorBinding* albedoBinding = FindBinding(descriptorSet, 1);
        ASSERT_NE(nullptr, defaultAlbedoBinding);
        ASSERT_NE(nullptr, albedoBinding);
        ASSERT_NE(nullptr, defaultAlbedoBinding->textureView);
        ASSERT_NE(nullptr, albedoBinding->textureView);

        EXPECT_EQ(defaultAlbedoBinding->textureView, albedoBinding->textureView);
        EXPECT_EQ(nullptr, gpuResources.GetTexture(albedo.GetId()));

        materialSystem.Shutdown();
        viewCache.Shutdown();
        gpuResources.Shutdown();
    }

    TEST(MaterialSystemValidation, MaterialSetFallsBackWhenTextureUploadFailed)
    {
        FakeDevice device;
        RenderRuntimeTestHarness gpuResources;
        gpuResources.Initialize(&device);

        ResourceViewCache viewCache;
        viewCache.Initialize(&device);

        FakeDescriptorSetLayout materialSetLayout;
        MaterialSystem materialSystem;
        ASSERT_TRUE(materialSystem.Initialize(&device, &materialSetLayout, &gpuResources.GetRegistry()));

        Resource::TextureHandle albedo = CreateTextureResource(
            303,
            Resource::TextureFormat::RGBA8,
            {255, 255, 255});
        gpuResources.UploadImmediate(albedo.Get());
        ASSERT_EQ(RenderResourcePublicState::Reserved,
                  gpuResources.GetResourceState(albedo.GetId()));

        Resource::MaterialResource materialResource;
        ConfigureMaterialWithAlbedo(materialResource, albedo);
        RHIDescriptorSet* descriptorSet = materialSystem.PrepareMaterialBinding(gpuResources.ResolveOrUpload(&materialResource), &viewCache).descriptorSet;

        ASSERT_NE(nullptr, descriptorSet);

        const RHIDescriptorBinding* defaultAlbedoBinding = FindBinding(materialSystem.GetDefaultMaterialSet(), 1);
        const RHIDescriptorBinding* albedoBinding = FindBinding(descriptorSet, 1);
        ASSERT_NE(nullptr, defaultAlbedoBinding);
        ASSERT_NE(nullptr, albedoBinding);
        ASSERT_NE(nullptr, defaultAlbedoBinding->textureView);
        ASSERT_NE(nullptr, albedoBinding->textureView);

        EXPECT_EQ(defaultAlbedoBinding->textureView, albedoBinding->textureView);
        EXPECT_EQ(nullptr, gpuResources.GetTexture(albedo.GetId()));

        materialSystem.Shutdown();
        viewCache.Shutdown();
        gpuResources.Shutdown();
    }

    TEST(MaterialSystemValidation, MaterialResourceTracksShaderRuntimeContractAndDependency)
    {
        Resource::MaterialResource materialResource;
        materialResource.SetId(601);
        materialResource.SetName("ShaderContractMaterial");
        materialResource.SetMaterialData(std::make_shared<Material>());

        Resource::ShaderHandle shader = CreateShaderResource(602);
        ASSERT_TRUE(shader->HasValidRuntimeContract());

        materialResource.SetShader(shader);

        EXPECT_TRUE(materialResource.HasShader());
        EXPECT_TRUE(materialResource.HasValidShaderRuntimeContract());
        EXPECT_EQ(materialResource.GetShaderRuntimeContractHash(), shader->GetRuntimeContractHash());
        EXPECT_EQ(materialResource.GetShader().Get(), shader.Get());

        const Resource::MaterialShaderContractSnapshot snapshot =
            materialResource.GetShaderContractSnapshot();
        EXPECT_TRUE(snapshot.shaderAssigned);
        EXPECT_TRUE(snapshot.shaderLoaded);
        EXPECT_TRUE(snapshot.shaderContractValid);
        EXPECT_EQ(snapshot.shaderResourceId, shader.GetId());
        EXPECT_EQ(snapshot.shaderContractHash, shader->GetRuntimeContractHash());
        EXPECT_EQ(snapshot.shaderPayloadHash, shader->GetRuntimeContract().payloadHash);
        EXPECT_EQ(snapshot.shaderContractKey, shader->GetRuntimeContract().cacheKey);
        EXPECT_NE(snapshot.shaderContractKey.find("backend=DX12"), std::string::npos);
        EXPECT_NE(snapshot.shaderContractKey.find("stage=Pixel"), std::string::npos);

        const std::string snapshotJson = materialResource.ExportShaderContractSnapshotJson();
        EXPECT_NE(snapshotJson.find("\"schemaVersion\": 1"), std::string::npos);
        EXPECT_NE(snapshotJson.find("\"schemaId\": \"RVX.Resource.MaterialShaderContractSnapshot\""),
                  std::string::npos);
        EXPECT_NE(snapshotJson.find("\"id\": \"materialShaderContractSnapshotJson\""), std::string::npos);
        EXPECT_NE(snapshotJson.find("\"kind\": \"MaterialShaderContractSnapshotJson\""), std::string::npos);
        EXPECT_NE(snapshotJson.find("\"contentType\": \"application/json\""), std::string::npos);
        EXPECT_NE(snapshotJson.find("\"contentHash\": \"\""), std::string::npos);
        EXPECT_NE(snapshotJson.find("\"relativePath\": \"\""), std::string::npos);
        EXPECT_NE(snapshotJson.find("\"material\": {"), std::string::npos);
        EXPECT_NE(snapshotJson.find("\"resourceId\": 601"), std::string::npos);
        EXPECT_NE(snapshotJson.find("\"name\": \"ShaderContractMaterial\""), std::string::npos);
        EXPECT_NE(snapshotJson.find("\"shader\": {"), std::string::npos);
        EXPECT_NE(snapshotJson.find("\"assigned\": true"), std::string::npos);
        EXPECT_NE(snapshotJson.find("\"loaded\": true"), std::string::npos);
        EXPECT_NE(snapshotJson.find("\"contractValid\": true"), std::string::npos);
        EXPECT_NE(snapshotJson.find("\"resourceId\": 602"), std::string::npos);
        EXPECT_NE(snapshotJson.find("\"contractHash\": " +
                                    std::to_string(shader->GetRuntimeContractHash())),
                  std::string::npos);
        EXPECT_NE(snapshotJson.find("\"payloadHash\": " +
                                    std::to_string(shader->GetRuntimeContract().payloadHash)),
                  std::string::npos);
        EXPECT_NE(snapshotJson.find("\"contractKey\": "), std::string::npos);
        EXPECT_NE(snapshotJson.find("\"diagnosticMessage\": \"Shader runtime contract is valid.\""),
                  std::string::npos);
        EXPECT_FALSE(materialResource.SaveShaderContractSnapshotJson(nullptr));
        EXPECT_FALSE(materialResource.SaveShaderContractSnapshotJson(""));

        const auto suffix = std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count());
        const std::filesystem::path snapshotPath =
            std::filesystem::temp_directory_path() /
            ("RVX_MaterialShaderContractSnapshot_" + suffix + ".json");
        const std::string snapshotPathString = snapshotPath.string();
        ASSERT_TRUE(materialResource.SaveShaderContractSnapshotJson(snapshotPathString.c_str()));
        EXPECT_EQ(ReadTextFile(snapshotPath), snapshotJson);
        std::error_code removeError;
        std::filesystem::remove(snapshotPath, removeError);

        const std::vector<Resource::ResourceId> deps = materialResource.GetRequiredDependencies();
        EXPECT_NE(std::find(deps.begin(), deps.end(), shader.GetId()), deps.end());
    }

    TEST(MaterialSystemValidation, MaterialResourceReportsInvalidShaderRuntimeContract)
    {
        Resource::MaterialResource materialResource;
        materialResource.SetId(603);
        materialResource.SetName("InvalidShaderContractMaterial");

        Resource::ShaderHandle shader = CreateShaderResource(604, false);
        ASSERT_FALSE(shader->HasValidRuntimeContract());

        materialResource.SetShader(shader);

        EXPECT_TRUE(materialResource.HasShader());
        EXPECT_FALSE(materialResource.HasValidShaderRuntimeContract());
        EXPECT_EQ(materialResource.GetShaderRuntimeContractHash(), 0u);

        const Resource::MaterialShaderContractSnapshot snapshot =
            materialResource.GetShaderContractSnapshot();
        EXPECT_TRUE(snapshot.shaderAssigned);
        EXPECT_TRUE(snapshot.shaderLoaded);
        EXPECT_FALSE(snapshot.shaderContractValid);
        EXPECT_EQ(snapshot.shaderResourceId, shader.GetId());
        EXPECT_EQ(snapshot.shaderPayloadHash, shader->GetRuntimeContract().payloadHash);
        EXPECT_EQ(snapshot.shaderContractHash, 0u);
        EXPECT_NE(snapshot.diagnosticMessage.find("source path"), std::string::npos);

        const std::string snapshotJson = materialResource.ExportShaderContractSnapshotJson();
        EXPECT_NE(snapshotJson.find("\"schemaId\": \"RVX.Resource.MaterialShaderContractSnapshot\""),
                  std::string::npos);
        EXPECT_NE(snapshotJson.find("\"kind\": \"MaterialShaderContractSnapshotJson\""),
                  std::string::npos);
        EXPECT_NE(snapshotJson.find("\"name\": \"InvalidShaderContractMaterial\""), std::string::npos);
        EXPECT_NE(snapshotJson.find("\"assigned\": true"), std::string::npos);
        EXPECT_NE(snapshotJson.find("\"loaded\": true"), std::string::npos);
        EXPECT_NE(snapshotJson.find("\"contractValid\": false"), std::string::npos);
        EXPECT_NE(snapshotJson.find("\"resourceId\": 604"), std::string::npos);
        EXPECT_NE(snapshotJson.find("\"contractHash\": 0"), std::string::npos);
        EXPECT_NE(snapshotJson.find("\"payloadHash\": " +
                                    std::to_string(shader->GetRuntimeContract().payloadHash)),
                  std::string::npos);
        EXPECT_NE(snapshotJson.find("\"contractKey\": \"\""), std::string::npos);
        EXPECT_NE(snapshotJson.find("source path"), std::string::npos);
    }

    TEST(MaterialSystemValidation, MaterialResourceShaderContractSnapshotJsonReportsMissingShader)
    {
        Resource::MaterialResource materialResource;
        materialResource.SetId(605);
        materialResource.SetName("MissingShaderMaterial");
        materialResource.SetMaterialData(std::make_shared<Material>());

        EXPECT_FALSE(materialResource.HasShader());
        EXPECT_FALSE(materialResource.HasValidShaderRuntimeContract());
        EXPECT_EQ(materialResource.GetShaderRuntimeContractHash(), 0u);

        const Resource::MaterialShaderContractSnapshot snapshot =
            materialResource.GetShaderContractSnapshot();
        EXPECT_FALSE(snapshot.shaderAssigned);
        EXPECT_FALSE(snapshot.shaderLoaded);
        EXPECT_FALSE(snapshot.shaderContractValid);
        EXPECT_EQ(snapshot.shaderResourceId, Resource::InvalidResourceId);
        EXPECT_EQ(snapshot.shaderContractHash, 0u);
        EXPECT_EQ(snapshot.shaderPayloadHash, 0u);
        EXPECT_TRUE(snapshot.shaderContractKey.empty());
        EXPECT_EQ(snapshot.diagnosticMessage, "Material has no shader assigned.");

        const std::string snapshotJson = materialResource.ExportShaderContractSnapshotJson();
        EXPECT_NE(snapshotJson.find("\"schemaId\": \"RVX.Resource.MaterialShaderContractSnapshot\""),
                  std::string::npos);
        EXPECT_NE(snapshotJson.find("\"id\": \"materialShaderContractSnapshotJson\""),
                  std::string::npos);
        EXPECT_NE(snapshotJson.find("\"kind\": \"MaterialShaderContractSnapshotJson\""),
                  std::string::npos);
        EXPECT_NE(snapshotJson.find("\"resourceId\": 605"), std::string::npos);
        EXPECT_NE(snapshotJson.find("\"name\": \"MissingShaderMaterial\""), std::string::npos);
        EXPECT_NE(snapshotJson.find("\"assigned\": false"), std::string::npos);
        EXPECT_NE(snapshotJson.find("\"loaded\": false"), std::string::npos);
        EXPECT_NE(snapshotJson.find("\"contractValid\": false"), std::string::npos);
        EXPECT_NE(snapshotJson.find("\"resourceId\": " +
                                    std::to_string(Resource::InvalidResourceId)),
                  std::string::npos);
        EXPECT_NE(snapshotJson.find("\"contractHash\": 0"), std::string::npos);
        EXPECT_NE(snapshotJson.find("\"payloadHash\": 0"), std::string::npos);
        EXPECT_NE(snapshotJson.find("\"contractKey\": \"\""), std::string::npos);
        EXPECT_NE(snapshotJson.find("\"diagnosticMessage\": \"Material has no shader assigned.\""),
                  std::string::npos);
    }
} // namespace
