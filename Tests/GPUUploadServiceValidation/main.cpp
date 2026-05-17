#include "Core/Core.h"
#include "Render/GPUUploadService.h"
#include "RHI/RHICommandContext.h"
#include "RHI/RHIDevice.h"
#include "TestFramework/TestRunner.h"

#include <cstring>
#include <memory>
#include <vector>

using namespace RVX;
using namespace RVX::Test;

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
        uint32 beginCount = 0;
        uint32 endCount = 0;
        uint32 copyBufferCount = 0;
        uint32 copyBufferToTextureCount = 0;
        uint32 bufferBarrierCount = 0;
        uint32 textureBarrierCount = 0;

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
        void CopyBufferToTexture(RHIBuffer*, RHITexture*, const RHIBufferTextureCopyDesc&) override { ++copyBufferToTextureCount; }
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
            retainedCommandContext = RHICommandContextRef(new FakeCommandContext());
            lastCommandContext = static_cast<FakeCommandContext*>(retainedCommandContext.Get());
            return retainedCommandContext;
        }

        void SubmitCommandContext(RHICommandContext* context, RHIFence* signalFence) override
        {
            ++submittedCommandContextCount;
            lastSubmittedContext = context;
            lastSubmittedFence = signalFence;
        }

        void SubmitCommandContexts(std::span<RHICommandContext* const>, RHIFence*) override {}
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
        const RHICapabilities& GetCapabilities() const override { return capabilities; }
        RHIBackendType GetBackendType() const override { return RHIBackendType::None; }

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
        RHICapabilities capabilities;
        uint64 fenceInitialValueOverride = 0;
        bool signalLastFenceOnWaitIdle = false;
    };
} // namespace

bool Test_StagedBufferUploadsBatchUntilFlush()
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

    TEST_ASSERT_TRUE(firstResult.succeeded);
    TEST_ASSERT_TRUE(secondResult.succeeded);
    TEST_ASSERT_TRUE(firstResult.isPending);
    TEST_ASSERT_TRUE(secondResult.isPending);
    TEST_ASSERT_TRUE(uploadService.IsUploadPending(firstResult.uploadId));
    TEST_ASSERT_TRUE(uploadService.IsUploadPending(secondResult.uploadId));
    TEST_ASSERT_EQ(device.createdCommandContextCount, 1u);
    TEST_ASSERT_EQ(device.submittedCommandContextCount, 0u);
    TEST_ASSERT_EQ(device.createdFenceCount, 0u);
    TEST_ASSERT_NOT_NULL(device.lastCommandContext);
    TEST_ASSERT_EQ(device.lastCommandContext->beginCount, 1u);
    TEST_ASSERT_EQ(device.lastCommandContext->endCount, 0u);
    TEST_ASSERT_EQ(device.lastCommandContext->copyBufferCount, 2u);

    uploadService.FlushBatchUploads();

    TEST_ASSERT_EQ(device.submittedCommandContextCount, 1u);
    TEST_ASSERT_EQ(device.createdFenceCount, 1u);
    TEST_ASSERT_EQ(device.lastCommandContext->endCount, 1u);
    TEST_ASSERT_EQ(device.lastSubmittedContext, device.lastCommandContext);
    TEST_ASSERT_EQ(device.lastSubmittedFence, device.lastFence);

    TEST_ASSERT_EQ(uploadService.ProcessCompletedUploads(), 0u);
    TEST_ASSERT_NOT_NULL(device.lastFence);
    device.lastFence->Signal(1);
    TEST_ASSERT_EQ(uploadService.ProcessCompletedUploads(), 2u);
    TEST_ASSERT_TRUE(uploadService.IsUploadComplete(firstResult.uploadId));
    TEST_ASSERT_TRUE(uploadService.IsUploadComplete(secondResult.uploadId));

    const auto stats = uploadService.GetStats();
    TEST_ASSERT_EQ(stats.pendingUploadCount, 0u);
    TEST_ASSERT_EQ(stats.completedUploadCount, 2u);
    TEST_ASSERT_EQ(stats.stagingBytesInFlight, 0ull);

    uploadService.Shutdown();
    return true;
}

bool Test_ShutdownFlushesDirtyBatchAndWaitsForPendingUploads()
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

    TEST_ASSERT_TRUE(result.succeeded);
    TEST_ASSERT_TRUE(result.isPending);
    TEST_ASSERT_EQ(device.submittedCommandContextCount, 0u);
    TEST_ASSERT_EQ(device.waitIdleCount, 0u);

    uploadService.Shutdown();

    TEST_ASSERT_EQ(device.submittedCommandContextCount, 1u);
    TEST_ASSERT_EQ(device.createdFenceCount, 1u);
    TEST_ASSERT_EQ(device.waitIdleCount, 1u);
    TEST_ASSERT_EQ(device.lastCommandContext->endCount, 1u);
    return true;
}

bool Test_FlushTracksFenceNextCompletedValue()
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
    TEST_ASSERT_TRUE(result.succeeded);

    uploadService.FlushBatchUploads();

    TEST_ASSERT_EQ(uploadService.ProcessCompletedUploads(), 0u);
    TEST_ASSERT_NOT_NULL(device.lastFence);

    device.lastFence->Signal(4);
    TEST_ASSERT_EQ(uploadService.ProcessCompletedUploads(), 0u);
    TEST_ASSERT_TRUE(uploadService.IsUploadPending(result.uploadId));

    device.lastFence->Signal(5);
    TEST_ASSERT_EQ(uploadService.ProcessCompletedUploads(), 1u);
    TEST_ASSERT_TRUE(uploadService.IsUploadComplete(result.uploadId));

    uploadService.ForgetCompletedUpload(result.uploadId);
    TEST_ASSERT_TRUE(!uploadService.IsUploadComplete(result.uploadId));

    uploadService.Shutdown();
    return true;
}

bool Test_CompletedFenceIsReusedForNextBatch()
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
    TEST_ASSERT_TRUE(firstResult.succeeded);
    uploadService.FlushBatchUploads();

    TEST_ASSERT_EQ(device.createdFenceCount, 1u);
    TEST_ASSERT_NOT_NULL(device.lastFence);
    RHIFence* firstFence = device.lastFence;

    device.lastFence->Signal(1);
    TEST_ASSERT_EQ(uploadService.ProcessCompletedUploads(), 1u);
    TEST_ASSERT_TRUE(uploadService.IsUploadComplete(firstResult.uploadId));

    auto secondResult = uploadService.UploadBufferDataWithResult(desc, second, sizeof(second));
    TEST_ASSERT_TRUE(secondResult.succeeded);
    uploadService.FlushBatchUploads();

    TEST_ASSERT_EQ(device.createdFenceCount, 1u);
    TEST_ASSERT_EQ(device.lastSubmittedFence, firstFence);

    device.lastFence->Signal(1);
    TEST_ASSERT_EQ(uploadService.ProcessCompletedUploads(), 0u);
    TEST_ASSERT_TRUE(uploadService.IsUploadPending(secondResult.uploadId));

    device.lastFence->Signal(2);
    TEST_ASSERT_EQ(uploadService.ProcessCompletedUploads(), 1u);
    TEST_ASSERT_TRUE(uploadService.IsUploadComplete(secondResult.uploadId));

    uploadService.Shutdown();
    return true;
}

bool Test_FlushAndWaitSubmitsDirtyBatchAndCompletesUploads()
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
    TEST_ASSERT_TRUE(result.succeeded);
    TEST_ASSERT_TRUE(result.isPending);
    TEST_ASSERT_EQ(device.submittedCommandContextCount, 0u);
    TEST_ASSERT_TRUE(uploadService.IsUploadPending(result.uploadId));

    TEST_ASSERT_EQ(uploadService.FlushAndWaitForUploads(), 1u);

    TEST_ASSERT_EQ(device.submittedCommandContextCount, 1u);
    TEST_ASSERT_EQ(device.waitIdleCount, 1u);
    TEST_ASSERT_TRUE(!uploadService.IsUploadPending(result.uploadId));
    TEST_ASSERT_TRUE(uploadService.IsUploadComplete(result.uploadId));

    const auto stats = uploadService.GetStats();
    TEST_ASSERT_EQ(stats.pendingUploadCount, 0u);
    TEST_ASSERT_EQ(stats.completedUploadCount, 1u);
    TEST_ASSERT_EQ(stats.stagingBytesInFlight, 0ull);

    uploadService.Shutdown();
    return true;
}

bool Test_UnsupportedTextureLayoutDoesNotCreateEmptyTexture()
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

    TEST_ASSERT_TRUE(!result.succeeded);
    TEST_ASSERT_TRUE(!result.resource);
    TEST_ASSERT_EQ(result.mode, GPUUploadMode::None);
    TEST_ASSERT_EQ(result.failureReason, GPUUploadFailureReason::Unsupported);
    TEST_ASSERT_EQ(device.createdTextureCount, 0u);

    const auto stats = uploadService.GetStats();
    TEST_ASSERT_EQ(stats.failedUploadCount, 1u);
    TEST_ASSERT_EQ(stats.textureUploadCount, 0u);
    TEST_ASSERT_EQ(stats.uploadedBytes, 0ull);

    uploadService.Shutdown();
    return true;
}

bool Test_AbandonedUploadDoesNotRemainCompleted()
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
    TEST_ASSERT_TRUE(result.succeeded);
    TEST_ASSERT_TRUE(result.isPending);

    uploadService.AbandonUpload(result.uploadId);
    uploadService.FlushBatchUploads();

    TEST_ASSERT_NOT_NULL(device.lastFence);
    device.lastFence->Signal(1);
    TEST_ASSERT_EQ(uploadService.ProcessCompletedUploads(), 1u);
    TEST_ASSERT_TRUE(!uploadService.IsUploadPending(result.uploadId));
    TEST_ASSERT_TRUE(!uploadService.IsUploadComplete(result.uploadId));

    const auto stats = uploadService.GetStats();
    TEST_ASSERT_EQ(stats.pendingUploadCount, 0u);
    TEST_ASSERT_EQ(stats.stagingBytesInFlight, 0ull);

    uploadService.Shutdown();
    return true;
}

int main()
{
    Log::Initialize();
    RVX_CORE_INFO("GPUUploadService Validation Tests");

    TestSuite suite;
    suite.AddTest("StagedBufferUploadsBatchUntilFlush", Test_StagedBufferUploadsBatchUntilFlush);
    suite.AddTest("ShutdownFlushesDirtyBatchAndWaitsForPendingUploads", Test_ShutdownFlushesDirtyBatchAndWaitsForPendingUploads);
    suite.AddTest("FlushTracksFenceNextCompletedValue", Test_FlushTracksFenceNextCompletedValue);
    suite.AddTest("CompletedFenceIsReusedForNextBatch", Test_CompletedFenceIsReusedForNextBatch);
    suite.AddTest("FlushAndWaitSubmitsDirtyBatchAndCompletesUploads", Test_FlushAndWaitSubmitsDirtyBatchAndCompletesUploads);
    suite.AddTest("UnsupportedTextureLayoutDoesNotCreateEmptyTexture", Test_UnsupportedTextureLayoutDoesNotCreateEmptyTexture);
    suite.AddTest("AbandonedUploadDoesNotRemainCompleted", Test_AbandonedUploadDoesNotRemainCompleted);

    auto results = suite.Run();
    suite.PrintResults(results);

    Log::Shutdown();

    for (const auto& result : results)
    {
        if (!result.passed)
            return 1;
    }

    return 0;
}
