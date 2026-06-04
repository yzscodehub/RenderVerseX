#include "Core/Core.h"
#include "Render/GPUResourceManager.h"
#include "Render/Graph/ResourceViewCache.h"
#include "Render/Material/MaterialClassification.h"
#include "Render/Material/MaterialSystem.h"
#include "Resource/Types/MaterialResource.h"
#include "Resource/Types/TextureResource.h"
#include "RHI/RHICommandContext.h"
#include "RHI/RHIDevice.h"
#include "RHI/RHIUpload.h"
#include "Scene/Material.h"

#include <gtest/gtest.h>

#include <memory>
#include <vector>

using namespace RVX;

namespace
{
    class FakeBuffer final : public RHIBuffer
    {
    public:
        explicit FakeBuffer(const RHIBufferDesc& desc)
            : m_desc(desc)
            , m_storage(static_cast<size_t>(desc.size))
        {
        }

        uint64 GetSize() const override { return m_desc.size; }
        RHIBufferUsage GetUsage() const override { return m_desc.usage; }
        RHIMemoryType GetMemoryType() const override { return m_desc.memoryType; }
        uint32 GetStride() const override { return m_desc.stride; }

        void* Map() override { return m_storage.empty() ? nullptr : m_storage.data(); }
        void Unmap() override {}

        const std::vector<uint8>& GetStorage() const { return m_storage; }

    private:
        RHIBufferDesc m_desc;
        std::vector<uint8> m_storage;
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
            : m_texture(texture)
            , m_format(desc.format == RHIFormat::Unknown && texture ? texture->GetFormat() : desc.format)
            , m_range(desc.subresourceRange)
        {
        }

        RHITexture* GetTexture() const override { return m_texture; }
        RHIFormat GetFormat() const override { return m_format; }
        const RHISubresourceRange& GetSubresourceRange() const override { return m_range; }

    private:
        RHITexture* m_texture = nullptr;
        RHIFormat m_format = RHIFormat::Unknown;
        RHISubresourceRange m_range;
    };

    class FakeSampler final : public RHISampler
    {
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
        RHIBufferRef CreateBuffer(const RHIBufferDesc& desc) override
        {
            ++createdBufferCount;
            return RHIBufferRef(new FakeBuffer(desc));
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

        RHISamplerRef CreateSampler(const RHISamplerDesc&) override
        {
            ++createdSamplerCount;
            return RHISamplerRef(new FakeSampler());
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
            return RHIDescriptorSetRef(new FakeDescriptorSet(desc));
        }

        RHIQueryPoolRef CreateQueryPool(const RHIQueryPoolDesc&) override { return nullptr; }

        RHICommandContextRef CreateCommandContext(RHICommandQueueType type) override
        {
            ++createdCommandContextCount;
            lastCommandQueueType = type;
            auto context = RHICommandContextRef(new FakeCommandContext());
            lastCommandContext = static_cast<FakeCommandContext*>(context.Get());
            return context;
        }

        uint64 SubmitCommandContext(RHICommandContext*, RHIFence* signalFence = nullptr) override
        {
            ++submittedCommandContextCount;
            if (!signalFence)
                return 0;

            const uint64 value = m_nextFenceValue++;
            signalFence->Signal(value);
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
            for (const RHIFenceRef& fence : retainedFences)
            {
                if (fence)
                    fence->Signal(UINT64_MAX);
            }
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
        RHIBackendType GetBackendType() const override { return RHIBackendType::None; }

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
        RHICommandQueueType lastCommandQueueType = RHICommandQueueType::Graphics;
        FakeCommandContext* lastCommandContext = nullptr;
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

    void ConfigureMaterialWithAlbedo(Resource::MaterialResource& materialResource,
                                     const Resource::TextureHandle& albedo)
    {
        materialResource.SetId(201);
        materialResource.SetName("TestMaterialResource");
        materialResource.SetMaterialData(std::make_shared<Material>());
        materialResource.SetTexture("albedo", albedo);
    }

    const RHIDescriptorBinding* FindBinding(const RHIDescriptorSet* descriptorSet, uint32 binding)
    {
        const auto* fakeSet = static_cast<const FakeDescriptorSet*>(descriptorSet);
        return fakeSet ? fakeSet->FindBinding(binding) : nullptr;
    }

    TEST(MaterialSystemValidation, ClassifiesMaterialAlphaModes)
    {
        auto opaque = std::make_shared<Material>();
        opaque->SetAlphaMode(Material::AlphaMode::Opaque);
        EXPECT_EQ(MaterialRenderMode::Opaque,
                  ClassifyMaterialRenderMode(opaque.get()));

        auto masked = std::make_shared<Material>();
        masked->SetAlphaMode(Material::AlphaMode::Mask);
        EXPECT_EQ(MaterialRenderMode::Masked,
                  ClassifyMaterialRenderMode(masked.get()));

        auto transparent = std::make_shared<Material>();
        transparent->SetAlphaMode(Material::AlphaMode::Blend);
        EXPECT_EQ(MaterialRenderMode::Transparent,
                  ClassifyMaterialRenderMode(transparent.get()));

        EXPECT_EQ(MaterialPipelineVariant::Opaque,
                  GetPipelineVariantForRenderMode(MaterialRenderMode::Opaque));
        EXPECT_EQ(MaterialPipelineVariant::Masked,
                  GetPipelineVariantForRenderMode(MaterialRenderMode::Masked));
        EXPECT_EQ(MaterialPipelineVariant::Transparent,
                  GetPipelineVariantForRenderMode(MaterialRenderMode::Transparent));
    }

    TEST(MaterialSystemValidation, MaterialSetUsesResidentTextureViewForAlbedo)
    {
        FakeDevice device;
        GPUResourceManager gpuResources;
        gpuResources.Initialize(&device);

        ResourceViewCache viewCache;
        viewCache.Initialize(&device);

        FakeDescriptorSetLayout materialSetLayout;
        MaterialSystem materialSystem;
        ASSERT_TRUE(materialSystem.Initialize(&device, &gpuResources, &materialSetLayout));

        Resource::TextureHandle albedo = CreateTextureResource(301);
        gpuResources.UploadImmediate(albedo.Get());
        ASSERT_EQ(GPUResourceState::GPUReady, gpuResources.GetResourceState(albedo.GetId()));

        Resource::MaterialResource materialResource;
        ConfigureMaterialWithAlbedo(materialResource, albedo);
        RHIDescriptorSet* descriptorSet = materialSystem.GetOrCreateMaterialSet(&materialResource, &viewCache);

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

    TEST(MaterialSystemValidation, MaterialSetFallsBackWhenTextureIsNotGPUReady)
    {
        FakeDevice device;
        GPUResourceManager gpuResources;
        gpuResources.Initialize(&device);

        ResourceViewCache viewCache;
        viewCache.Initialize(&device);

        FakeDescriptorSetLayout materialSetLayout;
        MaterialSystem materialSystem;
        ASSERT_TRUE(materialSystem.Initialize(&device, &gpuResources, &materialSetLayout));

        Resource::TextureHandle albedo = CreateTextureResource(302);
        ASSERT_NE(GPUResourceState::GPUReady, gpuResources.GetResourceState(albedo.GetId()));

        Resource::MaterialResource materialResource;
        ConfigureMaterialWithAlbedo(materialResource, albedo);
        RHIDescriptorSet* descriptorSet = materialSystem.GetOrCreateMaterialSet(&materialResource, &viewCache);

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
        GPUResourceManager gpuResources;
        gpuResources.Initialize(&device);

        ResourceViewCache viewCache;
        viewCache.Initialize(&device);

        FakeDescriptorSetLayout materialSetLayout;
        MaterialSystem materialSystem;
        ASSERT_TRUE(materialSystem.Initialize(&device, &gpuResources, &materialSetLayout));

        Resource::TextureHandle albedo = CreateTextureResource(
            303,
            Resource::TextureFormat::RGBA8,
            {255, 255, 255});
        gpuResources.UploadImmediate(albedo.Get());
        ASSERT_EQ(GPUResourceState::Failed, gpuResources.GetResourceState(albedo.GetId()));

        Resource::MaterialResource materialResource;
        ConfigureMaterialWithAlbedo(materialResource, albedo);
        RHIDescriptorSet* descriptorSet = materialSystem.GetOrCreateMaterialSet(&materialResource, &viewCache);

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
} // namespace
