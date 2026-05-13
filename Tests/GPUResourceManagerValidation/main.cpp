#include "Core/Core.h"
#include "Render/GPUResourceManager.h"
#include "Render/GPUUploadService.h"
#include "Resource/Types/MeshResource.h"
#include "Resource/Types/TextureResource.h"
#include "RHI/RHICommandContext.h"
#include "RHI/RHIDevice.h"
#include "Scene/Mesh.h"
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

    class FakeStagingBuffer final : public RHIStagingBuffer
    {
    public:
        FakeStagingBuffer(const RHIStagingBufferDesc& desc, bool mapSucceeds = true)
            : m_desc(desc)
            , m_mapSucceeds(mapSucceeds)
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
            if (!m_mapSucceeds)
                return nullptr;

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
        bool m_mapSucceeds = true;
    };

    class FakeCommandContext final : public RHICommandContext
    {
    public:
        uint32 beginCount = 0;
        uint32 endCount = 0;
        uint32 bufferBarrierCount = 0;
        uint32 textureBarrierCount = 0;
        uint32 copyBufferCount = 0;
        uint32 copyBufferToTextureCount = 0;
        RHIBufferBarrier lastBufferBarrier;
        RHITextureBarrier lastTextureBarrier;
        RHIBuffer* lastCopySrc = nullptr;
        RHIBuffer* lastCopyDst = nullptr;
        RHITexture* lastTextureCopyDst = nullptr;
        uint64 lastCopySize = 0;
        RHIBufferTextureCopyDesc lastBufferTextureCopyDesc;

        void Begin() override { ++beginCount; }
        void End() override { ++endCount; }
        void Reset() override {}
        void BeginEvent(const char*, uint32 = 0) override {}
        void EndEvent() override {}
        void SetMarker(const char*, uint32 = 0) override {}
        void BufferBarrier(const RHIBufferBarrier& barrier) override
        {
            ++bufferBarrierCount;
            lastBufferBarrier = barrier;
        }
        void TextureBarrier(const RHITextureBarrier& barrier) override
        {
            ++textureBarrierCount;
            lastTextureBarrier = barrier;
        }
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

        void CopyBuffer(RHIBuffer* src, RHIBuffer* dst, uint64, uint64, uint64 size) override
        {
            ++copyBufferCount;
            lastCopySrc = src;
            lastCopyDst = dst;
            lastCopySize = size;
        }

        void CopyTexture(RHITexture*, RHITexture*, const RHITextureCopyDesc& = {}) override {}
        void CopyBufferToTexture(RHIBuffer* src, RHITexture* dst, const RHIBufferTextureCopyDesc& desc) override
        {
            ++copyBufferToTextureCount;
            lastCopySrc = src;
            lastTextureCopyDst = dst;
            lastBufferTextureCopyDesc = desc;
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

            return RHIBufferRef(new FakeBuffer(desc));
        }

        RHITextureRef CreateTexture(const RHITextureDesc& desc) override
        {
            ++createdTextureCount;
            if (failTextureCreation)
                return nullptr;

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
            if (!supportStagedCopy)
                return nullptr;

            retainedCommandContext = RHICommandContextRef(new FakeCommandContext());
            lastCommandContext = static_cast<FakeCommandContext*>(retainedCommandContext.Get());
            return retainedCommandContext;
        }

        void SubmitCommandContext(RHICommandContext*, RHIFence* signalFence) override
        {
            ++submittedCommandContextCount;
            lastSubmittedFence = signalFence;
            if (completeSubmittedFenceImmediately && signalFence)
            {
                signalFence->Signal(1);
            }
        }
        void SubmitCommandContexts(std::span<RHICommandContext* const>, RHIFence*) override {}
        RHISwapChainRef CreateSwapChain(const RHISwapChainDesc&) override { return nullptr; }
        RHIFenceRef CreateFence(uint64 initialValue) override
        {
            ++createdFenceCount;
            retainedFences.push_back(RHIFenceRef(new FakeFence(initialValue)));
            lastFence = static_cast<FakeFence*>(retainedFences.back().Get());
            fences.push_back(lastFence);
            return retainedFences.back();
        }
        void WaitForFence(RHIFence*, uint64) override {}
        void WaitIdle() override
        {
            ++waitIdleCount;
            for (FakeFence* fence : fences)
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
            if (!supportStagedCopy)
                return nullptr;

            return RHIStagingBufferRef(new FakeStagingBuffer(desc, stagingMapSucceeds));
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
        RHICommandQueueType lastCommandQueueType = RHICommandQueueType::Copy;
        bool failBufferCreation = false;
        bool failTextureCreation = false;
        bool supportStagedCopy = false;
        bool stagingMapSucceeds = true;
        bool completeSubmittedFenceImmediately = false;
        FakeCommandContext* lastCommandContext = nullptr;
        FakeFence* lastFence = nullptr;
        std::vector<FakeFence*> fences;
        RHIFence* lastSubmittedFence = nullptr;
        RHICommandContextRef retainedCommandContext;
        std::vector<RHIFenceRef> retainedFences;
        RHICapabilities capabilities;
    };

    std::unique_ptr<Resource::MeshResource> CreateMeshResource(Resource::ResourceId id, std::shared_ptr<Mesh> mesh)
    {
        auto resource = std::make_unique<Resource::MeshResource>();
        resource->SetId(id);
        resource->SetName("TestMesh");
        resource->SetMesh(std::move(mesh));
        return resource;
    }

    std::shared_ptr<Mesh> CreatePositionOnlyMesh()
    {
        auto mesh = std::make_shared<Mesh>();
        mesh->SetPositions({
            {0.0f, 0.5f, 0.0f},
            {-0.5f, -0.5f, 0.0f},
            {0.5f, -0.5f, 0.0f}
        });
        return mesh;
    }

    std::unique_ptr<Resource::TextureResource> CreateTextureResource(Resource::ResourceId id)
    {
        auto resource = std::make_unique<Resource::TextureResource>();
        resource->SetId(id);
        resource->SetName("TestTexture");

        Resource::TextureMetadata metadata;
        metadata.width = 1;
        metadata.height = 1;
        metadata.format = Resource::TextureFormat::RGBA8;
        metadata.isSRGB = false;

        resource->SetData({255, 255, 255, 255}, metadata);
        return resource;
    }
} // namespace

bool Test_DuplicateQueuedUploadIsIgnored()
{
    FakeDevice device;
    GPUResourceManager manager;
    manager.Initialize(&device);

    auto mesh = Resource::ResourceHandle<Resource::MeshResource>(
        CreateMeshResource(101, MeshFactory::CreateTriangle()).release());

    manager.RequestUpload(mesh.Get());
    manager.RequestUpload(mesh.Get(), UploadPriority::Immediate);

    const auto stats = manager.GetStats();
    TEST_ASSERT_EQ(stats.pendingUploadCount, 1u);
    TEST_ASSERT_EQ(stats.queuedUploadCount, 1u);
    TEST_ASSERT_EQ(manager.GetResourceState(mesh->GetId()), GPUResourceState::UploadQueued);

    manager.Shutdown();
    return true;
}

bool Test_UnmanagedResourceIsNotQueuedForAsyncUpload()
{
    FakeDevice device;
    GPUResourceManager manager;
    manager.Initialize(&device);

    auto mesh = CreateMeshResource(109, MeshFactory::CreateTriangle());
    manager.RequestUpload(mesh.get());

    const auto stats = manager.GetStats();
    TEST_ASSERT_EQ(stats.pendingUploadCount, 0u);
    TEST_ASSERT_EQ(manager.GetResourceState(mesh->GetId()), GPUResourceState::Unloaded);

    manager.Shutdown();
    return true;
}

bool Test_QueuedUploadRetainsRefCountedResource()
{
    FakeDevice device;
    GPUResourceManager manager;
    manager.Initialize(&device);

    auto meshHandle = Resource::ResourceHandle<Resource::MeshResource>(
        CreateMeshResource(107, MeshFactory::CreateTriangle()).release());
    Resource::MeshResource* mesh = meshHandle.Get();

    TEST_ASSERT_EQ(mesh->GetRefCount(), 1u);
    manager.RequestUpload(mesh);
    TEST_ASSERT_EQ(mesh->GetRefCount(), 2u);

    const Resource::ResourceId meshId = mesh->GetId();
    meshHandle.Reset();

    manager.ProcessPendingUploads();

    TEST_ASSERT_EQ(manager.GetResourceState(meshId), GPUResourceState::GPUReady);
    TEST_ASSERT_TRUE(manager.IsResident(meshId));

    manager.Shutdown();
    return true;
}

bool Test_ProcessPendingUploadsWithZeroBudgetDoesNotStartNewUpload()
{
    FakeDevice device;
    GPUResourceManager manager;
    manager.Initialize(&device);

    auto mesh = Resource::ResourceHandle<Resource::MeshResource>(
        CreateMeshResource(108, MeshFactory::CreateTriangle()).release());
    manager.RequestUpload(mesh.Get());

    manager.ProcessPendingUploads(0.0f);

    TEST_ASSERT_EQ(device.createdBufferCount, 0u);
    TEST_ASSERT_EQ(manager.GetResourceState(mesh->GetId()), GPUResourceState::UploadQueued);
    {
        const auto stats = manager.GetStats();
        TEST_ASSERT_EQ(stats.pendingUploadCount, 1ull);
        TEST_ASSERT_EQ(stats.queuedUploadCount, 1ull);
    }

    manager.ProcessPendingUploads();

    TEST_ASSERT_TRUE(manager.IsGPUReady(mesh->GetId()));

    manager.Shutdown();
    return true;
}

bool Test_GPUUploadServiceUploadsBufferData()
{
    FakeDevice device;
    GPUUploadService uploadService;
    uploadService.Initialize(&device);

    const uint32 source[] = {1, 2, 3, 4};
    GPUUploadBufferDesc desc;
    desc.size = sizeof(source);
    desc.usage = RHIBufferUsage::Vertex;
    desc.stride = sizeof(uint32);
    desc.debugName = "UploadServiceBuffer";

    auto buffer = uploadService.UploadBufferData(desc, source, sizeof(source));

    TEST_ASSERT_NOT_NULL(buffer.Get());
    TEST_ASSERT_EQ(buffer->GetSize(), static_cast<uint64>(sizeof(source)));
    TEST_ASSERT_EQ(buffer->GetUsage(), RHIBufferUsage::Vertex);
    TEST_ASSERT_EQ(buffer->GetMemoryType(), RHIMemoryType::Upload);
    TEST_ASSERT_EQ(buffer->GetStride(), static_cast<uint32>(sizeof(uint32)));

    auto* fakeBuffer = dynamic_cast<FakeBuffer*>(buffer.Get());
    TEST_ASSERT_NOT_NULL(fakeBuffer);
    TEST_ASSERT_TRUE(std::memcmp(fakeBuffer->GetStorage().data(), source, sizeof(source)) == 0);

    const auto stats = uploadService.GetStats();
    TEST_ASSERT_EQ(stats.bufferUploadCount, 1u);
    TEST_ASSERT_EQ(stats.failedUploadCount, 0u);
    TEST_ASSERT_EQ(stats.uploadedBytes, static_cast<uint64>(sizeof(source)));

    uploadService.Shutdown();
    return true;
}

bool Test_GPUUploadServiceRejectsInvalidBufferUpload()
{
    FakeDevice device;
    GPUUploadService uploadService;
    uploadService.Initialize(&device);

    GPUUploadBufferDesc desc;
    desc.size = 16;
    desc.usage = RHIBufferUsage::Vertex;

    auto buffer = uploadService.UploadBufferData(desc, nullptr, desc.size);

    TEST_ASSERT_TRUE(!buffer);

    const auto stats = uploadService.GetStats();
    TEST_ASSERT_EQ(stats.bufferUploadCount, 0u);
    TEST_ASSERT_EQ(stats.failedUploadCount, 1u);

    uploadService.Shutdown();
    return true;
}

bool Test_GPUUploadServiceReportsInvalidBufferData()
{
    FakeDevice device;
    GPUUploadService uploadService;
    uploadService.Initialize(&device);

    GPUUploadBufferDesc desc;
    desc.size = 16;
    desc.usage = RHIBufferUsage::Vertex;

    auto result = uploadService.UploadBufferDataWithResult(desc, nullptr, desc.size);

    TEST_ASSERT_FALSE(result.succeeded);
    TEST_ASSERT_TRUE(!result.resource);
    TEST_ASSERT_EQ(result.mode, GPUUploadMode::None);
    TEST_ASSERT_EQ(result.failureReason, GPUUploadFailureReason::InvalidData);
    TEST_ASSERT_EQ(result.bytesUploaded, 0ull);

    const auto stats = uploadService.GetStats();
    TEST_ASSERT_EQ(stats.failedUploadCount, 1u);
    TEST_ASSERT_EQ(stats.immediateUploadCount, 0u);
    TEST_ASSERT_EQ(stats.uploadedBytes, 0ull);

    uploadService.Shutdown();
    return true;
}

bool Test_GPUUploadServiceReportsBufferCreationFailure()
{
    FakeDevice device;
    device.failBufferCreation = true;

    GPUUploadService uploadService;
    uploadService.Initialize(&device);

    const uint32 source[] = {1, 2, 3, 4};
    GPUUploadBufferDesc desc;
    desc.size = sizeof(source);
    desc.usage = RHIBufferUsage::Vertex;

    auto result = uploadService.UploadBufferDataWithResult(desc, source, sizeof(source));

    TEST_ASSERT_FALSE(result.succeeded);
    TEST_ASSERT_TRUE(!result.resource);
    TEST_ASSERT_EQ(result.mode, GPUUploadMode::None);
    TEST_ASSERT_EQ(result.failureReason, GPUUploadFailureReason::CreateResourceFailed);
    TEST_ASSERT_EQ(result.bytesUploaded, 0ull);

    const auto stats = uploadService.GetStats();
    TEST_ASSERT_EQ(stats.failedUploadCount, 1u);
    TEST_ASSERT_EQ(stats.bufferUploadCount, 0u);

    uploadService.Shutdown();
    return true;
}

bool Test_GPUUploadServiceUsesStagedCopyForBufferWhenAvailable()
{
    FakeDevice device;
    device.supportStagedCopy = true;

    GPUUploadService uploadService;
    uploadService.Initialize(&device);

    const uint32 source[] = {10, 20, 30, 40};
    GPUUploadBufferDesc desc;
    desc.size = sizeof(source);
    desc.usage = RHIBufferUsage::Vertex;
    desc.stride = sizeof(uint32);
    desc.debugName = "StagedBuffer";

    auto result = uploadService.UploadBufferDataWithResult(desc, source, sizeof(source));

    TEST_ASSERT_TRUE(result.succeeded);
    TEST_ASSERT_NOT_NULL(result.resource.Get());
    TEST_ASSERT_EQ(result.mode, GPUUploadMode::StagedCopy);
    TEST_ASSERT_EQ(result.failureReason, GPUUploadFailureReason::None);
    TEST_ASSERT_EQ(result.bytesUploaded, static_cast<uint64>(sizeof(source)));
    TEST_ASSERT_EQ(result.resource->GetMemoryType(), RHIMemoryType::Default);
    TEST_ASSERT_EQ(result.resource->GetUsage() & RHIBufferUsage::CopyDst, RHIBufferUsage::CopyDst);

    TEST_ASSERT_NOT_NULL(device.lastCommandContext);
    TEST_ASSERT_EQ(device.lastCommandQueueType, RHICommandQueueType::Copy);
    TEST_ASSERT_EQ(device.lastCommandContext->beginCount, 1u);
    TEST_ASSERT_EQ(device.lastCommandContext->endCount, 0u);
    TEST_ASSERT_EQ(device.submittedCommandContextCount, 0u);
    TEST_ASSERT_EQ(device.createdFenceCount, 0u);

    uploadService.FlushBatchUploads();

    TEST_ASSERT_EQ(device.lastCommandContext->endCount, 1u);
    TEST_ASSERT_EQ(device.lastCommandContext->copyBufferCount, 1u);
    TEST_ASSERT_EQ(device.lastCommandContext->lastCopyDst, result.resource.Get());
    TEST_ASSERT_EQ(device.lastCommandContext->lastCopySize, static_cast<uint64>(sizeof(source)));
    TEST_ASSERT_EQ(device.lastCommandContext->bufferBarrierCount, 1u);
    TEST_ASSERT_EQ(device.lastCommandContext->lastBufferBarrier.buffer, result.resource.Get());
    TEST_ASSERT_EQ(device.lastCommandContext->lastBufferBarrier.stateBefore, RHIResourceState::CopyDest);
    TEST_ASSERT_EQ(device.lastCommandContext->lastBufferBarrier.stateAfter, RHIResourceState::Common);
    TEST_ASSERT_EQ(device.submittedCommandContextCount, 1u);
    TEST_ASSERT_EQ(result.isPending, true);
    TEST_ASSERT_TRUE(result.uploadId != 0);
    TEST_ASSERT_EQ(device.createdFenceCount, 1u);
    TEST_ASSERT_EQ(device.waitIdleCount, 0u);

    const auto stats = uploadService.GetStats();
    TEST_ASSERT_EQ(stats.bufferUploadCount, 1u);
    TEST_ASSERT_EQ(stats.stagedUploadCount, 1u);
    TEST_ASSERT_EQ(stats.immediateUploadCount, 0u);
    TEST_ASSERT_EQ(stats.pendingUploadCount, 1u);
    TEST_ASSERT_EQ(stats.stagingBytesInFlight, static_cast<uint64>(sizeof(source)));
    TEST_ASSERT_EQ(stats.uploadedBytes, static_cast<uint64>(sizeof(source)));

    uploadService.Shutdown();
    return true;
}

bool Test_GPUUploadServiceCompletesPendingBufferUploadAfterFence()
{
    FakeDevice device;
    device.supportStagedCopy = true;

    GPUUploadService uploadService;
    uploadService.Initialize(&device);

    const uint32 source[] = {10, 20, 30, 40};
    GPUUploadBufferDesc desc;
    desc.size = sizeof(source);
    desc.usage = RHIBufferUsage::Vertex;

    auto result = uploadService.UploadBufferDataWithResult(desc, source, sizeof(source));

    TEST_ASSERT_TRUE(result.succeeded);
    TEST_ASSERT_TRUE(result.isPending);
    TEST_ASSERT_TRUE(uploadService.IsUploadPending(result.uploadId));
    TEST_ASSERT_FALSE(uploadService.IsUploadComplete(result.uploadId));

    uploadService.FlushBatchUploads();

    TEST_ASSERT_EQ(uploadService.ProcessCompletedUploads(), 0u);
    TEST_ASSERT_TRUE(uploadService.IsUploadPending(result.uploadId));

    TEST_ASSERT_NOT_NULL(device.lastFence);
    device.lastFence->Signal(1);

    TEST_ASSERT_EQ(uploadService.ProcessCompletedUploads(), 1u);
    TEST_ASSERT_FALSE(uploadService.IsUploadPending(result.uploadId));
    TEST_ASSERT_TRUE(uploadService.IsUploadComplete(result.uploadId));

    const auto stats = uploadService.GetStats();
    TEST_ASSERT_EQ(stats.pendingUploadCount, 0u);
    TEST_ASSERT_EQ(stats.completedUploadCount, 1u);
    TEST_ASSERT_EQ(stats.stagingBytesInFlight, 0ull);

    uploadService.Shutdown();
    return true;
}

bool Test_GPUUploadServiceFallsBackWhenStagingMapFails()
{
    FakeDevice device;
    device.supportStagedCopy = true;
    device.stagingMapSucceeds = false;

    GPUUploadService uploadService;
    uploadService.Initialize(&device);

    const uint32 source[] = {1, 2, 3, 4};
    GPUUploadBufferDesc desc;
    desc.size = sizeof(source);
    desc.usage = RHIBufferUsage::Vertex;

    auto result = uploadService.UploadBufferDataWithResult(desc, source, sizeof(source));

    TEST_ASSERT_TRUE(result.succeeded);
    TEST_ASSERT_EQ(result.mode, GPUUploadMode::ImmediateMapped);
    TEST_ASSERT_EQ(result.failureReason, GPUUploadFailureReason::None);
    TEST_ASSERT_FALSE(result.isPending);
    TEST_ASSERT_EQ(result.uploadId, 0ull);
    TEST_ASSERT_EQ(result.resource->GetMemoryType(), RHIMemoryType::Upload);

    const auto stats = uploadService.GetStats();
    TEST_ASSERT_EQ(stats.failedUploadCount, 0u);
    TEST_ASSERT_EQ(stats.bufferUploadCount, 1u);
    TEST_ASSERT_EQ(stats.immediateUploadCount, 1u);
    TEST_ASSERT_EQ(stats.stagedUploadCount, 0u);

    uploadService.Shutdown();
    return true;
}

bool Test_GPUUploadServiceRejectsTextureWhenStagedCopyUnavailable()
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
    desc.dataSize = sizeof(pixels);

    auto result = uploadService.UploadTextureDataWithResult(desc, pixels);

    TEST_ASSERT_FALSE(result.succeeded);
    TEST_ASSERT_TRUE(!result.resource);
    TEST_ASSERT_EQ(result.mode, GPUUploadMode::None);
    TEST_ASSERT_EQ(result.failureReason, GPUUploadFailureReason::Unsupported);
    TEST_ASSERT_EQ(result.bytesUploaded, 0ull);

    const auto stats = uploadService.GetStats();
    TEST_ASSERT_EQ(stats.textureUploadCount, 0u);
    TEST_ASSERT_EQ(stats.failedUploadCount, 1u);
    TEST_ASSERT_EQ(stats.immediateUploadCount, 0u);
    TEST_ASSERT_EQ(stats.uploadedBytes, 0ull);

    uploadService.Shutdown();
    return true;
}

bool Test_GPUUploadServiceUsesStagedCopyForTextureWhenAvailable()
{
    FakeDevice device;
    device.supportStagedCopy = true;

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
    desc.dataSize = sizeof(pixels);

    auto result = uploadService.UploadTextureDataWithResult(desc, pixels);

    TEST_ASSERT_TRUE(result.succeeded);
    TEST_ASSERT_NOT_NULL(result.resource.Get());
    TEST_ASSERT_EQ(result.mode, GPUUploadMode::StagedCopy);
    TEST_ASSERT_EQ(result.failureReason, GPUUploadFailureReason::None);
    TEST_ASSERT_EQ(result.bytesUploaded, static_cast<uint64>(sizeof(pixels)));

    TEST_ASSERT_NOT_NULL(device.lastCommandContext);
    TEST_ASSERT_EQ(device.lastCommandQueueType, RHICommandQueueType::Copy);
    TEST_ASSERT_EQ(device.lastCommandContext->beginCount, 1u);
    TEST_ASSERT_EQ(device.lastCommandContext->endCount, 0u);
    TEST_ASSERT_EQ(device.submittedCommandContextCount, 0u);
    TEST_ASSERT_EQ(device.createdFenceCount, 0u);

    uploadService.FlushBatchUploads();

    TEST_ASSERT_EQ(device.lastCommandContext->endCount, 1u);
    TEST_ASSERT_EQ(device.lastCommandContext->copyBufferToTextureCount, 1u);
    TEST_ASSERT_EQ(device.lastCommandContext->lastTextureCopyDst, result.resource.Get());
    TEST_ASSERT_EQ(device.lastCommandContext->lastBufferTextureCopyDesc.bufferRowPitch, 256u);
    TEST_ASSERT_EQ(device.lastCommandContext->lastBufferTextureCopyDesc.textureRegion.width, 2u);
    TEST_ASSERT_EQ(device.lastCommandContext->lastBufferTextureCopyDesc.textureRegion.height, 2u);
    TEST_ASSERT_EQ(device.lastCommandContext->textureBarrierCount, 1u);
    TEST_ASSERT_EQ(device.lastCommandContext->lastTextureBarrier.texture, result.resource.Get());
    TEST_ASSERT_EQ(device.lastCommandContext->lastTextureBarrier.stateBefore, RHIResourceState::CopyDest);
    TEST_ASSERT_EQ(device.lastCommandContext->lastTextureBarrier.stateAfter, RHIResourceState::Common);
    TEST_ASSERT_EQ(device.submittedCommandContextCount, 1u);
    TEST_ASSERT_EQ(result.isPending, true);
    TEST_ASSERT_TRUE(result.uploadId != 0);
    TEST_ASSERT_EQ(device.createdFenceCount, 1u);
    TEST_ASSERT_EQ(device.waitIdleCount, 0u);

    const auto stats = uploadService.GetStats();
    TEST_ASSERT_EQ(stats.textureUploadCount, 1u);
    TEST_ASSERT_EQ(stats.stagedUploadCount, 1u);
    TEST_ASSERT_EQ(stats.immediateUploadCount, 0u);
    TEST_ASSERT_EQ(stats.pendingUploadCount, 1u);
    TEST_ASSERT_EQ(stats.stagingBytesInFlight, 512ull);
    TEST_ASSERT_EQ(stats.uploadedBytes, static_cast<uint64>(sizeof(pixels)));

    uploadService.Shutdown();
    return true;
}

bool Test_GPUUploadServiceRejectsInvalidTextureDimensions()
{
    FakeDevice device;
    GPUUploadService uploadService;
    uploadService.Initialize(&device);

    const uint8 pixels[] = {255, 255, 255, 255};
    GPUUploadTextureDesc desc;
    desc.textureDesc = RHITextureDesc::Texture2D(0, 1, RHIFormat::RGBA8_UNORM);
    desc.dataSize = sizeof(pixels);

    auto result = uploadService.UploadTextureDataWithResult(desc, pixels);

    TEST_ASSERT_FALSE(result.succeeded);
    TEST_ASSERT_TRUE(!result.resource);
    TEST_ASSERT_EQ(result.mode, GPUUploadMode::None);
    TEST_ASSERT_EQ(result.failureReason, GPUUploadFailureReason::InvalidDescription);
    TEST_ASSERT_EQ(result.bytesUploaded, 0ull);

    const auto stats = uploadService.GetStats();
    TEST_ASSERT_EQ(stats.failedUploadCount, 1u);
    TEST_ASSERT_EQ(stats.textureUploadCount, 0u);

    uploadService.Shutdown();
    return true;
}

bool Test_GPUUploadServiceRejectsUnsupportedTextureFormat()
{
    FakeDevice device;
    GPUUploadService uploadService;
    uploadService.Initialize(&device);

    const uint8 pixels[] = {0, 0, 0, 0};
    GPUUploadTextureDesc desc;
    desc.textureDesc = RHITextureDesc::Texture2D(1, 1, RHIFormat::Unknown);
    desc.dataSize = sizeof(pixels);

    auto result = uploadService.UploadTextureDataWithResult(desc, pixels);

    TEST_ASSERT_FALSE(result.succeeded);
    TEST_ASSERT_TRUE(!result.resource);
    TEST_ASSERT_EQ(result.mode, GPUUploadMode::None);
    TEST_ASSERT_EQ(result.failureReason, GPUUploadFailureReason::Unsupported);
    TEST_ASSERT_EQ(result.bytesUploaded, 0ull);

    const auto stats = uploadService.GetStats();
    TEST_ASSERT_EQ(stats.failedUploadCount, 1u);
    TEST_ASSERT_EQ(stats.textureUploadCount, 0u);

    uploadService.Shutdown();
    return true;
}

bool Test_MeshWithoutIndexDataFailsUpload()
{
    FakeDevice device;
    GPUResourceManager manager;
    manager.Initialize(&device);

    auto mesh = CreateMeshResource(102, CreatePositionOnlyMesh());

    manager.UploadImmediate(mesh.get());

    TEST_ASSERT_EQ(manager.GetResourceState(mesh->GetId()), GPUResourceState::Failed);
    TEST_ASSERT_FALSE(manager.IsResident(mesh->GetId()));
    TEST_ASSERT_FALSE(manager.IsGPUReady(mesh->GetId()));
    TEST_ASSERT_FALSE(manager.GetMeshBuffers(mesh->GetId()).IsValid());

    manager.Shutdown();
    return true;
}

bool Test_ValidMeshUploadBecomesGPUReady()
{
    FakeDevice device;
    GPUResourceManager manager;
    manager.Initialize(&device);

    auto mesh = CreateMeshResource(103, MeshFactory::CreateTriangle());

    manager.UploadImmediate(mesh.get());

    const auto buffers = manager.GetMeshBuffers(mesh->GetId());
    TEST_ASSERT_EQ(manager.GetResourceState(mesh->GetId()), GPUResourceState::GPUReady);
    TEST_ASSERT_TRUE(manager.IsResident(mesh->GetId()));
    TEST_ASSERT_TRUE(manager.IsGPUReady(mesh->GetId()));
    TEST_ASSERT_TRUE(buffers.IsValid());
    TEST_ASSERT_TRUE(device.createdBufferCount >= 2);

    manager.Shutdown();
    return true;
}

bool Test_StagedMeshUploadImmediateWaitsForFenceCompletion()
{
    FakeDevice device;
    device.supportStagedCopy = true;

    GPUResourceManager manager;
    manager.Initialize(&device);

    auto mesh = CreateMeshResource(104, MeshFactory::CreateTriangle());

    manager.UploadImmediate(mesh.get());

    TEST_ASSERT_EQ(manager.GetResourceState(mesh->GetId()), GPUResourceState::GPUReady);
    TEST_ASSERT_TRUE(manager.IsResident(mesh->GetId()));
    TEST_ASSERT_TRUE(manager.IsGPUReady(mesh->GetId()));
    TEST_ASSERT_TRUE(manager.GetMeshBuffers(mesh->GetId()).IsValid());
    TEST_ASSERT_EQ(device.waitIdleCount, 1u);
    {
        const auto stats = manager.GetStats();
        TEST_ASSERT_EQ(stats.residentMeshCount, 1ull);
        TEST_ASSERT_EQ(stats.uploadingCount, 0ull);
    }

    manager.Shutdown();
    return true;
}

bool Test_TransitionTextureTransitionsResidentTextureOnce()
{
    FakeDevice device;
    device.supportStagedCopy = true;
    device.completeSubmittedFenceImmediately = true;

    GPUResourceManager manager;
    manager.Initialize(&device);

    auto texture = CreateTextureResource(105);
    manager.UploadImmediate(texture.get());
    manager.ProcessPendingUploads();

    TEST_ASSERT_EQ(manager.GetResourceState(texture->GetId()), GPUResourceState::GPUReady);
    TEST_ASSERT_TRUE(manager.IsResident(texture->GetId()));

    FakeCommandContext ctx;
    TEST_ASSERT_TRUE(manager.TransitionTexture(texture->GetId(), ctx, RHIResourceState::ShaderResource));
    TEST_ASSERT_EQ(ctx.textureBarrierCount, 1u);
    TEST_ASSERT_EQ(ctx.lastTextureBarrier.texture, manager.GetTexture(texture->GetId()));
    TEST_ASSERT_EQ(ctx.lastTextureBarrier.stateBefore, RHIResourceState::Common);
    TEST_ASSERT_EQ(ctx.lastTextureBarrier.stateAfter, RHIResourceState::ShaderResource);

    TEST_ASSERT_TRUE(manager.TransitionTexture(texture->GetId(), ctx, RHIResourceState::ShaderResource));
    TEST_ASSERT_EQ(ctx.textureBarrierCount, 1u);

    TEST_ASSERT_FALSE(manager.TransitionTexture(Resource::InvalidResourceId, ctx, RHIResourceState::ShaderResource));

    manager.Shutdown();
    return true;
}

bool Test_TextureEvictionNotifiesViewCachesBeforeRelease()
{
    FakeDevice device;
    device.supportStagedCopy = true;

    GPUResourceManager manager;
    manager.Initialize(&device);

    uint32 invalidatedCount = 0;
    RHITexture* invalidatedTexture = nullptr;
    manager.SetTextureInvalidatedCallback(
        [&invalidatedCount, &invalidatedTexture](RHITexture* texture)
        {
            ++invalidatedCount;
            invalidatedTexture = texture;
        });

    auto texture = CreateTextureResource(110);
    manager.UploadImmediate(texture.get());

    RHITexture* residentTexture = manager.GetTexture(texture->GetId());
    TEST_ASSERT_NOT_NULL(residentTexture);
    TEST_ASSERT_EQ(invalidatedCount, 0u);

    manager.EvictUnused(10, 1);

    TEST_ASSERT_EQ(invalidatedCount, 1u);
    TEST_ASSERT_EQ(invalidatedTexture, residentTexture);
    TEST_ASSERT_TRUE(manager.GetTexture(texture->GetId()) == nullptr);
    TEST_ASSERT_FALSE(manager.IsResident(texture->GetId()));
    TEST_ASSERT_EQ(manager.GetResourceState(texture->GetId()), GPUResourceState::Unloaded);

    manager.Shutdown();
    return true;
}

bool Test_UnsupportedTextureUploadMarksResourceFailed()
{
    FakeDevice device;
    device.supportStagedCopy = false;

    GPUResourceManager manager;
    manager.Initialize(&device);

    auto texture = CreateTextureResource(106);
    manager.UploadImmediate(texture.get());

    TEST_ASSERT_EQ(manager.GetResourceState(texture->GetId()), GPUResourceState::Failed);
    TEST_ASSERT_FALSE(manager.IsResident(texture->GetId()));
    TEST_ASSERT_FALSE(manager.IsGPUReady(texture->GetId()));
    TEST_ASSERT_TRUE(manager.GetTexture(texture->GetId()) == nullptr);
    TEST_ASSERT_EQ(device.createdTextureCount, 0u);
    TEST_ASSERT_EQ(device.submittedCommandContextCount, 0u);

    const auto stats = manager.GetStats();
    TEST_ASSERT_EQ(stats.failedUploadCount, 1ull);
    TEST_ASSERT_EQ(stats.residentTextureCount, 0ull);

    manager.Shutdown();
    return true;
}

int main()
{
    Log::Initialize();
    RVX_CORE_INFO("GPUResourceManager Validation Tests");

    TestSuite suite;
    suite.AddTest("GPUUploadServiceUploadsBufferData", Test_GPUUploadServiceUploadsBufferData);
    suite.AddTest("GPUUploadServiceRejectsInvalidBufferUpload", Test_GPUUploadServiceRejectsInvalidBufferUpload);
    suite.AddTest("GPUUploadServiceReportsInvalidBufferData", Test_GPUUploadServiceReportsInvalidBufferData);
    suite.AddTest("GPUUploadServiceReportsBufferCreationFailure", Test_GPUUploadServiceReportsBufferCreationFailure);
    suite.AddTest("GPUUploadServiceUsesStagedCopyForBufferWhenAvailable", Test_GPUUploadServiceUsesStagedCopyForBufferWhenAvailable);
    suite.AddTest("GPUUploadServiceCompletesPendingBufferUploadAfterFence", Test_GPUUploadServiceCompletesPendingBufferUploadAfterFence);
    suite.AddTest("GPUUploadServiceFallsBackWhenStagingMapFails", Test_GPUUploadServiceFallsBackWhenStagingMapFails);
    suite.AddTest("GPUUploadServiceRejectsTextureWhenStagedCopyUnavailable", Test_GPUUploadServiceRejectsTextureWhenStagedCopyUnavailable);
    suite.AddTest("GPUUploadServiceUsesStagedCopyForTextureWhenAvailable", Test_GPUUploadServiceUsesStagedCopyForTextureWhenAvailable);
    suite.AddTest("GPUUploadServiceRejectsInvalidTextureDimensions", Test_GPUUploadServiceRejectsInvalidTextureDimensions);
    suite.AddTest("GPUUploadServiceRejectsUnsupportedTextureFormat", Test_GPUUploadServiceRejectsUnsupportedTextureFormat);
    suite.AddTest("DuplicateQueuedUploadIsIgnored", Test_DuplicateQueuedUploadIsIgnored);
    suite.AddTest("UnmanagedResourceIsNotQueuedForAsyncUpload", Test_UnmanagedResourceIsNotQueuedForAsyncUpload);
    suite.AddTest("QueuedUploadRetainsRefCountedResource", Test_QueuedUploadRetainsRefCountedResource);
    suite.AddTest("ProcessPendingUploadsWithZeroBudgetDoesNotStartNewUpload", Test_ProcessPendingUploadsWithZeroBudgetDoesNotStartNewUpload);
    suite.AddTest("MeshWithoutIndexDataFailsUpload", Test_MeshWithoutIndexDataFailsUpload);
    suite.AddTest("ValidMeshUploadBecomesGPUReady", Test_ValidMeshUploadBecomesGPUReady);
    suite.AddTest("StagedMeshUploadImmediateWaitsForFenceCompletion", Test_StagedMeshUploadImmediateWaitsForFenceCompletion);
    suite.AddTest("TransitionTextureTransitionsResidentTextureOnce", Test_TransitionTextureTransitionsResidentTextureOnce);
    suite.AddTest("TextureEvictionNotifiesViewCachesBeforeRelease", Test_TextureEvictionNotifiesViewCachesBeforeRelease);
    suite.AddTest("UnsupportedTextureUploadMarksResourceFailed", Test_UnsupportedTextureUploadMarksResourceFailed);

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
