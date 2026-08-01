#include "Core/Core.h"
#include "Render/GPUUploadService.h"
#include "RHI/RHICommandContext.h"
#include "RHI/RHIDevice.h"
#include <gtest/gtest.h>

#include <cstring>
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

        std::vector<uint8>& GetStorage() { return m_storage; }

    private:
        RHIBufferDesc m_desc;
        std::vector<uint8> m_storage;
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
            return fakeBuffer->GetStorage().data() + offset;
        }

        void Unmap() override {}
        uint64 GetSize() const override { return m_desc.size; }
        RHIBuffer* GetBuffer() const override { return m_buffer.Get(); }

    private:
        RHIStagingBufferDesc m_desc;
        RHIBufferRef m_buffer;
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

    class FakeCommandContext final : public RHICommandContext
    {
    public:
        explicit FakeCommandContext(RHICommandQueueType queueType)
            : m_queueType(queueType)
        {
        }

        RHICommandQueueType GetQueueType() const override { return m_queueType; }
        uint32 beginCount = 0;
        uint32 endCount = 0;
        uint32 copyBufferCount = 0;
        uint32 copyBufferToTextureCount = 0;
        uint32 bufferBarrierCount = 0;
        uint32 textureBarrierCount = 0;
        std::vector<RHIBufferTextureCopyDesc> copyBufferToTextureDescs;

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
        void CopyBuffer(RHIBuffer*, RHIBuffer*, uint64, uint64, uint64) override { ++copyBufferCount; }
        void CopyTexture(RHITexture*, RHITexture*, const RHITextureCopyDesc& = {}) override {}
        void CopyBufferToTexture(RHIBuffer*, RHITexture*, const RHIBufferTextureCopyDesc& desc) override
        {
            ++copyBufferToTextureCount;
            copyBufferToTextureDescs.push_back(desc);
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

    private:
        RHICommandQueueType m_queueType = RHICommandQueueType::Graphics;
    };

    class FakeFence final : public RHIFence
    {
    public:
        explicit FakeFence(uint64 initialValue)
            : m_completedValue(initialValue)
            , m_nextSignalValue(initialValue + 1)
        {
        }

        uint64 GetCompletedValue() const override { return m_completedValue; }
        void Signal(uint64 value) override
        {
            m_completedValue = value;
            if (m_nextSignalValue <= value)
            {
                m_nextSignalValue = value + 1;
            }
        }
        void SignalOnQueue(uint64 value, RHICommandQueueType) override { Signal(value); }
        void Wait(uint64 value, uint64 = UINT64_MAX) override { Signal(value); }
        uint64 AllocateSignalValue() { return m_nextSignalValue++; }

    private:
        uint64 m_completedValue = 0;
        uint64 m_nextSignalValue = 1;
    };

    class FakeDevice final : public IRHIDevice
    {
    public:
        FakeDevice()
        {
            capabilities.backendType = RHIBackendType::DX12;
            capabilities.adapterName = "GPUUploadServiceValidation";
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
            backendType = RHIBackendType::DX12;
        }

        RHIBufferRef CreateBuffer(const RHIBufferDesc& desc) override
        {
            ++createdBufferCount;
            return RHIBufferRef(new FakeBuffer(desc));
        }

        RHITextureRef CreateTexture(const RHITextureDesc& desc) override
        {
            ++createdTextureCount;
            lastCreatedTextureDesc = desc;
            return RHITextureRef(new FakeTexture(desc));
        }

        RHITextureViewRef CreateTextureView(RHITexture*, const RHITextureViewDesc& = {}) override { return nullptr; }
        RHISamplerRef CreateSampler(const RHISamplerDesc&) override { return nullptr; }
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
        RHIDescriptorSetRef CreateDescriptorSet(const RHIDescriptorSetDesc&) override { return nullptr; }
        RHIQueryPoolRef CreateQueryPool(const RHIQueryPoolDesc&) override { return nullptr; }
        RHICommandContextRef CreateCommandContext(RHICommandQueueType type) override
        {
            ++createdCommandContextCount;
            lastCommandQueueType = type;
            retainedCommandContext = RHICommandContextRef(new FakeCommandContext(type));
            lastCommandContext = static_cast<FakeCommandContext*>(retainedCommandContext.Get());
            return retainedCommandContext;
        }

        uint64 SubmitCommandContext(RHICommandContext* context, RHIFence* signalFence) override
        {
            ++submittedCommandContextCount;
            lastSubmittedContext = context;
            lastSubmittedFence = signalFence;
            return signalFence ? static_cast<FakeFence*>(signalFence)->AllocateSignalValue() : 0;
        }

        uint64 SubmitCommandContexts(std::span<RHICommandContext* const>, RHIFence* signalFence) override
        {
            return signalFence ? static_cast<FakeFence*>(signalFence)->AllocateSignalValue() : 0;
        }
        RHISwapChainRef CreateSwapChain(const RHISwapChainDesc&) override { return nullptr; }
        RHIFenceRef CreateFence(uint64 initialValue) override
        {
            ++createdFenceCount;
            retainedFences.push_back(RHIFenceRef(new FakeFence(fenceInitialValueOverride ? fenceInitialValueOverride : initialValue)));
            lastFence = static_cast<FakeFence*>(retainedFences.back().Get());
            return retainedFences.back();
        }

        void WaitForFence(RHIFence*, uint64) override {}
        void WaitIdle() override
        {
            ++waitIdleCount;
            if (signalLastFenceOnWaitIdle && lastFence)
            {
                lastFence->Signal(lastFence->GetCompletedValue() + 1);
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
        const RHICapabilities& GetCapabilities() const override
        {
            capabilities.backendType = backendType;
            if (backendType == RHIBackendType::OpenGL)
            {
                capabilities.supportsAsyncCompute = false;
                capabilities.supportsDefaultQueueFenceSignal = false;
                capabilities.supportsExplicitQueueFenceSignal = false;
                capabilities.emulatesQueueFences = true;
                capabilities.queueTopology.completionMode =
                    RHIQueueCompletionMode::CompatibilityWaitIdle;
                capabilities.queueTopology.logicalQueueDomains = {
                    GPUQueueDomain::Graphics,
                    GPUQueueDomain::Graphics,
                    GPUQueueDomain::Graphics};
                capabilities.queueTopology.activeDomainCount = 1;
                capabilities.opengl.majorVersion = 4;
                capabilities.opengl.minorVersion = 6;
            }
            return capabilities;
        }
        RHIBackendType GetBackendType() const override { return backendType; }

        uint32 createdBufferCount = 0;
        uint32 createdTextureCount = 0;
        uint32 createdCommandContextCount = 0;
        uint32 submittedCommandContextCount = 0;
        uint32 createdStagingBufferCount = 0;
        uint32 createdFenceCount = 0;
        uint32 waitIdleCount = 0;
        RHICommandQueueType lastCommandQueueType = RHICommandQueueType::Graphics;
        FakeCommandContext* lastCommandContext = nullptr;
        FakeFence* lastFence = nullptr;
        RHICommandContext* lastSubmittedContext = nullptr;
        RHIFence* lastSubmittedFence = nullptr;
        RHICommandContextRef retainedCommandContext;
        std::vector<RHIFenceRef> retainedFences;
        mutable RHICapabilities capabilities;
        RHITextureDesc lastCreatedTextureDesc;
        RHIBackendType backendType = RHIBackendType::DX12;
        uint64 fenceInitialValueOverride = 0;
        bool signalLastFenceOnWaitIdle = false;
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
} // namespace

TEST(GPUUploadServiceValidation, RHITextureSubresourceHelpersUsePhysicalCubeLayers)
{
    RHITextureDesc cubeDesc;
    cubeDesc.dimension = RHITextureDimension::TextureCube;
    cubeDesc.arraySize = 1;
    cubeDesc.mipLevels = 2;

    EXPECT_EQ(GetTexturePhysicalLayerCount(cubeDesc), 6u);
    EXPECT_EQ(GetTextureSubresourceCount(cubeDesc), 12u);
    EXPECT_EQ(EncodeTextureSubresource(1, 5, cubeDesc.mipLevels), 11u);

    const auto decoded = DecodeTextureSubresource(11, cubeDesc.mipLevels);
    EXPECT_EQ(decoded.mipLevel, 1u);
    EXPECT_EQ(decoded.physicalLayer, 5u);
    EXPECT_EQ(ResolveTextureArrayLayerCount(cubeDesc, RHISubresourceRange::All()), 6u);

    RHITextureDesc arrayDesc;
    arrayDesc.dimension = RHITextureDimension::Texture2D;
    arrayDesc.arraySize = 3;
    arrayDesc.mipLevels = 4;

    EXPECT_EQ(GetTexturePhysicalLayerCount(arrayDesc), 3u);
    EXPECT_EQ(GetTextureSubresourceCount(arrayDesc), 12u);
    EXPECT_EQ(EncodeTextureSubresource(2, 1, arrayDesc.mipLevels), 6u);
}

TEST(GPUUploadServiceValidation, StagedBufferUploadsBatchUntilFlush)
{
    FakeDevice device;
    GPUUploadService uploadService;
    uploadService.Initialize(&device);

    const uint32 first[] = {1, 2, 3, 4};
    const uint32 second[] = {5, 6, 7, 8};

    GPUUploadBufferDesc desc;
    desc.size = sizeof(first);
    desc.usage = RHIBufferUsage::Vertex;
    desc.stride = sizeof(uint32);

    auto firstResult = uploadService.UploadBufferDataWithResult(desc, first, sizeof(first));
    auto secondResult = uploadService.UploadBufferDataWithResult(desc, second, sizeof(second));

    EXPECT_TRUE(firstResult.succeeded);
    EXPECT_TRUE(secondResult.succeeded);
    EXPECT_EQ(firstResult.finalAccess.layout, RHIResourceLayout::General);
    EXPECT_EQ(firstResult.finalAccess.domain, GPUQueueDomain::Copy);
    EXPECT_EQ(firstResult.finalAccess.contentValidity, RHIContentValidity::Valid);
    EXPECT_TRUE(firstResult.isPending);
    EXPECT_TRUE(secondResult.isPending);
    EXPECT_TRUE(uploadService.IsUploadPending(firstResult.uploadId));
    EXPECT_TRUE(uploadService.IsUploadPending(secondResult.uploadId));
    EXPECT_EQ(device.createdCommandContextCount, 1u);
    EXPECT_EQ(device.submittedCommandContextCount, 0u);
    EXPECT_EQ(device.createdFenceCount, 3u);
    ASSERT_NE(nullptr, device.lastCommandContext);
    EXPECT_EQ(device.lastCommandContext->beginCount, 1u);
    EXPECT_EQ(device.lastCommandContext->endCount, 0u);
    EXPECT_EQ(device.lastCommandContext->copyBufferCount, 2u);

    uploadService.FlushBatchUploads();

    EXPECT_EQ(device.submittedCommandContextCount, 1u);
    EXPECT_EQ(device.createdFenceCount, 3u);
    EXPECT_EQ(device.lastCommandContext->endCount, 1u);
    EXPECT_EQ(device.lastSubmittedContext, device.lastCommandContext);
    EXPECT_EQ(device.lastSubmittedFence, device.lastFence);

    EXPECT_EQ(uploadService.ProcessCompletedUploads(), 0u);
    ASSERT_NE(nullptr, device.lastFence);
    device.lastFence->Signal(1);
    EXPECT_EQ(uploadService.ProcessCompletedUploads(), 2u);
    EXPECT_TRUE(uploadService.IsUploadComplete(firstResult.uploadId));
    EXPECT_TRUE(uploadService.IsUploadComplete(secondResult.uploadId));

    const auto stats = uploadService.GetStats();
    EXPECT_EQ(stats.pendingUploadCount, 0u);
    EXPECT_EQ(stats.completedUploadCount, 2u);
    EXPECT_EQ(stats.stagingBytesInFlight, 0ull);

    uploadService.Shutdown();
}

TEST(GPUUploadServiceValidation, ShutdownFlushesDirtyBatchAndWaitsForPendingUploads)
{
    FakeDevice device;
    GPUUploadService uploadService;
    uploadService.Initialize(&device);

    const uint32 source[] = {1, 2, 3, 4};

    GPUUploadBufferDesc desc;
    desc.size = sizeof(source);
    desc.usage = RHIBufferUsage::Vertex;
    desc.stride = sizeof(uint32);

    auto result = uploadService.UploadBufferDataWithResult(desc, source, sizeof(source));

    EXPECT_TRUE(result.succeeded);
    EXPECT_TRUE(result.isPending);
    EXPECT_EQ(device.submittedCommandContextCount, 0u);
    EXPECT_EQ(device.waitIdleCount, 0u);

    uploadService.Shutdown();

    EXPECT_EQ(device.submittedCommandContextCount, 1u);
    EXPECT_EQ(device.createdFenceCount, 3u);
    EXPECT_EQ(device.waitIdleCount, 0u);
    EXPECT_EQ(device.lastCommandContext->endCount, 1u);
}

TEST(GPUUploadServiceValidation, FlushTracksFenceNextCompletedValue)
{
    FakeDevice device;
    device.fenceInitialValueOverride = 4;

    GPUUploadService uploadService;
    uploadService.Initialize(&device);

    const uint32 source[] = {1, 2, 3, 4};

    GPUUploadBufferDesc desc;
    desc.size = sizeof(source);
    desc.usage = RHIBufferUsage::Vertex;
    desc.stride = sizeof(uint32);

    auto result = uploadService.UploadBufferDataWithResult(desc, source, sizeof(source));
    EXPECT_TRUE(result.succeeded);

    uploadService.FlushBatchUploads();

    EXPECT_EQ(uploadService.ProcessCompletedUploads(), 0u);
    ASSERT_NE(nullptr, device.lastFence);

    device.lastFence->Signal(4);
    EXPECT_EQ(uploadService.ProcessCompletedUploads(), 0u);
    EXPECT_TRUE(uploadService.IsUploadPending(result.uploadId));

    device.lastFence->Signal(5);
    EXPECT_EQ(uploadService.ProcessCompletedUploads(), 1u);
    EXPECT_TRUE(uploadService.IsUploadComplete(result.uploadId));

    uploadService.ForgetCompletedUpload(result.uploadId);
    EXPECT_TRUE(!uploadService.IsUploadComplete(result.uploadId));

    uploadService.Shutdown();
}

TEST(GPUUploadServiceValidation, CompletedFenceIsReusedForNextBatch)
{
    FakeDevice device;
    GPUUploadService uploadService;
    uploadService.Initialize(&device);

    const uint32 first[] = {1, 2, 3, 4};
    const uint32 second[] = {5, 6, 7, 8};

    GPUUploadBufferDesc desc;
    desc.size = sizeof(first);
    desc.usage = RHIBufferUsage::Vertex;
    desc.stride = sizeof(uint32);

    auto firstResult = uploadService.UploadBufferDataWithResult(desc, first, sizeof(first));
    EXPECT_TRUE(firstResult.succeeded);
    uploadService.FlushBatchUploads();

    EXPECT_EQ(device.createdFenceCount, 3u);
    ASSERT_NE(nullptr, device.lastFence);
    RHIFence* firstFence = device.lastFence;

    device.lastFence->Signal(1);
    EXPECT_EQ(uploadService.ProcessCompletedUploads(), 1u);
    EXPECT_TRUE(uploadService.IsUploadComplete(firstResult.uploadId));

    auto secondResult = uploadService.UploadBufferDataWithResult(desc, second, sizeof(second));
    EXPECT_TRUE(secondResult.succeeded);
    uploadService.FlushBatchUploads();

    EXPECT_EQ(device.createdFenceCount, 3u);
    EXPECT_EQ(device.lastSubmittedFence, firstFence);

    device.lastFence->Signal(1);
    EXPECT_EQ(uploadService.ProcessCompletedUploads(), 0u);
    EXPECT_TRUE(uploadService.IsUploadPending(secondResult.uploadId));

    device.lastFence->Signal(2);
    EXPECT_EQ(uploadService.ProcessCompletedUploads(), 1u);
    EXPECT_TRUE(uploadService.IsUploadComplete(secondResult.uploadId));

    uploadService.Shutdown();
}

TEST(GPUUploadServiceValidation, FlushAndWaitSubmitsDirtyBatchAndCompletesUploads)
{
    FakeDevice device;
    device.signalLastFenceOnWaitIdle = true;

    GPUUploadService uploadService;
    uploadService.Initialize(&device);

    const uint32 source[] = {1, 2, 3, 4};

    GPUUploadBufferDesc desc;
    desc.size = sizeof(source);
    desc.usage = RHIBufferUsage::Vertex;
    desc.stride = sizeof(uint32);

    auto result = uploadService.UploadBufferDataWithResult(desc, source, sizeof(source));
    EXPECT_TRUE(result.succeeded);
    EXPECT_TRUE(result.isPending);
    EXPECT_EQ(device.submittedCommandContextCount, 0u);
    EXPECT_TRUE(uploadService.IsUploadPending(result.uploadId));

    EXPECT_EQ(uploadService.FlushAndWaitForUploads(), 1u);

    EXPECT_EQ(device.submittedCommandContextCount, 1u);
    EXPECT_EQ(device.waitIdleCount, 0u);
    EXPECT_TRUE(!uploadService.IsUploadPending(result.uploadId));
    EXPECT_TRUE(uploadService.IsUploadComplete(result.uploadId));

    const auto stats = uploadService.GetStats();
    EXPECT_EQ(stats.pendingUploadCount, 0u);
    EXPECT_EQ(stats.completedUploadCount, 1u);
    EXPECT_EQ(stats.stagingBytesInFlight, 0ull);

    uploadService.Shutdown();
}

TEST(GPUUploadServiceValidation, StagedTextureUploadCopiesEveryMipSubresource)
{
    FakeDevice device;
    GPUUploadService uploadService;
    uploadService.Initialize(&device);

    std::vector<uint8> pixels(84, 7);

    GPUUploadTextureDesc desc;
    desc.textureDesc = RHITextureDesc::Texture2D(4, 4, RHIFormat::RGBA8_UNORM);
    desc.textureDesc.mipLevels = 3;
    desc.dataSize = pixels.size();

    auto result = uploadService.UploadTextureDataWithResult(desc, pixels.data());

    ASSERT_TRUE(result.succeeded);
    EXPECT_EQ(result.finalAccess.layout, RHIResourceLayout::General);
    EXPECT_EQ(result.finalAccess.domain, GPUQueueDomain::Copy);
    EXPECT_EQ(result.finalAccess.contentValidity, RHIContentValidity::Valid);
    EXPECT_EQ(device.createdTextureCount, 1u);
    ASSERT_NE(nullptr, device.lastCommandContext);
    EXPECT_EQ(device.lastCommandContext->copyBufferToTextureCount, 3u);
    ASSERT_EQ(device.lastCommandContext->copyBufferToTextureDescs.size(), 3u);

    const auto& mip0 = device.lastCommandContext->copyBufferToTextureDescs[0];
    EXPECT_EQ(mip0.textureSubresource, 0u);
    EXPECT_EQ(mip0.bufferOffset, 0ull);
    EXPECT_EQ(mip0.bufferRowPitch, 256u);
    EXPECT_EQ(mip0.bufferImageHeight, 4u);
    EXPECT_EQ(mip0.textureRegion.width, 4u);
    EXPECT_EQ(mip0.textureRegion.height, 4u);

    const auto& mip1 = device.lastCommandContext->copyBufferToTextureDescs[1];
    EXPECT_EQ(mip1.textureSubresource, 1u);
    EXPECT_EQ(mip1.bufferOffset, 1024ull);
    EXPECT_EQ(mip1.bufferRowPitch, 256u);
    EXPECT_EQ(mip1.bufferImageHeight, 2u);
    EXPECT_EQ(mip1.textureRegion.width, 2u);
    EXPECT_EQ(mip1.textureRegion.height, 2u);

    const auto& mip2 = device.lastCommandContext->copyBufferToTextureDescs[2];
    EXPECT_EQ(mip2.textureSubresource, 2u);
    EXPECT_EQ(mip2.bufferOffset, 1536ull);
    EXPECT_EQ(mip2.bufferRowPitch, 256u);
    EXPECT_EQ(mip2.bufferImageHeight, 1u);
    EXPECT_EQ(mip2.textureRegion.width, 1u);
    EXPECT_EQ(mip2.textureRegion.height, 1u);

    EXPECT_EQ(result.bytesUploaded, pixels.size());
    uploadService.Shutdown();
}

TEST(GPUUploadServiceValidation, StagedOpenGLCompressedTextureUploadUsesTightBlockRows)
{
    FakeDevice device;
    device.backendType = RHIBackendType::OpenGL;
    GPUUploadService uploadService;
    uploadService.Initialize(&device);

    std::vector<uint8> blocks(24, 0xBC);

    GPUUploadTextureDesc desc;
    desc.textureDesc = RHITextureDesc::Texture2D(4, 4, RHIFormat::BC1_UNORM);
    desc.textureDesc.mipLevels = 3;
    desc.dataSize = blocks.size();

    auto result = uploadService.UploadTextureDataWithResult(desc, blocks.data());

    ASSERT_TRUE(result.succeeded);
    EXPECT_EQ(device.createdTextureCount, 1u);
    ASSERT_NE(nullptr, device.lastCommandContext);
    ASSERT_EQ(device.lastCommandContext->copyBufferToTextureDescs.size(), 3u);

    const auto& mip0 = device.lastCommandContext->copyBufferToTextureDescs[0];
    const auto& mip1 = device.lastCommandContext->copyBufferToTextureDescs[1];
    const auto& mip2 = device.lastCommandContext->copyBufferToTextureDescs[2];
    EXPECT_EQ(mip0.bufferOffset, 0ull);
    EXPECT_EQ(mip0.bufferRowPitch, 8u);
    EXPECT_EQ(mip0.bufferImageHeight, 1u);
    EXPECT_EQ(mip1.bufferOffset, 8ull);
    EXPECT_EQ(mip1.bufferRowPitch, 8u);
    EXPECT_EQ(mip1.bufferImageHeight, 1u);
    EXPECT_EQ(mip2.bufferOffset, 16ull);
    EXPECT_EQ(mip2.bufferRowPitch, 8u);
    EXPECT_EQ(mip2.bufferImageHeight, 1u);

    EXPECT_EQ(result.bytesUploaded, 24ull);
    EXPECT_EQ(uploadService.GetStats().stagingBytesInFlight, 24ull);
    uploadService.Shutdown();
}

TEST(GPUUploadServiceValidation, StagedTextureArrayUploadCopiesEveryLayer)
{
    FakeDevice device;
    GPUUploadService uploadService;
    uploadService.Initialize(&device);

    const uint8 pixels[] = {
        1, 2, 3, 4,
        5, 6, 7, 8
    };

    GPUUploadTextureDesc desc;
    desc.textureDesc = RHITextureDesc::Texture2D(1, 1, RHIFormat::RGBA8_UNORM);
    desc.textureDesc.arraySize = 2;
    desc.dataSize = sizeof(pixels);

    auto result = uploadService.UploadTextureDataWithResult(desc, pixels);

    ASSERT_TRUE(result.succeeded);
    ASSERT_NE(nullptr, device.lastCommandContext);
    ASSERT_EQ(device.lastCommandContext->copyBufferToTextureDescs.size(), 2u);
    EXPECT_EQ(device.lastCommandContext->copyBufferToTextureDescs[0].textureSubresource, 0u);
    EXPECT_EQ(device.lastCommandContext->copyBufferToTextureDescs[0].bufferOffset, 0ull);
    EXPECT_EQ(device.lastCommandContext->copyBufferToTextureDescs[1].textureSubresource, 1u);
    EXPECT_EQ(device.lastCommandContext->copyBufferToTextureDescs[1].bufferOffset, 512ull);

    uploadService.Shutdown();
}

TEST(GPUUploadServiceValidation, StagedCubemapUploadUsesPhysicalLayerMipOrder)
{
    FakeDevice device;
    GPUUploadService uploadService;
    uploadService.Initialize(&device);

    std::vector<uint8> pixels(120, 3);

    GPUUploadTextureDesc desc;
    desc.textureDesc = RHITextureDesc::Texture2D(2, 2, RHIFormat::RGBA8_UNORM);
    desc.textureDesc.dimension = RHITextureDimension::TextureCube;
    desc.textureDesc.arraySize = 1;
    desc.textureDesc.mipLevels = 2;
    desc.dataSize = pixels.size();

    auto result = uploadService.UploadTextureDataWithResult(desc, pixels.data());

    ASSERT_TRUE(result.succeeded);
    EXPECT_EQ(device.createdTextureCount, 1u);
    EXPECT_EQ(device.lastCreatedTextureDesc.dimension, RHITextureDimension::TextureCube);
    EXPECT_EQ(device.lastCreatedTextureDesc.arraySize, 1u);
    EXPECT_EQ(device.lastCreatedTextureDesc.mipLevels, 2u);
    ASSERT_NE(nullptr, device.lastCommandContext);
    ASSERT_EQ(device.lastCommandContext->copyBufferToTextureDescs.size(), 12u);

    for (uint32 physicalLayer = 0; physicalLayer < 6; ++physicalLayer)
    {
        const uint32 baseIndex = physicalLayer * 2;
        const auto& mip0 = device.lastCommandContext->copyBufferToTextureDescs[baseIndex + 0];
        const auto& mip1 = device.lastCommandContext->copyBufferToTextureDescs[baseIndex + 1];

        EXPECT_EQ(mip0.textureSubresource, EncodeTextureSubresource(0, physicalLayer, 2));
        EXPECT_EQ(mip0.textureRegion.width, 2u);
        EXPECT_EQ(mip0.textureRegion.height, 2u);

        EXPECT_EQ(mip1.textureSubresource, EncodeTextureSubresource(1, physicalLayer, 2));
        EXPECT_EQ(mip1.textureRegion.width, 1u);
        EXPECT_EQ(mip1.textureRegion.height, 1u);
        EXPECT_GT(mip1.bufferOffset, mip0.bufferOffset);
    }

    EXPECT_EQ(result.bytesUploaded, pixels.size());
    uploadService.Shutdown();
}

TEST(GPUUploadServiceValidation, TooSmallMippedTextureDataDoesNotCreateEmptyTexture)
{
    FakeDevice device;
    GPUUploadService uploadService;
    uploadService.Initialize(&device);

    const uint8 pixels[] = {
        255, 0, 0, 255,
        0, 255, 0, 255,
        0, 0, 255, 255,
        255, 255, 255, 255
    };

    GPUUploadTextureDesc desc;
    desc.textureDesc = RHITextureDesc::Texture2D(2, 2, RHIFormat::RGBA8_UNORM);
    desc.textureDesc.mipLevels = 2;
    desc.dataSize = sizeof(pixels);

    auto result = uploadService.UploadTextureDataWithResult(desc, pixels);

    EXPECT_TRUE(!result.succeeded);
    EXPECT_TRUE(!result.resource);
    EXPECT_EQ(result.mode, GPUUploadMode::None);
    EXPECT_EQ(result.failureReason, GPUUploadFailureReason::Unsupported);
    EXPECT_EQ(device.createdTextureCount, 0u);

    const auto stats = uploadService.GetStats();
    EXPECT_EQ(stats.failedUploadCount, 1u);
    EXPECT_EQ(stats.textureUploadCount, 0u);
    EXPECT_EQ(stats.uploadedBytes, 0ull);

    uploadService.Shutdown();
}

TEST(GPUUploadServiceValidation, AbandonedUploadDoesNotRemainCompleted)
{
    FakeDevice device;
    GPUUploadService uploadService;
    uploadService.Initialize(&device);

    const uint32 source[] = {1, 2, 3, 4};

    GPUUploadBufferDesc desc;
    desc.size = sizeof(source);
    desc.usage = RHIBufferUsage::Vertex;
    desc.stride = sizeof(uint32);

    auto result = uploadService.UploadBufferDataWithResult(desc, source, sizeof(source));
    EXPECT_TRUE(result.succeeded);
    EXPECT_TRUE(result.isPending);

    uploadService.AbandonUpload(result.uploadId);
    uploadService.FlushBatchUploads();

    ASSERT_NE(nullptr, device.lastFence);
    device.lastFence->Signal(1);
    EXPECT_EQ(uploadService.ProcessCompletedUploads(), 1u);
    EXPECT_TRUE(!uploadService.IsUploadPending(result.uploadId));
    EXPECT_TRUE(!uploadService.IsUploadComplete(result.uploadId));

    const auto stats = uploadService.GetStats();
    EXPECT_EQ(stats.pendingUploadCount, 0u);
    EXPECT_EQ(stats.stagingBytesInFlight, 0ull);

    uploadService.Shutdown();
}
