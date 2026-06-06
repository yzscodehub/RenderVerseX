#include "Core/Log.h"
#include "Core/MathTypes.h"
#include "Render/Material/MaterialClassification.h"
#include "RHI/RHI.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

#define private public
#include "Render/PipelineCache.h"
#undef private

#include "Render/GPUResourceManager.h"
#include "Render/Material/MaterialSystem.h"
#include "Render/Passes/OpaquePass.h"
#include "Render/Passes/TransparentPass.h"
#include "Render/Renderer/RenderScene.h"
#include "Render/Renderer/ViewData.h"
#include "Resource/Types/MaterialResource.h"
#include "Resource/Types/MeshResource.h"
#include "Resource/Types/TextureResource.h"
#include "RHI/RHICommandContext.h"
#include "RHI/RHIDevice.h"
#include "RHI/RHIUpload.h"
#include "Scene/Mesh.h"

#include <gtest/gtest.h>

using namespace RVX;

namespace
{
    namespace fs = std::filesystem;

    fs::path FindShaderDirectory()
    {
        fs::path cursor = fs::current_path();
        for (uint32 i = 0; i < 8; ++i)
        {
            fs::path candidate = cursor / "Render" / "Shaders";
            if (fs::exists(candidate / "DefaultLit.hlsl"))
            {
                return candidate;
            }

            if (!cursor.has_parent_path() || cursor == cursor.parent_path())
                break;

            cursor = cursor.parent_path();
        }

        return {};
    }

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

        void* Map() override { return !m_mapSucceeds || m_storage.empty() ? nullptr : m_storage.data(); }
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

    class FakeShader final : public RHIShader
    {
    public:
        explicit FakeShader(const RHIShaderDesc& desc)
            : m_stage(desc.stage)
        {
            if (desc.bytecode && desc.bytecodeSize > 0)
            {
                const auto* bytes = static_cast<const uint8*>(desc.bytecode);
                m_bytecode.assign(bytes, bytes + desc.bytecodeSize);
            }
        }

        RHIShaderStage GetStage() const override { return m_stage; }
        const std::vector<uint8>& GetBytecode() const override { return m_bytecode; }

    private:
        RHIShaderStage m_stage = RHIShaderStage::None;
        std::vector<uint8> m_bytecode;
    };

    class FakeDescriptorSetLayout final : public RHIDescriptorSetLayout
    {
    public:
        explicit FakeDescriptorSetLayout(const RHIDescriptorSetLayoutDesc& desc)
            : m_entries(desc.entries)
        {
        }

        const std::vector<RHIBindingLayoutEntry>& GetEntries() const override { return m_entries; }

    private:
        std::vector<RHIBindingLayoutEntry> m_entries;
    };

    class FakePipelineLayout final : public RHIPipelineLayout
    {
    };

    class FakePipeline final : public RHIPipeline
    {
    public:
        bool IsCompute() const override { return false; }
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

    class RecordingCommandContext final : public RHICommandContext
    {
    public:
        void Begin() override { ++beginCount; }
        void End() override { ++endCount; }
        void Reset() override {}
        void BeginEvent(const char*, uint32 = 0) override {}
        void EndEvent() override {}
        void SetMarker(const char*, uint32 = 0) override {}
        void BufferBarrier(const RHIBufferBarrier&) override { ++bufferBarrierCount; }
        void TextureBarrier(const RHITextureBarrier&) override { ++textureBarrierCount; }
        void Barriers(std::span<const RHIBufferBarrier>, std::span<const RHITextureBarrier>) override {}
        void BeginBarrier(const RHIBufferBarrier&) override {}
        void BeginBarrier(const RHITextureBarrier&) override {}
        void EndBarrier(const RHIBufferBarrier&) override {}
        void EndBarrier(const RHITextureBarrier&) override {}
        void BeginRenderPass(const RHIRenderPassDesc&) override { ++beginRenderPassCount; }
        void EndRenderPass() override { ++endRenderPassCount; }
        void SetPipeline(RHIPipeline* pipeline) override { pipelineSequence.push_back(pipeline); }
        void SetVertexBuffer(uint32, RHIBuffer*, uint64 = 0) override {}
        void SetVertexBuffers(uint32, std::span<RHIBuffer* const>, std::span<const uint64> = {}) override {}
        void SetIndexBuffer(RHIBuffer*, RHIFormat, uint64 = 0) override {}
        void SetDescriptorSet(uint32 set, RHIDescriptorSet*, std::span<const uint32> = {}) override
        {
            descriptorSetSequence.push_back(set);
        }
        void SetPushConstants(const void*, uint32, uint32 = 0) override {}
        void SetViewport(const RHIViewport&) override {}
        void SetViewports(std::span<const RHIViewport>) override {}
        void SetScissor(const RHIRect&) override {}
        void SetScissors(std::span<const RHIRect>) override {}
        void Draw(uint32, uint32 = 1, uint32 = 0, uint32 = 0) override {}
        void DrawIndexed(uint32, uint32 = 1, uint32 = 0, int32 = 0, uint32 = 0) override
        {
            ++drawIndexedCount;
        }
        void DrawIndirect(RHIBuffer*, uint64, uint32, uint32) override {}
        void DrawIndexedIndirect(RHIBuffer*, uint64, uint32, uint32) override {}
        void Dispatch(uint32, uint32, uint32) override {}
        void DispatchIndirect(RHIBuffer*, uint64) override {}
        void CopyBuffer(RHIBuffer*, RHIBuffer*, uint64, uint64, uint64) override { ++copyBufferCount; }
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
        uint32 beginRenderPassCount = 0;
        uint32 endRenderPassCount = 0;
        uint32 bufferBarrierCount = 0;
        uint32 textureBarrierCount = 0;
        uint32 copyBufferCount = 0;
        uint32 copyBufferToTextureCount = 0;
        uint32 drawIndexedCount = 0;
        std::vector<RHIPipeline*> pipelineSequence;
        std::vector<uint32> descriptorSetSequence;
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
            return RHIBufferRef(new FakeBuffer(desc, bufferMapSucceeds));
        }

        RHITextureRef CreateTexture(const RHITextureDesc& desc) override
        {
            return RHITextureRef(new FakeTexture(desc));
        }

        RHITextureViewRef CreateTextureView(RHITexture* texture, const RHITextureViewDesc& desc = {}) override
        {
            return RHITextureViewRef(new FakeTextureView(texture, desc));
        }

        RHISamplerRef CreateSampler(const RHISamplerDesc&) override
        {
            return RHISamplerRef(new FakeSampler());
        }

        RHIShaderRef CreateShader(const RHIShaderDesc& desc) override
        {
            return RHIShaderRef(new FakeShader(desc));
        }

        RHIHeapRef CreateHeap(const RHIHeapDesc&) override { return nullptr; }
        RHITextureRef CreatePlacedTexture(RHIHeap*, uint64, const RHITextureDesc&) override { return nullptr; }
        RHIBufferRef CreatePlacedBuffer(RHIHeap*, uint64, const RHIBufferDesc&) override { return nullptr; }
        MemoryRequirements GetTextureMemoryRequirements(const RHITextureDesc&) override { return {}; }
        MemoryRequirements GetBufferMemoryRequirements(const RHIBufferDesc&) override { return {}; }

        RHIDescriptorSetLayoutRef CreateDescriptorSetLayout(const RHIDescriptorSetLayoutDesc& desc) override
        {
            return RHIDescriptorSetLayoutRef(new FakeDescriptorSetLayout(desc));
        }

        RHIPipelineLayoutRef CreatePipelineLayout(const RHIPipelineLayoutDesc&) override
        {
            return RHIPipelineLayoutRef(new FakePipelineLayout());
        }

        RHIPipelineRef CreateGraphicsPipeline(const RHIGraphicsPipelineDesc&) override
        {
            return RHIPipelineRef(new FakePipeline());
        }

        RHIPipelineRef CreateComputePipeline(const RHIComputePipelineDesc&) override { return nullptr; }

        RHIDescriptorSetRef CreateDescriptorSet(const RHIDescriptorSetDesc& desc) override
        {
            return RHIDescriptorSetRef(new FakeDescriptorSet(desc));
        }

        RHIQueryPoolRef CreateQueryPool(const RHIQueryPoolDesc&) override { return nullptr; }

        RHICommandContextRef CreateCommandContext(RHICommandQueueType) override
        {
            return RHICommandContextRef(new RecordingCommandContext());
        }

        uint64 SubmitCommandContext(RHICommandContext*, RHIFence* signalFence = nullptr) override
        {
            if (!signalFence)
                return 0;

            const uint64 fenceValue = m_nextFenceValue++;
            signalFence->Signal(fenceValue);
            return fenceValue;
        }

        uint64 SubmitCommandContexts(std::span<RHICommandContext* const>, RHIFence* signalFence = nullptr) override
        {
            return SubmitCommandContext(nullptr, signalFence);
        }

        RHISwapChainRef CreateSwapChain(const RHISwapChainDesc&) override { return nullptr; }

        RHIFenceRef CreateFence(uint64 initialValue = 0) override
        {
            auto fence = RHIFenceRef(new FakeFence(initialValue));
            m_fences.push_back(fence);
            return fence;
        }

        void WaitForFence(RHIFence* fence, uint64 value) override
        {
            if (fence)
                fence->Wait(value);
        }

        void WaitIdle() override
        {
            for (const RHIFenceRef& fence : m_fences)
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
            return RHIStagingBufferRef(new FakeStagingBuffer(desc));
        }

        RHIRingBufferRef CreateRingBuffer(const RHIRingBufferDesc&) override { return nullptr; }
        RHIMemoryStats GetMemoryStats() const override { return {}; }
        void BeginResourceGroup(const char*) override {}
        void EndResourceGroup() override {}
        const RHICapabilities& GetCapabilities() const override { return m_capabilities; }
        RHIBackendType GetBackendType() const override { return RHIBackendType::DX12; }

        bool bufferMapSucceeds = true;

    private:
        uint64 m_nextFenceValue = 1;
        std::vector<RHIFenceRef> m_fences;
        RHICapabilities m_capabilities;
    };

    std::unique_ptr<Resource::MeshResource> CreateMeshResource(Resource::ResourceId id)
    {
        auto resource = std::make_unique<Resource::MeshResource>();
        resource->SetId(id);
        resource->SetName("RenderPassMesh");
        resource->SetMesh(MeshFactory::CreateTriangle());
        return resource;
    }

    RenderObject MakeRenderObject(Resource::MeshResource& meshResource)
    {
        RenderObject object;
        object.meshId = meshResource.GetId();
        object.meshResource = &meshResource;
        object.worldMatrix = Mat4Identity();
        object.normalMatrix = Mat4Identity();
        object.visible = true;
        return object;
    }

    RenderDrawItem MakeDrawItem(MaterialRenderMode mode)
    {
        RenderDrawItem item;
        item.objectIndex = 0;
        item.submeshIndex = 0;
        item.meshId = 401;
        item.materialId = static_cast<uint64>(mode) + 1;
        item.renderMode = mode;
        return item;
    }

    Resource::TextureHandle CreateTextureResource(Resource::ResourceId id)
    {
        auto* texture = new Resource::TextureResource();
        texture->SetId(id);
        texture->SetName("RenderPassTexture");

        Resource::TextureMetadata metadata;
        metadata.width = 1;
        metadata.height = 1;
        metadata.format = Resource::TextureFormat::RGBA8;
        metadata.isSRGB = false;

        texture->SetData({255, 255, 255, 255}, metadata);
        return Resource::TextureHandle(texture);
    }

    void ConfigureMaterialWithAlbedo(Resource::MaterialResource& materialResource,
                                     const Resource::TextureHandle& albedo)
    {
        materialResource.SetId(501);
        materialResource.SetName("RenderPassFallbackMaterial");
        materialResource.SetMaterialData(std::make_shared<Material>());
        materialResource.SetTexture("albedo", albedo);
    }

    class RenderPassValidationFixture : public ::testing::Test
    {
    protected:
        static void SetUpTestSuite()
        {
            Log::Initialize();
        }

        static void TearDownTestSuite()
        {
            Log::Shutdown();
        }

        void SetUp() override {}

        void Initialize(bool bufferMapSucceeds = true)
        {
            const fs::path shaderDir = FindShaderDirectory();
            if (shaderDir.empty())
            {
                GTEST_SKIP() << "Render/Shaders directory not found";
            }

            device.bufferMapSucceeds = bufferMapSucceeds;

            ASSERT_TRUE(pipelineCache.Initialize(&device, shaderDir.string())) << pipelineCache.GetLastError();

            gpuResources.Initialize(&device);
            ASSERT_TRUE(materialSystem.Initialize(&device, &gpuResources, pipelineCache.GetMaterialSetLayout()));

            meshResource = CreateMeshResource(401);
            gpuResources.UploadImmediate(meshResource.get());
            ASSERT_TRUE(gpuResources.IsGPUReady(meshResource->GetId()));

            scene.AddObject(MakeRenderObject(*meshResource));

            colorTexture = device.CreateTexture(RHITextureDesc::Texture2D(64, 64, RHIFormat::RGBA8_UNORM));
            ASSERT_TRUE(colorTexture);
            colorView = device.CreateTextureView(colorTexture.Get());
            ASSERT_TRUE(colorView);
        }

        void TearDown() override
        {
            materialSystem.Shutdown();
            gpuResources.Shutdown();
            pipelineCache.Shutdown();
        }

        FakeDevice device;
        PipelineCache pipelineCache;
        GPUResourceManager gpuResources;
        MaterialSystem materialSystem;
        RenderScene scene;
        std::unique_ptr<Resource::MeshResource> meshResource;
        RHITextureRef colorTexture;
        RHITextureViewRef colorView;
        ViewData view;
    };
} // namespace

TEST_F(RenderPassValidationFixture, OpaquePassBindsOpaqueThenMaskedPipelinesAndDrawsBothGroups)
{
    ASSERT_NO_FATAL_FAILURE(Initialize());

    std::vector<RenderDrawItem> opaqueItems = {MakeDrawItem(MaterialRenderMode::Opaque)};
    std::vector<RenderDrawItem> maskedItems = {MakeDrawItem(MaterialRenderMode::Masked)};

    OpaquePass pass;
    pass.SetResources(&gpuResources, &pipelineCache, &materialSystem);
    pass.SetRenderScene(&scene, &opaqueItems, &maskedItems);
    pass.SetRenderTargets(colorView.Get(), nullptr);

    RecordingCommandContext ctx;
    pass.Execute(ctx, view);

    ASSERT_EQ(static_cast<size_t>(2), ctx.pipelineSequence.size());
    EXPECT_EQ(pipelineCache.GetOpaquePipeline(), ctx.pipelineSequence[0]);
    EXPECT_EQ(pipelineCache.GetMaskedPipeline(), ctx.pipelineSequence[1]);
    EXPECT_EQ(2u, ctx.drawIndexedCount);
}

TEST_F(RenderPassValidationFixture, OpaquePassSkipsMaskedItemsWhenMaskedPipelineIsMissing)
{
    ASSERT_NO_FATAL_FAILURE(Initialize());

    ASSERT_TRUE(pipelineCache.GetMaskedPipeline());
    pipelineCache.m_maskedPipeline.Reset();

    std::vector<RenderDrawItem> opaqueItems = {MakeDrawItem(MaterialRenderMode::Opaque)};
    std::vector<RenderDrawItem> maskedItems = {MakeDrawItem(MaterialRenderMode::Masked)};

    OpaquePass pass;
    pass.SetResources(&gpuResources, &pipelineCache, &materialSystem);
    pass.SetRenderScene(&scene, &opaqueItems, &maskedItems);
    pass.SetRenderTargets(colorView.Get(), nullptr);

    RecordingCommandContext ctx;
    pass.Execute(ctx, view);

    ASSERT_EQ(static_cast<size_t>(1), ctx.pipelineSequence.size());
    EXPECT_EQ(pipelineCache.GetOpaquePipeline(), ctx.pipelineSequence[0]);
    EXPECT_EQ(1u, ctx.drawIndexedCount);
}

TEST_F(RenderPassValidationFixture, OpaquePassSkipsOpaqueItemsWhenOpaquePipelineIsMissing)
{
    ASSERT_NO_FATAL_FAILURE(Initialize());

    ASSERT_TRUE(pipelineCache.GetOpaquePipeline());
    pipelineCache.m_opaquePipeline.Reset();

    std::vector<RenderDrawItem> opaqueItems = {MakeDrawItem(MaterialRenderMode::Opaque)};
    std::vector<RenderDrawItem> maskedItems = {MakeDrawItem(MaterialRenderMode::Masked)};

    OpaquePass pass;
    pass.SetResources(&gpuResources, &pipelineCache, &materialSystem);
    pass.SetRenderScene(&scene, &opaqueItems, &maskedItems);
    pass.SetRenderTargets(colorView.Get(), nullptr);

    RecordingCommandContext ctx;
    pass.Execute(ctx, view);

    ASSERT_EQ(static_cast<size_t>(1), ctx.pipelineSequence.size());
    EXPECT_EQ(pipelineCache.GetMaskedPipeline(), ctx.pipelineSequence[0]);
    EXPECT_EQ(1u, ctx.drawIndexedCount);
}

TEST_F(RenderPassValidationFixture, TransparentPassBindsTransparentPipeline)
{
    ASSERT_NO_FATAL_FAILURE(Initialize());

    std::vector<RenderDrawItem> transparentItems = {MakeDrawItem(MaterialRenderMode::Transparent)};

    TransparentPass pass;
    pass.SetResources(&gpuResources, &pipelineCache, &materialSystem);
    pass.SetRenderScene(&scene, &transparentItems);
    pass.SetRenderTargets(colorView.Get(), nullptr);

    RecordingCommandContext ctx;
    pass.Execute(ctx, view);

    ASSERT_EQ(static_cast<size_t>(1), ctx.pipelineSequence.size());
    EXPECT_EQ(pipelineCache.GetTransparentPipeline(), ctx.pipelineSequence[0]);
    EXPECT_EQ(1u, ctx.drawIndexedCount);
}

TEST_F(RenderPassValidationFixture, OpaquePassSkipsDrawWhenMaterialBindingErrors)
{
    ASSERT_NO_FATAL_FAILURE(Initialize(false));

    std::vector<RenderDrawItem> opaqueItems = {MakeDrawItem(MaterialRenderMode::Opaque)};
    std::vector<RenderDrawItem> maskedItems;

    OpaquePass pass;
    pass.SetResources(&gpuResources, &pipelineCache, &materialSystem);
    pass.SetRenderScene(&scene, &opaqueItems, &maskedItems);
    pass.SetRenderTargets(colorView.Get(), nullptr);

    RecordingCommandContext ctx;
    pass.Execute(ctx, view);

    EXPECT_EQ(0u, ctx.drawIndexedCount);
    EXPECT_EQ(MaterialBindingStatus::Error, materialSystem.GetLastBindingResult().status);
    EXPECT_FALSE(materialSystem.GetLastBindingResult().IsDrawable());
    EXPECT_FALSE(std::any_of(ctx.descriptorSetSequence.begin(), ctx.descriptorSetSequence.end(),
                             [](uint32 set) { return set == 2; }));
}

TEST_F(RenderPassValidationFixture, OpaquePassDrawsWhenMaterialBindingUsesFallback)
{
    ASSERT_NO_FATAL_FAILURE(Initialize());

    Resource::TextureHandle missingTexture = CreateTextureResource(502);
    Resource::MaterialResource materialResource;
    ConfigureMaterialWithAlbedo(materialResource, missingTexture);

    RenderDrawItem item = MakeDrawItem(MaterialRenderMode::Opaque);
    item.materialResource = &materialResource;
    std::vector<RenderDrawItem> opaqueItems = {item};
    std::vector<RenderDrawItem> maskedItems;

    OpaquePass pass;
    pass.SetResources(&gpuResources, &pipelineCache, &materialSystem);
    pass.SetRenderScene(&scene, &opaqueItems, &maskedItems);
    pass.SetRenderTargets(colorView.Get(), nullptr);

    RecordingCommandContext ctx;
    pass.Execute(ctx, view);

    EXPECT_EQ(1u, ctx.drawIndexedCount);
    EXPECT_EQ(MaterialBindingStatus::Fallback, materialSystem.GetLastBindingResult().status);
    EXPECT_TRUE(materialSystem.GetLastBindingResult().IsDrawable());
    EXPECT_TRUE(std::any_of(ctx.descriptorSetSequence.begin(), ctx.descriptorSetSequence.end(),
                            [](uint32 set) { return set == 2; }));
}

TEST_F(RenderPassValidationFixture, TransparentPassSkipsDrawWhenMaterialBindingErrors)
{
    ASSERT_NO_FATAL_FAILURE(Initialize(false));

    std::vector<RenderDrawItem> transparentItems = {MakeDrawItem(MaterialRenderMode::Transparent)};

    TransparentPass pass;
    pass.SetResources(&gpuResources, &pipelineCache, &materialSystem);
    pass.SetRenderScene(&scene, &transparentItems);
    pass.SetRenderTargets(colorView.Get(), nullptr);

    RecordingCommandContext ctx;
    pass.Execute(ctx, view);

    EXPECT_EQ(0u, ctx.drawIndexedCount);
    EXPECT_EQ(MaterialBindingStatus::Error, materialSystem.GetLastBindingResult().status);
    EXPECT_FALSE(materialSystem.GetLastBindingResult().IsDrawable());
    EXPECT_FALSE(std::any_of(ctx.descriptorSetSequence.begin(), ctx.descriptorSetSequence.end(),
                             [](uint32 set) { return set == 2; }));
}

TEST_F(RenderPassValidationFixture, TransparentPassDrawsWhenMaterialBindingUsesFallback)
{
    ASSERT_NO_FATAL_FAILURE(Initialize());

    Resource::TextureHandle missingTexture = CreateTextureResource(503);
    Resource::MaterialResource materialResource;
    ConfigureMaterialWithAlbedo(materialResource, missingTexture);

    RenderDrawItem item = MakeDrawItem(MaterialRenderMode::Transparent);
    item.materialResource = &materialResource;
    std::vector<RenderDrawItem> transparentItems = {item};

    TransparentPass pass;
    pass.SetResources(&gpuResources, &pipelineCache, &materialSystem);
    pass.SetRenderScene(&scene, &transparentItems);
    pass.SetRenderTargets(colorView.Get(), nullptr);

    RecordingCommandContext ctx;
    pass.Execute(ctx, view);

    EXPECT_EQ(1u, ctx.drawIndexedCount);
    EXPECT_EQ(MaterialBindingStatus::Fallback, materialSystem.GetLastBindingResult().status);
    EXPECT_TRUE(materialSystem.GetLastBindingResult().IsDrawable());
    EXPECT_TRUE(std::any_of(ctx.descriptorSetSequence.begin(), ctx.descriptorSetSequence.end(),
                            [](uint32 set) { return set == 2; }));
}
