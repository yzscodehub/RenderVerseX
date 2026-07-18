#include "Resources/RenderResourceRegistry.h"
#include "Resources/RenderRetirementQueue.h"
#include "Resources/RenderSubmissionTracker.h"
#include "Resources/RenderUploadProcessor.h"
#include "RHI/RHICapabilities.h"
#include "RHI/RHICommandContext.h"
#include "RHI/RHIDevice.h"
#include "Runtime/RenderResourceGateway.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <memory>
#include <span>
#include <vector>

namespace RVX
{
namespace
{
    struct DestructionState
    {
        bool destroyed = false;
    };

    class FakeBuffer final : public RHIBuffer
    {
    public:
        FakeBuffer(const RHIBufferDesc& desc,
                   std::shared_ptr<DestructionState> destruction = {})
            : m_desc(desc),
              m_storage(static_cast<size_t>(desc.size)),
              m_destruction(std::move(destruction))
        {
        }

        ~FakeBuffer() override
        {
            if (m_destruction)
            {
                m_destruction->destroyed = true;
            }
        }

        uint64 GetSize() const override { return m_desc.size; }
        RHIBufferUsage GetUsage() const override { return m_desc.usage; }
        RHIMemoryType GetMemoryType() const override { return m_desc.memoryType; }
        uint32 GetStride() const override { return m_desc.stride; }
        void* Map() override
        {
            return m_storage.empty() ? nullptr : m_storage.data();
        }
        void Unmap() override {}

    private:
        RHIBufferDesc m_desc;
        std::vector<uint8> m_storage;
        std::shared_ptr<DestructionState> m_destruction;
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
        RHITextureDimension GetDimension() const override
        {
            return m_desc.dimension;
        }
        RHISampleCount GetSampleCount() const override
        {
            return m_desc.sampleCount;
        }

    private:
        RHITextureDesc m_desc;
    };

    class FakeSampler final : public RHISampler
    {
    };

    class FakeStagingBuffer final : public RHIStagingBuffer
    {
    public:
        explicit FakeStagingBuffer(const RHIStagingBufferDesc& desc)
            : m_size(desc.size)
        {
            RHIBufferDesc bufferDesc;
            bufferDesc.size = desc.size;
            bufferDesc.usage = RHIBufferUsage::CopySrc;
            bufferDesc.memoryType = RHIMemoryType::Upload;
            m_buffer = RHIBufferRef(new FakeBuffer(bufferDesc));
            m_storage.resize(static_cast<size_t>(desc.size));
        }

        void* Map(uint64 offset = 0, uint64 size = RVX_WHOLE_SIZE) override
        {
            const uint64 mappedSize = size == RVX_WHOLE_SIZE
                                          ? m_size - offset
                                          : size;
            if (offset > m_size || mappedSize > m_size - offset)
            {
                return nullptr;
            }
            return m_storage.data() + static_cast<size_t>(offset);
        }
        void Unmap() override {}
        uint64 GetSize() const override { return m_size; }
        RHIBuffer* GetBuffer() const override { return m_buffer.Get(); }
        const std::vector<uint8>& GetStorage() const { return m_storage; }

    private:
        uint64 m_size = 0;
        RHIBufferRef m_buffer;
        std::vector<uint8> m_storage;
    };

    class FakeCommandContext final : public RHICommandContext
    {
    public:
        explicit FakeCommandContext(RHICommandQueueType queueType)
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
        void TextureBarrier(const RHITextureBarrier&) override {}
        void Barriers(std::span<const RHIBufferBarrier>,
                      std::span<const RHITextureBarrier>) override {}
        void BeginBarrier(const RHIBufferBarrier&) override {}
        void BeginBarrier(const RHITextureBarrier&) override {}
        void EndBarrier(const RHIBufferBarrier&) override {}
        void EndBarrier(const RHITextureBarrier&) override {}
        void BeginRenderPass(const RHIRenderPassDesc&) override {}
        void EndRenderPass() override {}
        void SetPipeline(RHIPipeline*) override {}
        void SetVertexBuffer(uint32, RHIBuffer*, uint64 = 0) override {}
        void SetVertexBuffers(uint32,
                              std::span<RHIBuffer* const>,
                              std::span<const uint64> = {}) override {}
        void SetIndexBuffer(RHIBuffer*, RHIFormat, uint64 = 0) override {}
        void SetDescriptorSet(uint32,
                              RHIDescriptorSet*,
                              std::span<const uint32> = {}) override {}
        void SetPushConstants(const void*, uint32, uint32 = 0) override {}
        void SetViewport(const RHIViewport&) override {}
        void SetViewports(std::span<const RHIViewport>) override {}
        void SetScissor(const RHIRect&) override {}
        void SetScissors(std::span<const RHIRect>) override {}
        void Draw(uint32, uint32 = 1, uint32 = 0, uint32 = 0) override {}
        void DrawIndexed(uint32,
                         uint32 = 1,
                         uint32 = 0,
                         int32 = 0,
                         uint32 = 0) override {}
        void DrawIndirect(RHIBuffer*, uint64, uint32, uint32) override {}
        void DrawIndexedIndirect(RHIBuffer*, uint64, uint32, uint32) override {}
        void Dispatch(uint32, uint32, uint32) override {}
        void DispatchIndirect(RHIBuffer*, uint64) override {}
        void CopyBuffer(RHIBuffer*,
                        RHIBuffer*,
                        uint64,
                        uint64,
                        uint64) override
        {
            ++copyBufferCount;
        }
        void CopyTexture(RHITexture*,
                         RHITexture*,
                         const RHITextureCopyDesc& = {}) override {}
        void CopyBufferToTexture(
            RHIBuffer*,
            RHITexture*,
            const RHIBufferTextureCopyDesc& desc) override
        {
            ++copyTextureCount;
            textureCopies.push_back(desc);
        }
        void CopyTextureToBuffer(
            RHITexture*,
            RHIBuffer*,
            const RHIBufferTextureCopyDesc&) override {}
        void BeginQuery(RHIQueryPool*, uint32) override {}
        void EndQuery(RHIQueryPool*, uint32) override {}
        void WriteTimestamp(RHIQueryPool*, uint32) override {}
        void ResolveQueries(RHIQueryPool*,
                            uint32,
                            uint32,
                            RHIBuffer*,
                            uint64) override {}
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
        uint32 copyBufferCount = 0;
        uint32 copyTextureCount = 0;
        std::vector<RHIBufferTextureCopyDesc> textureCopies;

    private:
        RHICommandQueueType m_queueType = RHICommandQueueType::Graphics;
    };

    class FakeFence final : public RHIFence
    {
    public:
        explicit FakeFence(uint64 initialValue)
            : m_completed(initialValue), m_next(initialValue + 1U)
        {
        }

        uint64 GetCompletedValue() const override { return m_completed; }
        void Signal(uint64 value) override { Complete(value); }
        void SignalOnQueue(uint64 value, RHICommandQueueType) override
        {
            Complete(value);
        }
        void Wait(uint64 value, uint64 = UINT64_MAX) override
        {
            Complete(value);
        }
        uint64 Allocate() { return m_next++; }
        void Complete(uint64 value)
        {
            m_completed = std::max(m_completed, value);
            m_next = std::max(m_next, value + 1U);
        }

    private:
        uint64 m_completed = 0;
        uint64 m_next = 1;
    };

    RHICapabilities MakeCapabilities(bool compatibility = false)
    {
        RHICapabilities capabilities;
        capabilities.backendType = RHIBackendType::DX12;
        capabilities.adapterName = "RenderResourceRuntimeValidation";
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
        if (compatibility)
        {
            capabilities.backendType = RHIBackendType::DX11;
            capabilities.supportsDefaultQueueFenceSignal = false;
            capabilities.supportsExplicitQueueFenceSignal = false;
            capabilities.emulatesQueueFences = true;
            capabilities.supportsAsyncCompute = false;
            capabilities.queueTopology.completionMode =
                RHIQueueCompletionMode::CompatibilityWaitIdle;
            capabilities.queueTopology.logicalQueueDomains = {
                GPUQueueDomain::Graphics,
                GPUQueueDomain::Graphics,
                GPUQueueDomain::Graphics};
            capabilities.queueTopology.activeDomainCount = 1;
            return capabilities;
        }
        capabilities.queueTopology.completionMode =
            RHIQueueCompletionMode::NativeTimeline;
        capabilities.queueTopology.logicalQueueDomains = {
            GPUQueueDomain::Graphics,
            GPUQueueDomain::Compute,
            GPUQueueDomain::Copy};
        capabilities.queueTopology.activeDomainCount = 3;
        return capabilities;
    }

    class FakeDevice final : public IRHIDevice
    {
    public:
        explicit FakeDevice(bool compatibility = false)
            : capabilities(MakeCapabilities(compatibility))
        {
        }

        RHIBufferRef CreateBuffer(const RHIBufferDesc& desc) override
        {
            if (ShouldFailCreate())
            {
                return {};
            }
            auto state = std::make_shared<DestructionState>();
            resourceBufferStates.push_back(state);
            return RHIBufferRef(new FakeBuffer(desc, std::move(state)));
        }
        RHITextureRef CreateTexture(const RHITextureDesc& desc) override
        {
            return ShouldFailCreate() ? RHITextureRef{}
                                      : RHITextureRef(new FakeTexture(desc));
        }
        RHITextureViewRef CreateTextureView(
            RHITexture*,
            const RHITextureViewDesc& = {}) override
        {
            return {};
        }
        RHISamplerRef CreateSampler(const RHISamplerDesc&) override
        {
            return ShouldFailCreate() ? RHISamplerRef{}
                                      : RHISamplerRef(new FakeSampler());
        }
        RHIShaderRef CreateShader(const RHIShaderDesc&) override { return {}; }
        RHIHeapRef CreateHeap(const RHIHeapDesc&) override { return {}; }
        RHITextureRef CreatePlacedTexture(
            RHIHeap*,
            uint64,
            const RHITextureDesc&) override
        {
            return {};
        }
        RHIBufferRef CreatePlacedBuffer(
            RHIHeap*,
            uint64,
            const RHIBufferDesc&) override
        {
            return {};
        }
        MemoryRequirements GetTextureMemoryRequirements(
            const RHITextureDesc&) override
        {
            return {};
        }
        MemoryRequirements GetBufferMemoryRequirements(
            const RHIBufferDesc&) override
        {
            return {};
        }
        RHIDescriptorSetLayoutRef CreateDescriptorSetLayout(
            const RHIDescriptorSetLayoutDesc&) override
        {
            return {};
        }
        RHIPipelineLayoutRef CreatePipelineLayout(
            const RHIPipelineLayoutDesc&) override
        {
            return {};
        }
        RHIPipelineRef CreateGraphicsPipeline(
            const RHIGraphicsPipelineDesc&) override
        {
            return {};
        }
        RHIPipelineRef CreateComputePipeline(
            const RHIComputePipelineDesc&) override
        {
            return {};
        }
        RHIDescriptorSetRef CreateDescriptorSet(
            const RHIDescriptorSetDesc&) override
        {
            return {};
        }
        RHIQueryPoolRef CreateQueryPool(const RHIQueryPoolDesc&) override
        {
            return {};
        }
        RHICommandContextRef CreateCommandContext(
            RHICommandQueueType type) override
        {
            if (failContextCreation)
            {
                return {};
            }
            RHICommandContextRef context(new FakeCommandContext(type));
            commandContexts.push_back(context);
            return context;
        }
        uint64 SubmitCommandContext(RHICommandContext*,
                                    RHIFence* signalFence) override
        {
            ++submitCount;
            if (failSubmit || signalFence == nullptr)
            {
                return 0;
            }
            lastFence = static_cast<FakeFence*>(signalFence);
            lastSubmittedValue = lastFence->Allocate();
            return lastSubmittedValue;
        }
        uint64 SubmitCommandContexts(
            std::span<RHICommandContext* const>,
            RHIFence*) override
        {
            return 0;
        }
        RHISwapChainRef CreateSwapChain(const RHISwapChainDesc&) override
        {
            return {};
        }
        RHIFenceRef CreateFence(uint64 initialValue = 0) override
        {
            RHIFenceRef fence(new FakeFence(initialValue));
            fences.push_back(fence);
            return fence;
        }
        void WaitForFence(RHIFence* fence, uint64 value) override
        {
            fence->Wait(value);
        }
        void WaitIdle() override
        {
            ++waitIdleCount;
            if (lastFence != nullptr)
            {
                lastFence->Complete(lastSubmittedValue);
            }
        }
        void BeginFrame() override {}
        void EndFrame() override {}
        uint32 GetCurrentFrameIndex() const override { return 0; }
        RHIStagingBufferRef CreateStagingBuffer(
            const RHIStagingBufferDesc& desc) override
        {
            if (failStagingCreation)
            {
                return {};
            }
            RHIStagingBufferRef staging(new FakeStagingBuffer(desc));
            lastStaging = static_cast<FakeStagingBuffer*>(staging.Get());
            return staging;
        }
        RHIRingBufferRef CreateRingBuffer(
            const RHIRingBufferDesc&) override
        {
            return {};
        }
        RHIMemoryStats GetMemoryStats() const override { return {}; }
        void BeginResourceGroup(const char*) override {}
        void EndResourceGroup() override {}
        const RHICapabilities& GetCapabilities() const override
        {
            return capabilities;
        }
        RHIBackendType GetBackendType() const override
        {
            return capabilities.backendType;
        }

        void CompleteLastSubmission()
        {
            ASSERT_NE(lastFence, nullptr);
            lastFence->Complete(lastSubmittedValue);
        }

        bool ShouldFailCreate()
        {
            ++createCallCount;
            return failCreateAt != 0 && createCallCount == failCreateAt;
        }

        RHICapabilities capabilities;
        std::vector<RHIFenceRef> fences;
        std::vector<RHICommandContextRef> commandContexts;
        std::vector<std::shared_ptr<DestructionState>> resourceBufferStates;
        FakeFence* lastFence = nullptr;
        uint64 lastSubmittedValue = 0;
        uint32 submitCount = 0;
        uint32 waitIdleCount = 0;
        uint32 createCallCount = 0;
        uint32 failCreateAt = 0;
        bool failContextCreation = false;
        bool failStagingCreation = false;
        bool failSubmit = false;
        FakeStagingBuffer* lastStaging = nullptr;
    };

    ResourceUploadRequestCreateInfo MakeMeshInfo(
        AssetId assetId,
        RenderResourceHandle handle,
        bool withNormal = false)
    {
        MeshUploadPayload payload;
        payload.createInfo.vertexCount = 3;
        payload.createInfo.boundsMin = Vec3(-1.0f);
        payload.createInfo.boundsMax = Vec3(1.0f);
        payload.bytes.resize(withNormal ? 72U : 36U, 7U);
        payload.positionRange = UploadByteRange{0, 36, 12};
        if (withNormal)
        {
            payload.normalRange = UploadByteRange{36, 36, 12};
        }

        ResourceUploadRequestCreateInfo info;
        info.sequence = assetId.value;
        info.assetId = assetId;
        info.handle = handle;
        info.kind = RenderResourceKind::Mesh;
        info.payload = std::move(payload);
        info.declaredPayloadBytes = withNormal ? 72 : 36;
        return info;
    }

    ResourceUploadRequestCreateInfo MakeTextureInfo(
        AssetId assetId,
        RenderResourceHandle handle)
    {
        TextureUploadPayload payload;
        payload.createInfo.width = 1;
        payload.createInfo.height = 1;
        payload.createInfo.depth = 1;
        payload.createInfo.mipLevels = 1;
        payload.createInfo.arrayLayers = 1;
        payload.createInfo.format = TextureUploadFormat::RGBA8;
        payload.bytes = {1, 2, 3, 4};
        payload.subresources.push_back(
            TextureUploadSubresource{UploadByteRange{0, 4, 0},
                                     0,
                                     0,
                                     4,
                                     4});

        ResourceUploadRequestCreateInfo info;
        info.sequence = assetId.value;
        info.assetId = assetId;
        info.handle = handle;
        info.kind = RenderResourceKind::Texture;
        info.payload = std::move(payload);
        info.declaredPayloadBytes =
            4 + static_cast<uint64>(sizeof(TextureUploadSubresource));
        return info;
    }

    ResourceUploadRequestCreateInfo MakeMaterialInfo(
        AssetId assetId,
        RenderResourceHandle handle,
        const std::vector<RenderResourceHandle>& textures)
    {
        MaterialUploadPayload payload;
        for (size_t index = 0; index < textures.size(); ++index)
        {
            MaterialUploadTextureBinding binding;
            binding.slot = index == 0
                               ? MaterialUploadTextureSlot::BaseColor
                               : MaterialUploadTextureSlot::Normal;
            binding.texture = textures[index];
            payload.textureBindings.push_back(binding);
        }

        ResourceUploadRequestCreateInfo info;
        info.sequence = assetId.value;
        info.assetId = assetId;
        info.handle = handle;
        info.kind = RenderResourceKind::Material;
        info.payload = std::move(payload);
        info.dependencies = textures;
        info.declaredPayloadBytes =
            static_cast<uint64>(textures.size()) *
            (sizeof(MaterialUploadTextureBinding) +
             sizeof(RenderResourceHandle));
        return info;
    }

    class RenderResourceRuntimeFixture : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            RenderTransportConfig config;
            config.statusSlotCapacity = 1024;
            gateway = std::make_unique<RenderResourceGateway>(config);
            ASSERT_TRUE(tracker.Initialize(&device));
            ASSERT_TRUE(retirement.Initialize(&tracker));
            ASSERT_TRUE(registry.Initialize(&gateway->GetStatusTable(),
                                            &retirement));
            ASSERT_TRUE(processor.Initialize(&device,
                                             &gateway->GetStatusTable(),
                                             &registry,
                                             &tracker));
        }

        void TearDown() override
        {
            device.WaitIdle();
            static_cast<void>(processor.PollCompletion());
            processor.Shutdown();
            static_cast<void>(retirement.Poll());
            registry.Shutdown();
            static_cast<void>(retirement.ForceDeviceLostTeardown());
            tracker.Shutdown();
            gateway.reset();
        }

        RenderResourceHandle Reserve(AssetId asset,
                                     RenderResourceKind kind)
        {
            const RenderResourceReserveResult reserved =
                gateway->ReserveResource(asset, kind);
            EXPECT_EQ(reserved.code, RenderResourceReserveCode::Reserved);
            return reserved.handle;
        }

        ResourceUploadRequestRef CreateAndQueue(
            ResourceUploadRequestCreateInfo info)
        {
            const ResourceUploadRequestCreateResult created =
                ResourceUploadRequest::Create(std::move(info));
            EXPECT_EQ(created.code, ResourceUploadRequestCreateCode::Created);
            EXPECT_NE(created.request, nullptr);
            if (created.request)
            {
                EXPECT_EQ(gateway->TryEnqueueUpload(created.request).code,
                          RenderUploadEnqueueCode::Accepted);
            }
            return created.request;
        }

        RenderUploadProcessCode DequeueAndProcess()
        {
            ResourceUploadRequestRef dequeued = gateway->TryDequeueUpload();
            EXPECT_NE(dequeued, nullptr);
            return processor.ProcessUpload(std::move(dequeued));
        }

        void CompleteAndPoll()
        {
            device.CompleteLastSubmission();
            EXPECT_NE(processor.PollCompletion(), GPUCompletionStatus::Pending);
        }

        FakeDevice device;
        RenderSubmissionTracker tracker;
        RenderRetirementQueue retirement;
        RenderResourceRegistry registry;
        RenderUploadProcessor processor;
        std::unique_ptr<RenderResourceGateway> gateway;
    };

    TEST_F(RenderResourceRuntimeFixture, MeshCopyIsPendingThenCommitsExactly)
    {
        const AssetId asset{1};
        const RenderResourceHandle handle = Reserve(asset, RenderResourceKind::Mesh);
        ResourceUploadRequestRef owner =
            CreateAndQueue(MakeMeshInfo(asset, handle, true));

        EXPECT_EQ(DequeueAndProcess(), RenderUploadProcessCode::Accepted);
        EXPECT_EQ(gateway->QueryResourceStatus(handle).state,
                  RenderResourcePublicState::Uploading);
        EXPECT_EQ(registry.ResolveMesh(handle), nullptr);
        EXPECT_EQ(processor.PollCompletion(), GPUCompletionStatus::Pending);

        CompleteAndPoll();
        EXPECT_EQ(gateway->QueryResourceStatus(handle).state,
                  RenderResourcePublicState::GPUReady);
        const RenderMeshResourceData* mesh = registry.ResolveMesh(handle);
        ASSERT_NE(mesh, nullptr);
        EXPECT_EQ(mesh->buffers.size(), 2U);
        EXPECT_EQ(processor.GetInFlightCount(), 0U);
    }

    TEST_F(RenderResourceRuntimeFixture, TextureAndMaterialCommitWithExactDependencies)
    {
        const AssetId textureAsset{2};
        const RenderResourceHandle textureHandle =
            Reserve(textureAsset, RenderResourceKind::Texture);
        ResourceUploadRequestRef textureOwner =
            CreateAndQueue(MakeTextureInfo(textureAsset, textureHandle));
        ASSERT_EQ(DequeueAndProcess(), RenderUploadProcessCode::Accepted);
        CompleteAndPoll();
        ASSERT_NE(registry.ResolveTexture(textureHandle), nullptr);

        const AssetId materialAsset{3};
        const RenderResourceHandle materialHandle =
            Reserve(materialAsset, RenderResourceKind::Material);
        ResourceUploadRequestRef materialOwner = CreateAndQueue(
            MakeMaterialInfo(materialAsset,
                             materialHandle,
                             {textureHandle}));
        ASSERT_EQ(DequeueAndProcess(), RenderUploadProcessCode::Accepted);
        CompleteAndPoll();

        const RenderMaterialResourceData* material =
            registry.ResolveMaterial(materialHandle);
        ASSERT_NE(material, nullptr);
        EXPECT_NE(material->constants.Get(), nullptr);
        EXPECT_EQ(material->samplers.size(), 1U);
        const auto* dependencies = registry.GetDependencies(materialHandle);
        ASSERT_NE(dependencies, nullptr);
        ASSERT_EQ(dependencies->size(), 1U);
        EXPECT_EQ((*dependencies)[0], textureHandle);
    }

    TEST_F(RenderResourceRuntimeFixture, TextureUploadUsesSafeFootprintsAndExpandsRGB8)
    {
        const AssetId asset{16};
        const RenderResourceHandle handle =
            Reserve(asset, RenderResourceKind::Texture);
        TextureUploadPayload payload;
        payload.createInfo.width = 3;
        payload.createInfo.height = 2;
        payload.createInfo.depth = 1;
        payload.createInfo.mipLevels = 1;
        payload.createInfo.arrayLayers = 1;
        payload.createInfo.format = TextureUploadFormat::RGB8;
        payload.createInfo.isSRGB = false;
        payload.bytes = {
            1, 2, 3, 4, 5, 6, 7, 8, 9, 0, 0, 0,
            10, 11, 12, 13, 14, 15, 16, 17, 18, 0, 0, 0};
        payload.subresources.push_back(
            TextureUploadSubresource{UploadByteRange{0, 24, 0},
                                     0,
                                     0,
                                     12,
                                     24});
        ResourceUploadRequestCreateInfo info;
        info.sequence = asset.value;
        info.assetId = asset;
        info.handle = handle;
        info.kind = RenderResourceKind::Texture;
        info.payload = std::move(payload);
        info.declaredPayloadBytes =
            24 + sizeof(TextureUploadSubresource);
        ResourceUploadRequestRef owner = CreateAndQueue(std::move(info));

        ASSERT_EQ(DequeueAndProcess(), RenderUploadProcessCode::Accepted);
        ASSERT_NE(device.lastStaging, nullptr);
        EXPECT_EQ(device.lastStaging->GetSize(), 512U);
        const std::vector<uint8>& staging = device.lastStaging->GetStorage();
        ASSERT_EQ(staging.size(), 512U);
        EXPECT_EQ((std::array<uint8, 12>{staging[0],
                                        staging[1],
                                        staging[2],
                                        staging[3],
                                        staging[4],
                                        staging[5],
                                        staging[6],
                                        staging[7],
                                        staging[8],
                                        staging[9],
                                        staging[10],
                                        staging[11]}),
                  (std::array<uint8, 12>{1, 2, 3, 255,
                                        4, 5, 6, 255,
                                        7, 8, 9, 255}));
        EXPECT_EQ((std::array<uint8, 4>{staging[256],
                                       staging[257],
                                       staging[258],
                                       staging[259]}),
                  (std::array<uint8, 4>{10, 11, 12, 255}));
        ASSERT_FALSE(device.commandContexts.empty());
        auto* context = static_cast<FakeCommandContext*>(
            device.commandContexts.back().Get());
        ASSERT_EQ(context->textureCopies.size(), 1U);
        EXPECT_EQ(context->textureCopies[0].bufferOffset, 0U);
        EXPECT_EQ(context->textureCopies[0].bufferRowPitch, 256U);
        EXPECT_EQ(context->textureCopies[0].bufferImageHeight, 2U);

        CompleteAndPoll();
        const RenderTextureResourceData* texture =
            registry.ResolveTexture(handle);
        ASSERT_NE(texture, nullptr);
        EXPECT_EQ(texture->texture->GetFormat(), RHIFormat::RGBA8_UNORM);
    }

    TEST_F(RenderResourceRuntimeFixture, CubemapUsesLogicalCubeCountAndPhysicalFaces)
    {
        const AssetId asset{18};
        const RenderResourceHandle handle =
            Reserve(asset, RenderResourceKind::Texture);
        TextureUploadPayload payload;
        payload.createInfo.width = 1;
        payload.createInfo.height = 1;
        payload.createInfo.depth = 1;
        payload.createInfo.mipLevels = 1;
        payload.createInfo.arrayLayers = 6;
        payload.createInfo.format = TextureUploadFormat::RGBA8;
        payload.createInfo.isCubemap = true;
        payload.bytes.resize(24U, 42U);
        for (uint32 face = 0; face < 6U; ++face)
        {
            payload.subresources.push_back(TextureUploadSubresource{
                UploadByteRange{face * 4U, 4, 0}, 0, face, 4, 4});
        }
        ResourceUploadRequestCreateInfo info;
        info.sequence = asset.value;
        info.assetId = asset;
        info.handle = handle;
        info.kind = RenderResourceKind::Texture;
        info.payload = std::move(payload);
        info.declaredPayloadBytes =
            24 + 6U * sizeof(TextureUploadSubresource);
        ResourceUploadRequestRef owner = CreateAndQueue(std::move(info));

        ASSERT_EQ(DequeueAndProcess(), RenderUploadProcessCode::Accepted);
        ASSERT_FALSE(device.commandContexts.empty());
        auto* context = static_cast<FakeCommandContext*>(
            device.commandContexts.back().Get());
        ASSERT_EQ(context->textureCopies.size(), 6U);
        for (uint32 face = 0; face < 6U; ++face)
        {
            EXPECT_EQ(context->textureCopies[face].textureSubresource, face);
        }

        CompleteAndPoll();
        const RenderTextureResourceData* texture =
            registry.ResolveTexture(handle);
        ASSERT_NE(texture, nullptr);
        EXPECT_EQ(texture->texture->GetDimension(),
                  RHITextureDimension::TextureCube);
        EXPECT_EQ(texture->texture->GetArraySize(), 1U);
    }

    TEST_F(RenderResourceRuntimeFixture, FactoryRejectsSchemaAndRangeBeforeRender)
    {
        const RenderResourceHandle handle = Reserve(AssetId{4}, RenderResourceKind::Mesh);
        ResourceUploadRequestCreateInfo schema = MakeMeshInfo(AssetId{4}, handle);
        schema.schemaVersion += 1;
        EXPECT_EQ(ResourceUploadRequest::Create(schema).code,
                  ResourceUploadRequestCreateCode::InvalidSchema);

        ResourceUploadRequestCreateInfo range = MakeMeshInfo(AssetId{4}, handle);
        std::get<MeshUploadPayload>(range.payload).positionRange.offset = 1000;
        EXPECT_EQ(ResourceUploadRequest::Create(range).code,
                  ResourceUploadRequestCreateCode::InvalidPayload);
    }

    TEST_F(RenderResourceRuntimeFixture, MissingExactDependencyFailsWithoutRHIWork)
    {
        const RenderResourceHandle staleTexture{77, 1};
        const AssetId asset{5};
        const RenderResourceHandle handle =
            Reserve(asset, RenderResourceKind::Material);
        ResourceUploadRequestRef owner = CreateAndQueue(
            MakeMaterialInfo(asset, handle, {staleTexture}));

        EXPECT_EQ(DequeueAndProcess(),
                  RenderUploadProcessCode::DependencyUnavailable);
        const RenderResourceStatus status = gateway->QueryResourceStatus(handle);
        EXPECT_EQ(status.state, RenderResourcePublicState::Failed);
        EXPECT_EQ(status.failure,
                  RenderResourceFailureCode::DependencyUnavailable);
        EXPECT_EQ(device.createCallCount, 0U);
    }

    TEST_F(RenderResourceRuntimeFixture, NthCreationFailureRetiresPartialObjects)
    {
        const AssetId asset{6};
        const RenderResourceHandle handle = Reserve(asset, RenderResourceKind::Mesh);
        ResourceUploadRequestRef owner =
            CreateAndQueue(MakeMeshInfo(asset, handle, true));
        device.failCreateAt = 2;

        EXPECT_EQ(DequeueAndProcess(),
                  RenderUploadProcessCode::ResourceCreationFailed);
        EXPECT_EQ(gateway->QueryResourceStatus(handle).state,
                  RenderResourcePublicState::Failed);
        ASSERT_EQ(device.resourceBufferStates.size(), 1U);
        EXPECT_TRUE(device.resourceBufferStates.front()->destroyed);
        EXPECT_FALSE(registry.HasExactEntry(handle));
    }

    TEST_F(RenderResourceRuntimeFixture, SubmissionFailureRetiresCreatedObjects)
    {
        const AssetId asset{7};
        const RenderResourceHandle handle = Reserve(asset, RenderResourceKind::Mesh);
        ResourceUploadRequestRef owner =
            CreateAndQueue(MakeMeshInfo(asset, handle));
        device.failSubmit = true;

        EXPECT_EQ(DequeueAndProcess(), RenderUploadProcessCode::SubmissionFailed);
        const RenderResourceStatus status = gateway->QueryResourceStatus(handle);
        EXPECT_EQ(status.state, RenderResourcePublicState::Failed);
        EXPECT_EQ(status.failure,
                  RenderResourceFailureCode::UploadSubmissionFailed);
        ASSERT_EQ(device.resourceBufferStates.size(), 1U);
        EXPECT_TRUE(device.resourceBufferStates.front()->destroyed);
    }

    TEST_F(RenderResourceRuntimeFixture, ReleaseCoversReservedQueuedAndFailed)
    {
        const RenderResourceHandle reserved =
            Reserve(AssetId{8}, RenderResourceKind::Mesh);
        EXPECT_EQ(gateway->RequestRelease(reserved).code,
                  RenderReleaseCode::Accepted);
        processor.ProcessRelease(gateway->TryDequeueRelease());
        EXPECT_EQ(gateway->QueryResourceStatus(reserved).state,
                  RenderResourcePublicState::Released);

        const AssetId queuedAsset{9};
        const RenderResourceHandle queued =
            Reserve(queuedAsset, RenderResourceKind::Mesh);
        ResourceUploadRequestRef queuedOwner =
            CreateAndQueue(MakeMeshInfo(queuedAsset, queued));
        EXPECT_EQ(gateway->RequestRelease(queued).code,
                  RenderReleaseCode::Accepted);
        processor.ProcessRelease(gateway->TryDequeueRelease());
        EXPECT_EQ(gateway->QueryResourceStatus(queued).state,
                  RenderResourcePublicState::Released);
        EXPECT_EQ(DequeueAndProcess(), RenderUploadProcessCode::StaleState);

        const AssetId failedAsset{10};
        const RenderResourceHandle failed =
            Reserve(failedAsset, RenderResourceKind::Mesh);
        ResourceUploadRequestRef failedOwner =
            CreateAndQueue(MakeMeshInfo(failedAsset, failed));
        device.failContextCreation = true;
        EXPECT_EQ(DequeueAndProcess(),
                  RenderUploadProcessCode::ResourceCreationFailed);
        device.failContextCreation = false;
        EXPECT_EQ(gateway->RequestRelease(failed).code,
                  RenderReleaseCode::Accepted);
        processor.ProcessRelease(gateway->TryDequeueRelease());
        EXPECT_EQ(gateway->QueryResourceStatus(failed).state,
                  RenderResourcePublicState::Released);
    }

    TEST_F(RenderResourceRuntimeFixture, ReleaseWhileUploadingNeverPublishesReady)
    {
        const AssetId asset{11};
        const RenderResourceHandle handle = Reserve(asset, RenderResourceKind::Mesh);
        ResourceUploadRequestRef owner =
            CreateAndQueue(MakeMeshInfo(asset, handle));
        ASSERT_EQ(DequeueAndProcess(), RenderUploadProcessCode::Accepted);
        EXPECT_EQ(gateway->RequestRelease(handle).code,
                  RenderReleaseCode::Accepted);
        processor.ProcessRelease(gateway->TryDequeueRelease());

        CompleteAndPoll();
        EXPECT_EQ(gateway->QueryResourceStatus(handle).state,
                  RenderResourcePublicState::Released);
        EXPECT_EQ(registry.ResolveMesh(handle), nullptr);
    }

    TEST_F(RenderResourceRuntimeFixture, ReadyReleaseRetiresUntilRecordedTokenCompletes)
    {
        const AssetId asset{12};
        const RenderResourceHandle handle = Reserve(asset, RenderResourceKind::Mesh);
        ResourceUploadRequestRef owner =
            CreateAndQueue(MakeMeshInfo(asset, handle));
        ASSERT_EQ(DequeueAndProcess(), RenderUploadProcessCode::Accepted);
        CompleteAndPoll();
        ASSERT_EQ(device.resourceBufferStates.size(), 1U);
        const auto destruction = device.resourceBufferStates.front();

        EXPECT_EQ(gateway->RequestRelease(handle).code,
                  RenderReleaseCode::Accepted);
        processor.ProcessRelease(gateway->TryDequeueRelease());
        EXPECT_EQ(gateway->QueryResourceStatus(handle).state,
                  RenderResourcePublicState::Released);
        EXPECT_FALSE(destruction->destroyed);
        EXPECT_EQ(retirement.GetDiagnostics().entryCount, 1U);
        EXPECT_EQ(retirement.Poll(), GPUCompletionStatus::Completed);
        EXPECT_TRUE(destruction->destroyed);
    }

    TEST_F(RenderResourceRuntimeFixture, StaleGenerationCannotResolveReplacement)
    {
        const AssetId firstAsset{13};
        const RenderResourceHandle first =
            Reserve(firstAsset, RenderResourceKind::Texture);
        ResourceUploadRequestRef firstOwner =
            CreateAndQueue(MakeTextureInfo(firstAsset, first));
        ASSERT_EQ(DequeueAndProcess(), RenderUploadProcessCode::Accepted);
        CompleteAndPoll();
        ASSERT_NE(registry.ResolveTexture(first), nullptr);
        ASSERT_EQ(gateway->RequestRelease(first).code,
                  RenderReleaseCode::Accepted);
        processor.ProcessRelease(gateway->TryDequeueRelease());
        static_cast<void>(retirement.Poll());

        const AssetId secondAsset{14};
        const RenderResourceHandle second =
            Reserve(secondAsset, RenderResourceKind::Texture);
        EXPECT_EQ(second.slot, first.slot);
        EXPECT_EQ(second.generation, first.generation + 1U);
        ResourceUploadRequestRef secondOwner =
            CreateAndQueue(MakeTextureInfo(secondAsset, second));
        ASSERT_EQ(DequeueAndProcess(), RenderUploadProcessCode::Accepted);
        CompleteAndPoll();

        EXPECT_EQ(registry.ResolveTexture(first), nullptr);
        EXPECT_NE(registry.ResolveTexture(second), nullptr);
    }

    TEST_F(RenderResourceRuntimeFixture, RenderDropsRequestBeforeTerminalPublication)
    {
        const AssetId asset{15};
        const RenderResourceHandle handle = Reserve(asset, RenderResourceKind::Mesh);
        ResourceUploadRequestRef updateOwner =
            CreateAndQueue(MakeMeshInfo(asset, handle));
        std::weak_ptr<const ResourceUploadRequest> weak = updateOwner;
        ASSERT_EQ(DequeueAndProcess(), RenderUploadProcessCode::Accepted);
        EXPECT_GT(updateOwner.use_count(), 1L);

        CompleteAndPoll();
        EXPECT_EQ(gateway->QueryResourceStatus(handle).state,
                  RenderResourcePublicState::GPUReady);
        EXPECT_EQ(updateOwner.use_count(), 1L);
        EXPECT_FALSE(weak.expired());
        updateOwner.reset();
        EXPECT_TRUE(weak.expired());
    }

    TEST(RenderResourceRuntimeValidation, CompatibilityUploadWaitsIdleAndRecordsMode)
    {
        RenderTransportConfig config;
        config.statusSlotCapacity = 1024;
        RenderResourceGateway gateway(config);
        FakeDevice device(true);
        RenderSubmissionTracker tracker;
        RenderRetirementQueue retirement;
        RenderResourceRegistry registry;
        RenderUploadProcessor processor;
        ASSERT_TRUE(tracker.Initialize(&device));
        ASSERT_TRUE(retirement.Initialize(&tracker));
        ASSERT_TRUE(registry.Initialize(&gateway.GetStatusTable(),
                                        &retirement));
        ASSERT_TRUE(processor.Initialize(&device,
                                         &gateway.GetStatusTable(),
                                         &registry,
                                         &tracker));

        const AssetId asset{17};
        const RenderResourceReserveResult reserved =
            gateway.ReserveResource(asset, RenderResourceKind::Mesh);
        ASSERT_EQ(reserved.code, RenderResourceReserveCode::Reserved);
        ResourceUploadRequestCreateResult created =
            ResourceUploadRequest::Create(
                MakeMeshInfo(asset, reserved.handle));
        ASSERT_EQ(created.code, ResourceUploadRequestCreateCode::Created);
        ASSERT_EQ(gateway.TryEnqueueUpload(created.request).code,
                  RenderUploadEnqueueCode::Accepted);
        ASSERT_EQ(processor.ProcessUpload(gateway.TryDequeueUpload()),
                  RenderUploadProcessCode::Accepted);

        EXPECT_EQ(processor.PollCompletion(),
                  GPUCompletionStatus::CompatibilityWaitIdle);
        EXPECT_EQ(device.waitIdleCount, 1U);
        EXPECT_EQ(processor.GetStats().compatibilityWaits, 1U);
        EXPECT_EQ(gateway.QueryResourceStatus(reserved.handle).state,
                  RenderResourcePublicState::GPUReady);
        EXPECT_NE(registry.ResolveMesh(reserved.handle), nullptr);

        processor.Shutdown();
        registry.Shutdown();
        static_cast<void>(retirement.ForceDeviceLostTeardown());
        tracker.Shutdown();
    }
} // namespace
} // namespace RVX
