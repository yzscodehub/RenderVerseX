#include "Core/Core.h"
#include "Render/GPUResourceManager.h"
#include "Render/Graph/ResourceViewCache.h"
#include "Render/Material/MaterialBinder.h"
#include "Render/Material/MaterialClassification.h"
#include "Render/Material/MaterialSystem.h"
#include "Render/Material/MaterialTemplate.h"
#include "Resource/Types/MaterialResource.h"
#include "Resource/Types/TextureResource.h"
#include "RHI/RHICommandContext.h"
#include "RHI/RHIDevice.h"
#include "RHI/RHIUpload.h"
#include "Scene/Material.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace RVX;

namespace
{
    class FakeBuffer final : public RHIBuffer
    {
    public:
        explicit FakeBuffer(const RHIBufferDesc& desc, bool mapSucceeds = true)
            : m_desc(desc)
            , m_storage(static_cast<size_t>(desc.size))
            , m_mapSucceeds(mapSucceeds)
        {
        }

        uint64 GetSize() const override { return m_desc.size; }
        RHIBufferUsage GetUsage() const override { return m_desc.usage; }
        RHIMemoryType GetMemoryType() const override { return m_desc.memoryType; }
        uint32 GetStride() const override { return m_desc.stride; }

        void* Map() override
        {
            if (!m_mapSucceeds)
                return nullptr;

            return m_storage.empty() ? nullptr : m_storage.data();
        }
        void Unmap() override {}

        const std::vector<uint8>& GetStorage() const { return m_storage; }

    private:
        RHIBufferDesc m_desc;
        std::vector<uint8> m_storage;
        bool m_mapSucceeds = true;
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
            m_entries.push_back({7, RHIBindingType::SampledTexture, RHIShaderStage::All, 1, false});
            m_entries.push_back({8, RHIBindingType::SampledTexture, RHIShaderStage::All, 1, false});
            m_entries.push_back({9, RHIBindingType::SampledTexture, RHIShaderStage::All, 1, false});
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
            if (failBufferCreation)
                return nullptr;

            auto buffer = RHIBufferRef(new FakeBuffer(desc, bufferMapSucceeds));
            lastCreatedBuffer = static_cast<FakeBuffer*>(buffer.Get());
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
            if (failDescriptorSetCreation)
                return nullptr;

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
        bool failBufferCreation = false;
        bool failDescriptorSetCreation = false;
        bool bufferMapSucceeds = true;
        RHICommandQueueType lastCommandQueueType = RHICommandQueueType::Graphics;
        FakeCommandContext* lastCommandContext = nullptr;
        FakeBuffer* lastCreatedBuffer = nullptr;
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

        const MaterialGPUConstants constants = MaterialBinder::ConvertToGPU(material);

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

        MaterialGPUConstants constants = MaterialBinder::ConvertToGPU(material);
        EXPECT_EQ(constants.textureFlags, 0u);

        material.SetBaseColorTexture(TextureInfo("base-color.png"));
        material.SetNormalTexture(TextureInfo("normal.png"));
        material.SetMetallicRoughnessTexture(TextureInfo("mr.png"));
        material.SetOcclusionTexture(TextureInfo("ao.png"));
        material.SetEmissiveTexture(TextureInfo("emissive.png"));

        constants = MaterialBinder::ConvertToGPU(material);
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

        constants = MaterialBinder::ConvertToGPU(material);
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
        binder.Bind(ctx, material);

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
            binder.Bind(ctx, material);

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

        const MaterialBindingResult result = materialSystem.PrepareMaterialBinding(nullptr, nullptr);

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

        GPUResourceManager gpuResources;
        gpuResources.Initialize(&device);

        FakeDescriptorSetLayout materialLayout;
        MaterialSystem materialSystem;
        ASSERT_TRUE(materialSystem.Initialize(&device, &gpuResources, &materialLayout));

        const MaterialBindingResult result = materialSystem.PrepareMaterialBinding(nullptr, nullptr);

        EXPECT_EQ(MaterialBindingStatus::Error, result.status);
        EXPECT_FALSE(result.IsDrawable());
        EXPECT_FALSE(result.constantsUpdated);
        EXPECT_TRUE(result.usedFallback);
        EXPECT_TRUE(Contains(result.message, "map material constant buffer"));
        EXPECT_EQ(MaterialBindingStatus::Error, materialSystem.GetLastBindingResult().status);

        materialSystem.Shutdown();
        gpuResources.Shutdown();
    }

    TEST(MaterialSystemValidation, MaterialBindingDescriptorFailureUsesExplicitDefaultFallback)
    {
        FakeDevice device;
        GPUResourceManager gpuResources;
        gpuResources.Initialize(&device);

        FakeDescriptorSetLayout materialLayout;
        MaterialSystem materialSystem;
        ASSERT_TRUE(materialSystem.Initialize(&device, &gpuResources, &materialLayout));

        device.failDescriptorSetCreation = true;
        const MaterialBindingResult result = materialSystem.PrepareMaterialBinding(nullptr, nullptr);

        EXPECT_EQ(MaterialBindingStatus::Fallback, result.status);
        EXPECT_TRUE(result.IsDrawable());
        EXPECT_TRUE(result.constantsUpdated);
        EXPECT_TRUE(result.usedFallback);
        EXPECT_EQ(materialSystem.GetDefaultMaterialSet(), result.descriptorSet);
        EXPECT_TRUE(Contains(result.message, "default material set"));

        materialSystem.Shutdown();
        gpuResources.Shutdown();
    }

    TEST(MaterialSystemValidation, MaterialBindingNullMaterialUsesExplicitFallback)
    {
        FakeDevice device;
        GPUResourceManager gpuResources;
        gpuResources.Initialize(&device);

        FakeDescriptorSetLayout materialLayout;
        MaterialSystem materialSystem;
        ASSERT_TRUE(materialSystem.Initialize(&device, &gpuResources, &materialLayout));

        const MaterialBindingResult result = materialSystem.PrepareMaterialBinding(nullptr, nullptr);

        EXPECT_EQ(MaterialBindingStatus::Fallback, result.status);
        EXPECT_TRUE(result.IsDrawable());
        EXPECT_TRUE(result.usedFallback);
        EXPECT_TRUE(Contains(result.message, "fallback"));

        materialSystem.Shutdown();
        gpuResources.Shutdown();
    }

    TEST(MaterialSystemValidation, MaterialBindingResidentTexturesAreReady)
    {
        FakeDevice device;
        GPUResourceManager gpuResources;
        gpuResources.Initialize(&device);

        ResourceViewCache viewCache;
        viewCache.Initialize(&device);

        FakeDescriptorSetLayout materialLayout;
        MaterialSystem materialSystem;
        ASSERT_TRUE(materialSystem.Initialize(&device, &gpuResources, &materialLayout));

        auto texture = CreateTextureResource(306);
        Resource::MaterialResource materialResource;
        ConfigureMaterialWithAllTextures(materialResource, texture);

        gpuResources.UploadImmediate(texture.Get());
        ASSERT_TRUE(gpuResources.IsGPUReady(texture.GetId()));

        const MaterialBindingResult result = materialSystem.PrepareMaterialBinding(&materialResource, &viewCache);

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
        EXPECT_EQ("FullyTexturedMaterialResource", result.materialName);
        EXPECT_EQ(expectedFlags, materialSystem.GetLastBindingResult().textureFlags);
        EXPECT_EQ("FullyTexturedMaterialResource", materialSystem.GetLastBindingResult().materialName);
        EXPECT_TRUE(Contains(result.message, "ready"));

        materialSystem.Shutdown();
        viewCache.Shutdown();
        gpuResources.Shutdown();
    }

    TEST(MaterialSystemValidation, MaterialBindingDefaultOptionsKeepReadyNormalTextureEnabled)
    {
        FakeDevice device;
        GPUResourceManager gpuResources;
        gpuResources.Initialize(&device);

        ResourceViewCache viewCache;
        viewCache.Initialize(&device);

        FakeDescriptorSetLayout materialLayout;
        MaterialSystem materialSystem;
        ASSERT_TRUE(materialSystem.Initialize(&device, &gpuResources, &materialLayout));

        auto normalTexture = CreateTextureResource(320);
        Resource::MaterialResource materialResource;
        materialResource.SetId(220);
        materialResource.SetName("NormalOnlyMaterialResource");
        materialResource.SetMaterialData(std::make_shared<Material>("NormalOnlyMaterialResource"));
        materialResource.SetTexture("normal", normalTexture);

        gpuResources.UploadImmediate(normalTexture.Get());
        ASSERT_TRUE(gpuResources.IsGPUReady(normalTexture.GetId()));

        const MaterialBindingResult result = materialSystem.PrepareMaterialBinding(&materialResource, &viewCache);
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
        GPUResourceManager gpuResources;
        gpuResources.Initialize(&device);

        ResourceViewCache viewCache;
        viewCache.Initialize(&device);

        FakeDescriptorSetLayout materialLayout;
        MaterialSystem materialSystem;
        ASSERT_TRUE(materialSystem.Initialize(&device, &gpuResources, &materialLayout));

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
            materialSystem.PrepareMaterialBinding(&materialResource, &viewCache, options);
        const uint32 normalFlag = static_cast<uint32>(MaterialTextureFlags::HasNormal);

        EXPECT_EQ(MaterialBindingStatus::Fallback, result.status);
        EXPECT_TRUE(result.IsDrawable());
        EXPECT_TRUE(result.usedFallback);
        EXPECT_EQ(0u, result.textureFlags & normalFlag);
        EXPECT_EQ(normalFlag, result.fallbackTextureFlags & normalFlag);
        EXPECT_TRUE(Contains(result.message, "normal map disabled"));
        ASSERT_NE(device.lastCreatedBuffer, nullptr);
        const MaterialGPUConstants constants = ReadMaterialConstants(*device.lastCreatedBuffer);
        EXPECT_EQ(0u, constants.textureFlags & normalFlag);

        materialSystem.Shutdown();
        viewCache.Shutdown();
        gpuResources.Shutdown();
    }

    TEST(MaterialSystemValidation, MaterialBindingDoesNotReportFallbackForOmittedNormalTextureWhenNormalMapsDisabled)
    {
        FakeDevice device;
        GPUResourceManager gpuResources;
        gpuResources.Initialize(&device);

        ResourceViewCache viewCache;
        viewCache.Initialize(&device);

        FakeDescriptorSetLayout materialLayout;
        MaterialSystem materialSystem;
        ASSERT_TRUE(materialSystem.Initialize(&device, &gpuResources, &materialLayout));

        Resource::MaterialResource materialResource;
        materialResource.SetId(222);
        materialResource.SetName("OmittedNormalMaterialResource");
        materialResource.SetMaterialData(std::make_shared<Material>("OmittedNormalMaterialResource"));

        MaterialBindingOptions options;
        options.allowNormalMap = false;
        const MaterialBindingResult result =
            materialSystem.PrepareMaterialBinding(&materialResource, &viewCache, options);
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

    TEST(MaterialSystemValidation, MaterialBindingDefaultFallbackTextureDoesNotSetTextureFlag)
    {
        FakeDevice device;
        GPUResourceManager gpuResources;
        gpuResources.Initialize(&device);

        ResourceViewCache viewCache;
        viewCache.Initialize(&device);

        FakeDescriptorSetLayout materialLayout;
        MaterialSystem materialSystem;
        ASSERT_TRUE(materialSystem.Initialize(&device, &gpuResources, &materialLayout));

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
            materialSystem.PrepareMaterialBinding(&fallbackMaterial, &viewCache);

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
            materialSystem.PrepareMaterialBinding(&omittedTextureMaterial, &viewCache);

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
        GPUResourceManager gpuResources;
        gpuResources.Initialize(&device);

        ResourceViewCache viewCache;
        viewCache.Initialize(&device);

        FakeDescriptorSetLayout materialLayout;
        MaterialSystem materialSystem;
        ASSERT_TRUE(materialSystem.Initialize(&device, &gpuResources, &materialLayout));

        auto texture = CreateTextureResource(307);
        Resource::MaterialResource materialResource;
        ConfigureMaterialWithAlbedo(materialResource, texture);

        const MaterialBindingResult result = materialSystem.PrepareMaterialBinding(&materialResource, &viewCache);

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
            GPUResourceManager gpuResources;
            gpuResources.Initialize(&device);

            FakeDescriptorSetLayout materialLayout;
            MaterialSystem materialSystem;
            ASSERT_TRUE(materialSystem.Initialize(&device, &gpuResources, &materialLayout));

            EXPECT_TRUE(materialSystem.UpdateMaterialConstants(nullptr, nullptr));
            EXPECT_EQ(MaterialBindingStatus::Fallback, materialSystem.GetLastBindingResult().status);
            EXPECT_TRUE(materialSystem.GetLastBindingResult().constantsUpdated);

            materialSystem.Shutdown();
            gpuResources.Shutdown();
        }

        {
            FakeDevice device;
            device.bufferMapSucceeds = false;

            GPUResourceManager gpuResources;
            gpuResources.Initialize(&device);

            FakeDescriptorSetLayout materialLayout;
            MaterialSystem materialSystem;
            ASSERT_TRUE(materialSystem.Initialize(&device, &gpuResources, &materialLayout));

            EXPECT_FALSE(materialSystem.UpdateMaterialConstants(nullptr, nullptr));
            EXPECT_EQ(MaterialBindingStatus::Error, materialSystem.GetLastBindingResult().status);
            EXPECT_FALSE(materialSystem.GetLastBindingResult().constantsUpdated);

            materialSystem.Shutdown();
            gpuResources.Shutdown();
        }

        {
            MaterialSystem materialSystem;

            EXPECT_FALSE(materialSystem.UpdateMaterialConstants(nullptr, nullptr));
            EXPECT_EQ(MaterialBindingStatus::NotInitialized, materialSystem.GetLastBindingResult().status);
        }
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

    TEST(MaterialSystemValidation, MaterialSystemCreatesExplicitMipFilteredMaterialSampler)
    {
        FakeDevice device;
        GPUResourceManager gpuResources;
        gpuResources.Initialize(&device);

        FakeDescriptorSetLayout materialSetLayout;
        MaterialSystem materialSystem;
        ASSERT_TRUE(materialSystem.Initialize(&device, &gpuResources, &materialSetLayout));

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

    TEST(MaterialSystemValidation, DefaultMaterialSetBindsIBLFallbackViews)
    {
        FakeDevice device;
        GPUResourceManager gpuResources;
        gpuResources.Initialize(&device);

        FakeDescriptorSetLayout materialSetLayout;
        MaterialSystem materialSystem;
        ASSERT_TRUE(materialSystem.Initialize(&device, &gpuResources, &materialSetLayout));

        RHIDescriptorSet* descriptorSet = materialSystem.GetDefaultMaterialSet();
        ASSERT_NE(nullptr, descriptorSet);

        const RHIDescriptorBinding* irradianceBinding = FindBinding(descriptorSet, 7);
        const RHIDescriptorBinding* prefilteredBinding = FindBinding(descriptorSet, 8);
        const RHIDescriptorBinding* brdfBinding = FindBinding(descriptorSet, 9);
        ASSERT_NE(nullptr, irradianceBinding);
        ASSERT_NE(nullptr, prefilteredBinding);
        ASSERT_NE(nullptr, brdfBinding);
        ASSERT_NE(nullptr, irradianceBinding->textureView);
        ASSERT_NE(nullptr, prefilteredBinding->textureView);
        ASSERT_NE(nullptr, brdfBinding->textureView);

        EXPECT_EQ(RHITextureDimension::TextureCube, irradianceBinding->textureView->GetTexture()->GetDimension());
        EXPECT_EQ(RHITextureDimension::TextureCube, prefilteredBinding->textureView->GetTexture()->GetDimension());
        EXPECT_EQ(RHITextureDimension::Texture2D, brdfBinding->textureView->GetTexture()->GetDimension());
        EXPECT_EQ(irradianceBinding->textureView, prefilteredBinding->textureView);

        materialSystem.Shutdown();
        gpuResources.Shutdown();
    }

    TEST(MaterialSystemValidation, MaterialSetUsesResidentTextureIBLViews)
    {
        FakeDevice device;
        GPUResourceManager gpuResources;
        gpuResources.Initialize(&device);

        ResourceViewCache viewCache;
        viewCache.Initialize(&device);

        FakeDescriptorSetLayout materialSetLayout;
        MaterialSystem materialSystem;
        ASSERT_TRUE(materialSystem.Initialize(&device, &gpuResources, &materialSetLayout));

        Resource::TextureHandle irradiance = CreateIBLCubemapResource(401);
        Resource::TextureHandle prefiltered = CreateIBLCubemapResource(402, 2);
        Resource::TextureHandle brdfLUT = CreateIBLBRDFLUTResource(403);
        gpuResources.UploadImmediate(irradiance.Get());
        gpuResources.UploadImmediate(prefiltered.Get());
        gpuResources.UploadImmediate(brdfLUT.Get());
        ASSERT_TRUE(gpuResources.IsGPUReady(irradiance.GetId()));
        ASSERT_TRUE(gpuResources.IsGPUReady(prefiltered.GetId()));
        ASSERT_TRUE(gpuResources.IsGPUReady(brdfLUT.GetId()));

        MaterialSystem::EnvironmentIBLResources iblResources;
        iblResources.irradianceMap = irradiance.Get();
        iblResources.prefilteredMap = prefiltered.Get();
        iblResources.brdfLUT = brdfLUT.Get();
        iblResources.prefilteredMipLevels = 2;
        iblResources.textureIBLEnabled = true;
        materialSystem.SetEnvironmentIBLResources(iblResources);

        Resource::MaterialResource materialResource;
        materialResource.SetId(501);
        materialResource.SetName("IBLReadyMaterial");
        materialResource.SetMaterialData(std::make_shared<Material>());

        RHIDescriptorSet* descriptorSet = materialSystem.GetOrCreateMaterialSet(&materialResource, &viewCache);
        ASSERT_NE(nullptr, descriptorSet);

        const RHIDescriptorBinding* irradianceBinding = FindBinding(descriptorSet, 7);
        const RHIDescriptorBinding* prefilteredBinding = FindBinding(descriptorSet, 8);
        const RHIDescriptorBinding* brdfBinding = FindBinding(descriptorSet, 9);
        ASSERT_NE(nullptr, irradianceBinding);
        ASSERT_NE(nullptr, prefilteredBinding);
        ASSERT_NE(nullptr, brdfBinding);
        ASSERT_NE(nullptr, irradianceBinding->textureView);
        ASSERT_NE(nullptr, prefilteredBinding->textureView);
        ASSERT_NE(nullptr, brdfBinding->textureView);

        EXPECT_EQ(gpuResources.GetTexture(irradiance.GetId()), irradianceBinding->textureView->GetTexture());
        EXPECT_EQ(gpuResources.GetTexture(prefiltered.GetId()), prefilteredBinding->textureView->GetTexture());
        EXPECT_EQ(gpuResources.GetTexture(brdfLUT.GetId()), brdfBinding->textureView->GetTexture());

        materialSystem.Shutdown();
        viewCache.Shutdown();
        gpuResources.Shutdown();
    }

    TEST(MaterialSystemValidation, MaterialSetFallsBackWhenTextureIBLIsNotReady)
    {
        FakeDevice device;
        GPUResourceManager gpuResources;
        gpuResources.Initialize(&device);

        ResourceViewCache viewCache;
        viewCache.Initialize(&device);

        FakeDescriptorSetLayout materialSetLayout;
        MaterialSystem materialSystem;
        ASSERT_TRUE(materialSystem.Initialize(&device, &gpuResources, &materialSetLayout));

        Resource::TextureHandle irradiance = CreateIBLCubemapResource(411);
        Resource::TextureHandle prefiltered = CreateIBLCubemapResource(412, 2);
        Resource::TextureHandle brdfLUT = CreateIBLBRDFLUTResource(413);

        MaterialSystem::EnvironmentIBLResources iblResources;
        iblResources.irradianceMap = irradiance.Get();
        iblResources.prefilteredMap = prefiltered.Get();
        iblResources.brdfLUT = brdfLUT.Get();
        iblResources.prefilteredMipLevels = 2;
        iblResources.textureIBLEnabled = true;
        materialSystem.SetEnvironmentIBLResources(iblResources);

        Resource::MaterialResource materialResource;
        materialResource.SetId(502);
        materialResource.SetName("IBLFallbackMaterial");
        materialResource.SetMaterialData(std::make_shared<Material>());

        const MaterialBindingResult result = materialSystem.PrepareMaterialBinding(&materialResource, &viewCache);
        EXPECT_EQ(MaterialBindingStatus::Fallback, result.status);
        EXPECT_TRUE(result.usedFallback);
        ASSERT_NE(nullptr, result.descriptorSet);

        const RHIDescriptorBinding* defaultIrradiance = FindBinding(materialSystem.GetDefaultMaterialSet(), 7);
        const RHIDescriptorBinding* boundIrradiance = FindBinding(result.descriptorSet, 7);
        ASSERT_NE(nullptr, defaultIrradiance);
        ASSERT_NE(nullptr, boundIrradiance);
        EXPECT_EQ(defaultIrradiance->textureView, boundIrradiance->textureView);

        materialSystem.Shutdown();
        viewCache.Shutdown();
        gpuResources.Shutdown();
    }

    TEST(MaterialSystemValidation, EnvironmentIBLResourceChangeInvalidatesMaterialDescriptorCache)
    {
        FakeDevice device;
        GPUResourceManager gpuResources;
        gpuResources.Initialize(&device);

        ResourceViewCache viewCache;
        viewCache.Initialize(&device);

        FakeDescriptorSetLayout materialSetLayout;
        MaterialSystem materialSystem;
        ASSERT_TRUE(materialSystem.Initialize(&device, &gpuResources, &materialSetLayout));

        auto upload = [&gpuResources](const Resource::TextureHandle& texture)
        {
            gpuResources.UploadImmediate(texture.Get());
            ASSERT_TRUE(gpuResources.IsGPUReady(texture.GetId()));
        };

        Resource::TextureHandle irradianceA = CreateIBLCubemapResource(421);
        Resource::TextureHandle prefilteredA = CreateIBLCubemapResource(422);
        Resource::TextureHandle brdfA = CreateIBLBRDFLUTResource(423);
        upload(irradianceA);
        upload(prefilteredA);
        upload(brdfA);

        Resource::TextureHandle irradianceB = CreateIBLCubemapResource(431);
        Resource::TextureHandle prefilteredB = CreateIBLCubemapResource(432);
        Resource::TextureHandle brdfB = CreateIBLBRDFLUTResource(433);
        upload(irradianceB);
        upload(prefilteredB);
        upload(brdfB);

        Resource::MaterialResource materialResource;
        materialResource.SetId(503);
        materialResource.SetName("IBLCacheMaterial");
        materialResource.SetMaterialData(std::make_shared<Material>());

        MaterialSystem::EnvironmentIBLResources firstIBL;
        firstIBL.irradianceMap = irradianceA.Get();
        firstIBL.prefilteredMap = prefilteredA.Get();
        firstIBL.brdfLUT = brdfA.Get();
        firstIBL.textureIBLEnabled = true;
        materialSystem.SetEnvironmentIBLResources(firstIBL);
        RHIDescriptorSet* firstSet = materialSystem.GetOrCreateMaterialSet(&materialResource, &viewCache);
        ASSERT_NE(nullptr, firstSet);
        const uint32 descriptorSetCountAfterFirstIBL = device.createdDescriptorSetCount;

        MaterialSystem::EnvironmentIBLResources secondIBL;
        secondIBL.irradianceMap = irradianceB.Get();
        secondIBL.prefilteredMap = prefilteredB.Get();
        secondIBL.brdfLUT = brdfB.Get();
        secondIBL.textureIBLEnabled = true;
        materialSystem.SetEnvironmentIBLResources(secondIBL);
        RHIDescriptorSet* secondSet = materialSystem.GetOrCreateMaterialSet(&materialResource, &viewCache);
        ASSERT_NE(nullptr, secondSet);

        EXPECT_GT(device.createdDescriptorSetCount, descriptorSetCountAfterFirstIBL);
        const RHIDescriptorBinding* irradianceBinding = FindBinding(secondSet, 7);
        ASSERT_NE(nullptr, irradianceBinding);
        ASSERT_NE(nullptr, irradianceBinding->textureView);
        EXPECT_EQ(gpuResources.GetTexture(irradianceB.GetId()), irradianceBinding->textureView->GetTexture());

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
