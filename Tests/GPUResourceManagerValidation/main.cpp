#include "Core/Core.h"
#include "Render/GPUResourceManager.h"
#include "Render/GPUUploadService.h"
#include "Render/RayTracing/RayTracingScene.h"
#include "Render/RayTracing/RayTracingSceneManager.h"
#include "Render/Renderer/RenderScene.h"
#include "Resource/Loader/ModelLoader.h"
#include "Resource/Types/MaterialResource.h"
#include "Resource/Types/MeshResource.h"
#include "Resource/Types/TextureResource.h"
#include "RHI/RHICommandContext.h"
#include "RHI/RHIDevice.h"
#include "Scene/Mesh.h"
#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
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

    class FakeAccelerationStructure final : public RHIAccelerationStructure
    {
    public:
        explicit FakeAccelerationStructure(const RHIAccelerationStructureDesc& desc)
            : m_desc(desc)
            , m_address(s_nextAddress)
        {
            s_nextAddress += 0x1000;
        }

        FakeAccelerationStructure(const RHIAccelerationStructureDesc& desc, uint64 address)
            : m_desc(desc)
            , m_address(address)
        {
        }

        FakeAccelerationStructure()
            : FakeAccelerationStructure({RHIAccelerationStructureType::BottomLevel, 4096, "FakeBLAS"})
        {
        }

        RHIAccelerationStructureType GetType() const override { return m_desc.type; }
        uint64 GetSize() const override { return m_desc.size; }
        uint64 GetGPUVirtualAddress() const override { return m_address; }

    private:
        RHIAccelerationStructureDesc m_desc;
        uint64 m_address = 0;
        inline static uint64 s_nextAddress = 0x1000;
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
        const std::vector<uint8>& GetStorage() const
        {
            auto* fakeBuffer = static_cast<FakeBuffer*>(m_buffer.Get());
            return fakeBuffer->GetStorage();
        }

    private:
        RHIStagingBufferDesc m_desc;
        RHIBufferRef m_buffer;
        bool m_mapSucceeds = true;
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
        uint32 beginCount = 0;
        uint32 endCount = 0;
        uint32 bufferBarrierCount = 0;
        uint32 textureBarrierCount = 0;
        uint32 copyBufferCount = 0;
        uint32 copyBufferToTextureCount = 0;
        uint32 buildBottomLevelASCount = 0;
        uint32 buildTopLevelASCount = 0;
        RHIBufferBarrier lastBufferBarrier;
        RHITextureBarrier lastTextureBarrier;
        RHIBuffer* lastCopySrc = nullptr;
        RHIBuffer* lastCopyDst = nullptr;
        RHITexture* lastTextureCopyDst = nullptr;
        RHIAccelerationStructure* lastBottomLevelAS = nullptr;
        RHIAccelerationStructure* lastTopLevelAS = nullptr;
        RHIBuffer* lastBottomLevelScratch = nullptr;
        RHIBuffer* lastTopLevelScratch = nullptr;
        RHITopLevelASDesc lastTopLevelDesc;
        uint64 lastCopySize = 0;
        RHIBufferTextureCopyDesc lastBufferTextureCopyDesc;
        std::vector<RHIBufferTextureCopyDesc> bufferTextureCopyDescs;

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

        void BuildBottomLevelAccelerationStructure(
            RHIAccelerationStructure* dst,
            const RHIBottomLevelASDesc&,
            RHIBuffer* scratchBuffer,
            uint64 = 0,
            RHIAccelerationStructure* = nullptr) override
        {
            ++buildBottomLevelASCount;
            lastBottomLevelAS = dst;
            lastBottomLevelScratch = scratchBuffer;
        }

        void BuildTopLevelAccelerationStructure(
            RHIAccelerationStructure* dst,
            const RHITopLevelASDesc& desc,
            RHIBuffer* scratchBuffer,
            uint64 = 0,
            RHIAccelerationStructure* = nullptr) override
        {
            ++buildTopLevelASCount;
            lastTopLevelAS = dst;
            lastTopLevelDesc = desc;
            lastTopLevelScratch = scratchBuffer;
        }

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
            bufferTextureCopyDescs.push_back(desc);
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
            : m_completedValue(initialValue), m_nextValue(initialValue + 1U)
        {
        }

        uint64 GetCompletedValue() const override { return m_completedValue; }
        void Signal(uint64 value) override
        {
            m_completedValue = value;
            if (value != UINT64_MAX)
            {
                m_nextValue = std::max(m_nextValue, value + 1U);
            }
        }
        void SignalOnQueue(uint64 value, RHICommandQueueType) override { Signal(value); }
        void Wait(uint64 value, uint64 = UINT64_MAX) override { Signal(value); }
        uint64 AllocateSignalValue() { return m_nextValue++; }

    private:
        uint64 m_completedValue = 0;
        uint64 m_nextValue = 1;
    };

    class FakeDevice final : public IRHIDevice
    {
    public:
        FakeDevice()
        {
            capabilities.backendType = RHIBackendType::DX12;
            capabilities.adapterName = "GPUResourceManagerValidation";
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
            createdBufferDescs.push_back(desc);
            if (failBufferCreation)
                return nullptr;

            return RHIBufferRef(new FakeBuffer(desc));
        }

        RHITextureRef CreateTexture(const RHITextureDesc& desc) override
        {
            ++createdTextureCount;
            lastCreatedTextureDesc = desc;
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
        RHIAccelerationStructureBuildSizes GetBottomLevelASBuildSizes(const RHIBottomLevelASDesc& desc) override
        {
            ++bottomLevelSizeQueryCount;
            if (!capabilities.supportsRaytracing || !ValidateRHIBottomLevelASDesc(desc))
                return {};

            if (bottomLevelBuildSizeOverrideIndex < bottomLevelBuildSizeOverrides.size())
            {
                return bottomLevelBuildSizeOverrides[bottomLevelBuildSizeOverrideIndex++];
            }

            RHIAccelerationStructureBuildSizes sizes;
            sizes.accelerationStructureSize = 4096 * desc.geometries.size();
            sizes.buildScratchSize = 1024 * desc.geometries.size();
            sizes.updateScratchSize = 512 * desc.geometries.size();
            return sizes;
        }
        RHIAccelerationStructureBuildSizes GetTopLevelASBuildSizes(const RHITopLevelASDesc& desc) override
        {
            ++topLevelSizeQueryCount;
            if (!capabilities.supportsRaytracing || !ValidateRHITopLevelASDesc(desc))
                return {};
            if (overrideTopLevelBuildSizes)
                return topLevelBuildSizesOverride;

            RHIAccelerationStructureBuildSizes sizes;
            sizes.accelerationStructureSize = 4096 + 512 * desc.GetInstanceCount();
            sizes.buildScratchSize = 2048;
            sizes.updateScratchSize = 1024;
            return sizes;
        }
        RHIAccelerationStructureRef CreateAccelerationStructure(const RHIAccelerationStructureDesc& desc) override
        {
            ++createdAccelerationStructureCount;
            createdAccelerationStructureDescs.push_back(desc);
            if (failAccelerationStructureCreation || !capabilities.supportsRaytracing || desc.size == 0)
                return nullptr;

            return RHIAccelerationStructureRef(new FakeAccelerationStructure(desc));
        }
        RHIDescriptorSetRef CreateDescriptorSet(const RHIDescriptorSetDesc&) override { return nullptr; }
        RHIQueryPoolRef CreateQueryPool(const RHIQueryPoolDesc&) override { return nullptr; }
        RHICommandContextRef CreateCommandContext(RHICommandQueueType type) override
        {
            ++createdCommandContextCount;
            lastCommandQueueType = type;
            if (!supportStagedCopy)
                return nullptr;

            retainedCommandContext = RHICommandContextRef(new FakeCommandContext(type));
            lastCommandContext = static_cast<FakeCommandContext*>(retainedCommandContext.Get());
            return retainedCommandContext;
        }

        uint64 SubmitCommandContext(RHICommandContext*, RHIFence* signalFence) override
        {
            ++submittedCommandContextCount;
            lastSubmittedFence = signalFence;
            uint64 submittedValue = signalFence
                                        ? static_cast<FakeFence*>(signalFence)
                                              ->AllocateSignalValue()
                                        : 0;
            if (completeSubmittedFenceImmediately && signalFence)
            {
                signalFence->Signal(submittedValue);
            }
            return submittedValue;
        }
        uint64 SubmitCommandContexts(std::span<RHICommandContext* const>, RHIFence* signalFence) override
        {
            return signalFence ? 1 : 0;
        }
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

            auto staging = RHIStagingBufferRef(new FakeStagingBuffer(desc, stagingMapSucceeds));
            lastStagingBuffer = static_cast<FakeStagingBuffer*>(staging.Get());
            retainedStagingBuffers.push_back(staging);
            return staging;
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
        uint32 bottomLevelSizeQueryCount = 0;
        uint32 topLevelSizeQueryCount = 0;
        uint32 createdAccelerationStructureCount = 0;
        uint32 waitIdleCount = 0;
        RHICommandQueueType lastCommandQueueType = RHICommandQueueType::Copy;
        bool failBufferCreation = false;
        bool failTextureCreation = false;
        bool failAccelerationStructureCreation = false;
        bool overrideTopLevelBuildSizes = false;
        bool supportStagedCopy = false;
        bool stagingMapSucceeds = true;
        bool completeSubmittedFenceImmediately = false;
        FakeCommandContext* lastCommandContext = nullptr;
        FakeStagingBuffer* lastStagingBuffer = nullptr;
        FakeFence* lastFence = nullptr;
        std::vector<FakeFence*> fences;
        RHIFence* lastSubmittedFence = nullptr;
        RHICommandContextRef retainedCommandContext;
        std::vector<RHIAccelerationStructureBuildSizes> bottomLevelBuildSizeOverrides;
        size_t bottomLevelBuildSizeOverrideIndex = 0;
        std::vector<RHIStagingBufferRef> retainedStagingBuffers;
        std::vector<RHIFenceRef> retainedFences;
        std::vector<RHIBufferDesc> createdBufferDescs;
        std::vector<RHIAccelerationStructureDesc> createdAccelerationStructureDescs;
        RHIAccelerationStructureBuildSizes topLevelBuildSizesOverride;
        mutable RHICapabilities capabilities;
        RHIBackendType backendType = RHIBackendType::DX12;
        RHITextureDesc lastCreatedTextureDesc;
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

    std::unique_ptr<Resource::TextureResource> CreateTextureResource(
        Resource::ResourceId id,
        Resource::TextureFormat format,
        std::vector<uint8> pixels,
        uint32 width = 1,
        uint32 height = 1)
    {
        auto resource = std::make_unique<Resource::TextureResource>();
        resource->SetId(id);
        resource->SetName("TestTexture");

        Resource::TextureMetadata metadata;
        metadata.width = width;
        metadata.height = height;
        metadata.format = format;
        metadata.isSRGB = false;

        resource->SetData(std::move(pixels), metadata);
        return resource;
    }

    std::unique_ptr<Resource::TextureResource> CreateTextureResourceWithMetadata(
        Resource::ResourceId id,
        Resource::TextureMetadata metadata,
        std::vector<uint8> pixels)
    {
        auto resource = std::make_unique<Resource::TextureResource>();
        resource->SetId(id);
        resource->SetName("TestTexture");
        metadata.isSRGB = false;

        resource->SetData(std::move(pixels), metadata);
        return resource;
    }

    std::unique_ptr<Resource::MaterialResource> CreateMaterialResource(
        Resource::ResourceId id,
        Material::AlphaMode alphaMode)
    {
        auto material = std::make_shared<Material>("TestMaterial");
        material->SetMaterialId(static_cast<uint32_t>(id));
        material->SetAlphaMode(alphaMode);

        auto resource = std::make_unique<Resource::MaterialResource>();
        resource->SetId(id);
        resource->SetName("TestMaterial");
        resource->SetMaterialData(std::move(material));
        return resource;
    }

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

TEST(GPUResourceManagerValidation, DuplicateQueuedUploadIsIgnored)
{
    FakeDevice device;
    GPUResourceManager manager;
    manager.Initialize(&device);

    auto mesh = Resource::ResourceHandle<Resource::MeshResource>(
        CreateMeshResource(101, MeshFactory::CreateTriangle()).release());

    manager.RequestUpload(mesh.Get());
    manager.RequestUpload(mesh.Get(), UploadPriority::Immediate);

    const auto stats = manager.GetStats();
    EXPECT_EQ(stats.pendingUploadCount, 1u);
    EXPECT_EQ(stats.queuedUploadCount, 1u);
    EXPECT_EQ(manager.GetResourceState(mesh->GetId()), GPUResourceState::UploadQueued);

    manager.Shutdown();
}

TEST(GPUResourceManagerValidation, UnmanagedResourceIsNotQueuedForAsyncUpload)
{
    FakeDevice device;
    GPUResourceManager manager;
    manager.Initialize(&device);

    auto mesh = CreateMeshResource(109, MeshFactory::CreateTriangle());
    manager.RequestUpload(mesh.get());

    const auto stats = manager.GetStats();
    EXPECT_EQ(stats.pendingUploadCount, 0u);
    EXPECT_EQ(manager.GetResourceState(mesh->GetId()), GPUResourceState::Unloaded);

    manager.Shutdown();
}

TEST(GPUResourceManagerValidation, ResidentTextureIsNotQueuedForAsyncUpload)
{
    FakeDevice device;
    device.supportStagedCopy = true;
    device.completeSubmittedFenceImmediately = true;

    GPUResourceManager manager;
    manager.Initialize(&device);

    auto textureHandle = Resource::ResourceHandle<Resource::TextureResource>(
        CreateTextureResource(113).release());
    manager.UploadImmediate(textureHandle.Get());
    ASSERT_TRUE(manager.IsGPUReady(textureHandle.GetId()));

    manager.RequestUpload(textureHandle.Get(), UploadPriority::Immediate);

    const auto stats = manager.GetStats();
    EXPECT_EQ(stats.pendingUploadCount, 0u);
    EXPECT_EQ(stats.queuedUploadCount, 0u);
    EXPECT_EQ(manager.GetResourceState(textureHandle.GetId()), GPUResourceState::GPUReady);

    manager.Shutdown();
}

TEST(GPUResourceManagerValidation, UploadImmediateTextureRemovesQueuedStaleTextureForSameId)
{
    FakeDevice device;
    device.supportStagedCopy = true;
    device.completeSubmittedFenceImmediately = true;

    GPUResourceManager manager;
    manager.Initialize(&device);

    auto queuedTexture = Resource::ResourceHandle<Resource::TextureResource>(
        CreateTextureResource(114).release());
    manager.RequestUpload(queuedTexture.Get(), UploadPriority::Immediate);
    ASSERT_EQ(manager.GetStats().pendingUploadCount, 1u);
    ASSERT_EQ(manager.GetResourceState(queuedTexture.GetId()), GPUResourceState::UploadQueued);

    auto invalidRefresh = CreateTextureResource(
        114,
        Resource::TextureFormat::RGBA8,
        {1, 2, 3},
        1,
        1);
    manager.UploadImmediate(invalidRefresh.get());

    EXPECT_EQ(manager.GetStats().pendingUploadCount, 0u);
    EXPECT_EQ(manager.GetStats().queuedUploadCount, 0u);
    EXPECT_EQ(manager.GetResourceState(queuedTexture.GetId()), GPUResourceState::Failed);
    EXPECT_EQ(manager.GetTexture(queuedTexture.GetId()), nullptr);

    manager.ProcessPendingUploads();

    EXPECT_EQ(device.createdTextureCount, 0u);
    EXPECT_EQ(manager.GetResourceState(queuedTexture.GetId()), GPUResourceState::Failed);
    EXPECT_EQ(manager.GetTexture(queuedTexture.GetId()), nullptr);

    manager.Shutdown();
}

TEST(GPUResourceManagerValidation, QueuedUploadRetainsRefCountedResource)
{
    FakeDevice device;
    GPUResourceManager manager;
    manager.Initialize(&device);

    auto meshHandle = Resource::ResourceHandle<Resource::MeshResource>(
        CreateMeshResource(107, MeshFactory::CreateTriangle()).release());
    Resource::MeshResource* mesh = meshHandle.Get();

    EXPECT_EQ(mesh->GetRefCount(), 1u);
    manager.RequestUpload(mesh);
    EXPECT_EQ(mesh->GetRefCount(), 2u);

    const Resource::ResourceId meshId = mesh->GetId();
    meshHandle.Reset();

    manager.ProcessPendingUploads();

    EXPECT_EQ(manager.GetResourceState(meshId), GPUResourceState::GPUReady);
    EXPECT_TRUE(manager.IsResident(meshId));

    manager.Shutdown();
}

TEST(GPUResourceManagerValidation, ProcessPendingUploadsWithZeroBudgetDoesNotStartNewUpload)
{
    FakeDevice device;
    GPUResourceManager manager;
    manager.Initialize(&device);

    auto mesh = Resource::ResourceHandle<Resource::MeshResource>(
        CreateMeshResource(108, MeshFactory::CreateTriangle()).release());
    manager.RequestUpload(mesh.Get());

    manager.ProcessPendingUploads(0.0f);

    EXPECT_EQ(device.createdBufferCount, 0u);
    EXPECT_EQ(manager.GetResourceState(mesh->GetId()), GPUResourceState::UploadQueued);
    {
        const auto stats = manager.GetStats();
        EXPECT_EQ(stats.pendingUploadCount, 1ull);
        EXPECT_EQ(stats.queuedUploadCount, 1ull);
    }

    manager.ProcessPendingUploads();

    EXPECT_TRUE(manager.IsGPUReady(mesh->GetId()));

    manager.Shutdown();
}

TEST(GPUResourceManagerValidation, GPUUploadServiceUploadsBufferData)
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

    ASSERT_NE(nullptr, buffer.Get());
    EXPECT_EQ(buffer->GetSize(), static_cast<uint64>(sizeof(source)));
    EXPECT_EQ(buffer->GetUsage(), RHIBufferUsage::Vertex);
    EXPECT_EQ(buffer->GetMemoryType(), RHIMemoryType::Upload);
    EXPECT_EQ(buffer->GetStride(), static_cast<uint32>(sizeof(uint32)));

    auto* fakeBuffer = dynamic_cast<FakeBuffer*>(buffer.Get());
    ASSERT_NE(nullptr, fakeBuffer);
    EXPECT_TRUE(std::memcmp(fakeBuffer->GetStorage().data(), source, sizeof(source)) == 0);

    const auto stats = uploadService.GetStats();
    EXPECT_EQ(stats.bufferUploadCount, 1u);
    EXPECT_EQ(stats.failedUploadCount, 0u);
    EXPECT_EQ(stats.uploadedBytes, static_cast<uint64>(sizeof(source)));

    uploadService.Shutdown();
}

TEST(GPUResourceManagerValidation, GPUUploadServiceRejectsInvalidBufferUpload)
{
    FakeDevice device;
    GPUUploadService uploadService;
    uploadService.Initialize(&device);

    GPUUploadBufferDesc desc;
    desc.size = 16;
    desc.usage = RHIBufferUsage::Vertex;

    auto buffer = uploadService.UploadBufferData(desc, nullptr, desc.size);

    EXPECT_TRUE(!buffer);

    const auto stats = uploadService.GetStats();
    EXPECT_EQ(stats.bufferUploadCount, 0u);
    EXPECT_EQ(stats.failedUploadCount, 1u);

    uploadService.Shutdown();
}

TEST(GPUResourceManagerValidation, GPUUploadServiceReportsInvalidBufferData)
{
    FakeDevice device;
    GPUUploadService uploadService;
    uploadService.Initialize(&device);

    GPUUploadBufferDesc desc;
    desc.size = 16;
    desc.usage = RHIBufferUsage::Vertex;

    auto result = uploadService.UploadBufferDataWithResult(desc, nullptr, desc.size);

    EXPECT_FALSE(result.succeeded);
    EXPECT_TRUE(!result.resource);
    EXPECT_EQ(result.mode, GPUUploadMode::None);
    EXPECT_EQ(result.failureReason, GPUUploadFailureReason::InvalidData);
    EXPECT_EQ(result.bytesUploaded, 0ull);

    const auto stats = uploadService.GetStats();
    EXPECT_EQ(stats.failedUploadCount, 1u);
    EXPECT_EQ(stats.immediateUploadCount, 0u);
    EXPECT_EQ(stats.uploadedBytes, 0ull);

    uploadService.Shutdown();
}

TEST(GPUResourceManagerValidation, GPUUploadServiceReportsBufferCreationFailure)
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

    EXPECT_FALSE(result.succeeded);
    EXPECT_TRUE(!result.resource);
    EXPECT_EQ(result.mode, GPUUploadMode::None);
    EXPECT_EQ(result.failureReason, GPUUploadFailureReason::CreateResourceFailed);
    EXPECT_EQ(result.bytesUploaded, 0ull);

    const auto stats = uploadService.GetStats();
    EXPECT_EQ(stats.failedUploadCount, 1u);
    EXPECT_EQ(stats.bufferUploadCount, 0u);

    uploadService.Shutdown();
}

TEST(GPUResourceManagerValidation, GPUUploadServiceUsesStagedCopyForBufferWhenAvailable)
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

    EXPECT_TRUE(result.succeeded);
    ASSERT_NE(nullptr, result.resource.Get());
    EXPECT_EQ(result.mode, GPUUploadMode::StagedCopy);
    EXPECT_EQ(result.failureReason, GPUUploadFailureReason::None);
    EXPECT_EQ(result.bytesUploaded, static_cast<uint64>(sizeof(source)));
    EXPECT_EQ(result.resource->GetMemoryType(), RHIMemoryType::Default);
    EXPECT_EQ(result.resource->GetUsage() & RHIBufferUsage::CopyDst, RHIBufferUsage::CopyDst);

    ASSERT_NE(nullptr, device.lastCommandContext);
    EXPECT_EQ(device.lastCommandQueueType, RHICommandQueueType::Copy);
    EXPECT_EQ(device.lastCommandContext->beginCount, 1u);
    EXPECT_EQ(device.lastCommandContext->endCount, 0u);
    EXPECT_EQ(device.submittedCommandContextCount, 0u);
    EXPECT_EQ(device.createdFenceCount, 3u);

    uploadService.FlushBatchUploads();

    EXPECT_EQ(device.lastCommandContext->endCount, 1u);
    EXPECT_EQ(device.lastCommandContext->copyBufferCount, 1u);
    EXPECT_EQ(device.lastCommandContext->lastCopyDst, result.resource.Get());
    EXPECT_EQ(device.lastCommandContext->lastCopySize, static_cast<uint64>(sizeof(source)));
    EXPECT_EQ(device.lastCommandContext->bufferBarrierCount, 1u);
    EXPECT_EQ(device.lastCommandContext->lastBufferBarrier.buffer, result.resource.Get());
    EXPECT_EQ(device.lastCommandContext->lastBufferBarrier.stateBefore, RHIResourceState::CopyDest);
    EXPECT_EQ(device.lastCommandContext->lastBufferBarrier.stateAfter, RHIResourceState::Common);
    EXPECT_EQ(device.submittedCommandContextCount, 1u);
    EXPECT_EQ(result.isPending, true);
    EXPECT_TRUE(result.uploadId != 0);
    EXPECT_EQ(device.createdFenceCount, 3u);
    EXPECT_EQ(device.waitIdleCount, 0u);

    const auto stats = uploadService.GetStats();
    EXPECT_EQ(stats.bufferUploadCount, 1u);
    EXPECT_EQ(stats.stagedUploadCount, 1u);
    EXPECT_EQ(stats.immediateUploadCount, 0u);
    EXPECT_EQ(stats.pendingUploadCount, 1u);
    EXPECT_EQ(stats.stagingBytesInFlight, static_cast<uint64>(sizeof(source)));
    EXPECT_EQ(stats.uploadedBytes, static_cast<uint64>(sizeof(source)));

    uploadService.Shutdown();
}

TEST(GPUResourceManagerValidation, GPUUploadServiceCompletesPendingBufferUploadAfterFence)
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

    EXPECT_TRUE(result.succeeded);
    EXPECT_TRUE(result.isPending);
    EXPECT_TRUE(uploadService.IsUploadPending(result.uploadId));
    EXPECT_FALSE(uploadService.IsUploadComplete(result.uploadId));

    uploadService.FlushBatchUploads();

    EXPECT_EQ(uploadService.ProcessCompletedUploads(), 0u);
    EXPECT_TRUE(uploadService.IsUploadPending(result.uploadId));

    ASSERT_NE(nullptr, device.lastFence);
    device.lastFence->Signal(1);

    EXPECT_EQ(uploadService.ProcessCompletedUploads(), 1u);
    EXPECT_FALSE(uploadService.IsUploadPending(result.uploadId));
    EXPECT_TRUE(uploadService.IsUploadComplete(result.uploadId));

    const auto stats = uploadService.GetStats();
    EXPECT_EQ(stats.pendingUploadCount, 0u);
    EXPECT_EQ(stats.completedUploadCount, 1u);
    EXPECT_EQ(stats.stagingBytesInFlight, 0ull);

    uploadService.Shutdown();
}

TEST(GPUResourceManagerValidation, GPUUploadServiceFallsBackWhenStagingMapFails)
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

    EXPECT_TRUE(result.succeeded);
    EXPECT_EQ(result.mode, GPUUploadMode::ImmediateMapped);
    EXPECT_EQ(result.failureReason, GPUUploadFailureReason::None);
    EXPECT_FALSE(result.isPending);
    EXPECT_EQ(result.uploadId, 0ull);
    EXPECT_EQ(result.resource->GetMemoryType(), RHIMemoryType::Upload);

    const auto stats = uploadService.GetStats();
    EXPECT_EQ(stats.failedUploadCount, 0u);
    EXPECT_EQ(stats.bufferUploadCount, 1u);
    EXPECT_EQ(stats.immediateUploadCount, 1u);
    EXPECT_EQ(stats.stagedUploadCount, 0u);

    uploadService.Shutdown();
}

TEST(GPUResourceManagerValidation, GPUUploadServiceRejectsTextureWhenStagedCopyUnavailable)
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

    EXPECT_FALSE(result.succeeded);
    EXPECT_TRUE(!result.resource);
    EXPECT_EQ(result.mode, GPUUploadMode::None);
    EXPECT_EQ(result.failureReason, GPUUploadFailureReason::Unsupported);
    EXPECT_EQ(result.bytesUploaded, 0ull);

    const auto stats = uploadService.GetStats();
    EXPECT_EQ(stats.textureUploadCount, 0u);
    EXPECT_EQ(stats.failedUploadCount, 1u);
    EXPECT_EQ(stats.immediateUploadCount, 0u);
    EXPECT_EQ(stats.uploadedBytes, 0ull);

    uploadService.Shutdown();
}

TEST(GPUResourceManagerValidation, GPUUploadServiceUsesStagedCopyForTextureWhenAvailable)
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

    EXPECT_TRUE(result.succeeded);
    ASSERT_NE(nullptr, result.resource.Get());
    EXPECT_EQ(result.mode, GPUUploadMode::StagedCopy);
    EXPECT_EQ(result.failureReason, GPUUploadFailureReason::None);
    EXPECT_EQ(result.bytesUploaded, static_cast<uint64>(sizeof(pixels)));

    ASSERT_NE(nullptr, device.lastCommandContext);
    EXPECT_EQ(device.lastCommandQueueType, RHICommandQueueType::Copy);
    EXPECT_EQ(device.lastCommandContext->beginCount, 1u);
    EXPECT_EQ(device.lastCommandContext->endCount, 0u);
    EXPECT_EQ(device.submittedCommandContextCount, 0u);
    EXPECT_EQ(device.createdFenceCount, 3u);

    uploadService.FlushBatchUploads();

    EXPECT_EQ(device.lastCommandContext->endCount, 1u);
    EXPECT_EQ(device.lastCommandContext->copyBufferToTextureCount, 1u);
    EXPECT_EQ(device.lastCommandContext->lastTextureCopyDst, result.resource.Get());
    EXPECT_EQ(device.lastCommandContext->lastBufferTextureCopyDesc.bufferRowPitch, 256u);
    EXPECT_EQ(device.lastCommandContext->lastBufferTextureCopyDesc.textureRegion.width, 2u);
    EXPECT_EQ(device.lastCommandContext->lastBufferTextureCopyDesc.textureRegion.height, 2u);
    EXPECT_EQ(device.lastCommandContext->textureBarrierCount, 1u);
    EXPECT_EQ(device.lastCommandContext->lastTextureBarrier.texture, result.resource.Get());
    EXPECT_EQ(device.lastCommandContext->lastTextureBarrier.stateBefore, RHIResourceState::CopyDest);
    EXPECT_EQ(device.lastCommandContext->lastTextureBarrier.stateAfter, RHIResourceState::Common);
    EXPECT_EQ(device.submittedCommandContextCount, 1u);
    EXPECT_EQ(result.isPending, true);
    EXPECT_TRUE(result.uploadId != 0);
    EXPECT_EQ(device.createdFenceCount, 3u);
    EXPECT_EQ(device.waitIdleCount, 0u);

    const auto stats = uploadService.GetStats();
    EXPECT_EQ(stats.textureUploadCount, 1u);
    EXPECT_EQ(stats.stagedUploadCount, 1u);
    EXPECT_EQ(stats.immediateUploadCount, 0u);
    EXPECT_EQ(stats.pendingUploadCount, 1u);
    EXPECT_EQ(stats.stagingBytesInFlight, 512ull);
    EXPECT_EQ(stats.uploadedBytes, static_cast<uint64>(sizeof(pixels)));

    uploadService.Shutdown();
}

TEST(GPUResourceManagerValidation, GPUUploadServiceUsesBlockRowsForCompressedTextureUpload)
{
    FakeDevice device;
    device.supportStagedCopy = true;

    GPUUploadService uploadService;
    uploadService.Initialize(&device);

    const std::vector<uint8> blocks(24, 0x7F);
    GPUUploadTextureDesc desc;
    desc.textureDesc = RHITextureDesc::Texture2D(4, 4, RHIFormat::BC1_UNORM);
    desc.textureDesc.mipLevels = 3;
    desc.dataSize = blocks.size();

    auto result = uploadService.UploadTextureDataWithResult(desc, blocks.data());

    EXPECT_TRUE(result.succeeded);
    ASSERT_NE(nullptr, result.resource.Get());
    EXPECT_EQ(result.mode, GPUUploadMode::StagedCopy);
    EXPECT_EQ(result.bytesUploaded, 24ull);
    ASSERT_NE(nullptr, device.lastCommandContext);
    ASSERT_EQ(device.lastCommandContext->bufferTextureCopyDescs.size(), 3u);

    const auto& mip0 = device.lastCommandContext->bufferTextureCopyDescs[0];
    const auto& mip1 = device.lastCommandContext->bufferTextureCopyDescs[1];
    const auto& mip2 = device.lastCommandContext->bufferTextureCopyDescs[2];
    EXPECT_EQ(mip0.bufferRowPitch, 256u);
    EXPECT_EQ(mip0.bufferImageHeight, 1u);
    EXPECT_EQ(mip0.textureRegion.width, 4u);
    EXPECT_EQ(mip0.textureRegion.height, 4u);
    EXPECT_EQ(mip1.bufferOffset, 512ull);
    EXPECT_EQ(mip1.textureRegion.width, 2u);
    EXPECT_EQ(mip1.textureRegion.height, 2u);
    EXPECT_EQ(mip2.bufferOffset, 1024ull);
    EXPECT_EQ(mip2.textureRegion.width, 1u);
    EXPECT_EQ(mip2.textureRegion.height, 1u);

    const auto& storage = device.lastStagingBuffer->GetStorage();
    EXPECT_EQ(storage[static_cast<size_t>(mip0.bufferOffset)], 0x7Fu);
    EXPECT_EQ(storage[static_cast<size_t>(mip1.bufferOffset)], 0x7Fu);
    EXPECT_EQ(storage[static_cast<size_t>(mip2.bufferOffset)], 0x7Fu);
    EXPECT_EQ(uploadService.GetStats().uploadedBytes, 24ull);
    EXPECT_EQ(uploadService.GetStats().stagingBytesInFlight, 1280ull);

    uploadService.Shutdown();
}

TEST(GPUResourceManagerValidation, GPUUploadServiceUsesTightRowsForOpenGLCompressedTextureUpload)
{
    FakeDevice device;
    device.supportStagedCopy = true;
    device.backendType = RHIBackendType::OpenGL;

    GPUUploadService uploadService;
    uploadService.Initialize(&device);

    const std::vector<uint8> block(8, 0xAA);
    GPUUploadTextureDesc desc;
    desc.textureDesc = RHITextureDesc::Texture2D(4, 4, RHIFormat::BC1_UNORM);
    desc.dataSize = block.size();

    auto result = uploadService.UploadTextureDataWithResult(desc, block.data());

    EXPECT_TRUE(result.succeeded);
    ASSERT_NE(nullptr, result.resource.Get());
    EXPECT_EQ(result.failureReason, GPUUploadFailureReason::None);
    EXPECT_EQ(device.createdTextureCount, 1u);
    ASSERT_NE(nullptr, device.lastCommandContext);
    ASSERT_EQ(device.lastCommandContext->bufferTextureCopyDescs.size(), 1u);

    const auto& copyDesc = device.lastCommandContext->bufferTextureCopyDescs[0];
    EXPECT_EQ(copyDesc.bufferOffset, 0ull);
    EXPECT_EQ(copyDesc.bufferRowPitch, 8u);
    EXPECT_EQ(copyDesc.bufferImageHeight, 1u);
    EXPECT_EQ(copyDesc.textureRegion.width, 4u);
    EXPECT_EQ(copyDesc.textureRegion.height, 4u);
    EXPECT_EQ(uploadService.GetStats().failedUploadCount, 0u);
    EXPECT_EQ(uploadService.GetStats().uploadedBytes, 8ull);
    EXPECT_EQ(uploadService.GetStats().stagingBytesInFlight, 8ull);

    uploadService.Shutdown();
}

TEST(GPUResourceManagerValidation, GPUUploadServiceRejectsInvalidTextureDimensions)
{
    FakeDevice device;
    GPUUploadService uploadService;
    uploadService.Initialize(&device);

    const uint8 pixels[] = {255, 255, 255, 255};
    GPUUploadTextureDesc desc;
    desc.textureDesc = RHITextureDesc::Texture2D(0, 1, RHIFormat::RGBA8_UNORM);
    desc.dataSize = sizeof(pixels);

    auto result = uploadService.UploadTextureDataWithResult(desc, pixels);

    EXPECT_FALSE(result.succeeded);
    EXPECT_TRUE(!result.resource);
    EXPECT_EQ(result.mode, GPUUploadMode::None);
    EXPECT_EQ(result.failureReason, GPUUploadFailureReason::InvalidDescription);
    EXPECT_EQ(result.bytesUploaded, 0ull);

    const auto stats = uploadService.GetStats();
    EXPECT_EQ(stats.failedUploadCount, 1u);
    EXPECT_EQ(stats.textureUploadCount, 0u);

    uploadService.Shutdown();
}

TEST(GPUResourceManagerValidation, GPUUploadServiceRejectsUnsupportedTextureFormat)
{
    FakeDevice device;
    GPUUploadService uploadService;
    uploadService.Initialize(&device);

    const uint8 pixels[] = {0, 0, 0, 0};
    GPUUploadTextureDesc desc;
    desc.textureDesc = RHITextureDesc::Texture2D(1, 1, RHIFormat::Unknown);
    desc.dataSize = sizeof(pixels);

    auto result = uploadService.UploadTextureDataWithResult(desc, pixels);

    EXPECT_FALSE(result.succeeded);
    EXPECT_TRUE(!result.resource);
    EXPECT_EQ(result.mode, GPUUploadMode::None);
    EXPECT_EQ(result.failureReason, GPUUploadFailureReason::Unsupported);
    EXPECT_EQ(result.bytesUploaded, 0ull);

    const auto stats = uploadService.GetStats();
    EXPECT_EQ(stats.failedUploadCount, 1u);
    EXPECT_EQ(stats.textureUploadCount, 0u);

    uploadService.Shutdown();
}

TEST(GPUResourceManagerValidation, MeshWithoutIndexDataFailsUpload)
{
    FakeDevice device;
    GPUResourceManager manager;
    manager.Initialize(&device);

    auto mesh = CreateMeshResource(102, CreatePositionOnlyMesh());

    manager.UploadImmediate(mesh.get());

    EXPECT_EQ(manager.GetResourceState(mesh->GetId()), GPUResourceState::Failed);
    EXPECT_FALSE(manager.IsResident(mesh->GetId()));
    EXPECT_FALSE(manager.IsGPUReady(mesh->GetId()));
    EXPECT_FALSE(manager.GetMeshBuffers(mesh->GetId()).IsValid());

    manager.Shutdown();
}

TEST(GPUResourceManagerValidation, ValidMeshUploadBecomesGPUReady)
{
    FakeDevice device;
    GPUResourceManager manager;
    manager.Initialize(&device);

    auto mesh = CreateMeshResource(103, MeshFactory::CreateTriangle());

    manager.UploadImmediate(mesh.get());

    const auto buffers = manager.GetMeshBuffers(mesh->GetId());
    EXPECT_EQ(manager.GetResourceState(mesh->GetId()), GPUResourceState::GPUReady);
    EXPECT_TRUE(manager.IsResident(mesh->GetId()));
    EXPECT_TRUE(manager.IsGPUReady(mesh->GetId()));
    EXPECT_TRUE(buffers.IsValid());
    EXPECT_TRUE(device.createdBufferCount >= 2);

    manager.Shutdown();
}

TEST(GPUResourceManagerValidation, RayTracingSceneMetadataLayoutMatchesShaderABI)
{
    EXPECT_EQ(sizeof(RayTracingInstanceAlphaMetadata), 80u);
    EXPECT_EQ(offsetof(RayTracingInstanceAlphaMetadata, flags), 0u);
    EXPECT_EQ(offsetof(RayTracingInstanceAlphaMetadata, baseColorUVSet), 4u);
    EXPECT_EQ(offsetof(RayTracingInstanceAlphaMetadata, alphaCutoff), 8u);
    EXPECT_EQ(offsetof(RayTracingInstanceAlphaMetadata, baseColorAlpha), 12u);
    EXPECT_EQ(offsetof(RayTracingInstanceAlphaMetadata, baseColorTextureIdLow), 16u);
    EXPECT_EQ(offsetof(RayTracingInstanceAlphaMetadata, baseColorTextureIdHigh), 20u);
    EXPECT_EQ(offsetof(RayTracingInstanceAlphaMetadata, baseColorTextureTableIndex), 24u);
    EXPECT_EQ(offsetof(RayTracingInstanceAlphaMetadata, indexBufferTableIndex), 28u);
    EXPECT_EQ(offsetof(RayTracingInstanceAlphaMetadata, uvBufferTableIndex), 32u);
    EXPECT_EQ(offsetof(RayTracingInstanceAlphaMetadata, indexElementOffset), 36u);
    EXPECT_EQ(offsetof(RayTracingInstanceAlphaMetadata, baseVertex), 40u);
    EXPECT_EQ(offsetof(RayTracingInstanceAlphaMetadata, baseColorSamplerFlags), 44u);
    EXPECT_EQ(offsetof(RayTracingInstanceAlphaMetadata, normalBufferTableIndex), 48u);
    EXPECT_EQ(offsetof(RayTracingInstanceAlphaMetadata, tangentBufferTableIndex), 52u);
    EXPECT_EQ(offsetof(RayTracingInstanceAlphaMetadata, baseColorUVOffset), 56u);
    EXPECT_EQ(offsetof(RayTracingInstanceAlphaMetadata, baseColorUVScale), 64u);
    EXPECT_EQ(offsetof(RayTracingInstanceAlphaMetadata, baseColorUVRotation), 72u);
    EXPECT_EQ(offsetof(RayTracingInstanceAlphaMetadata, reserved2), 76u);

    EXPECT_EQ(sizeof(RayTracingInstanceTextureSamplingMetadata), 32u);
    EXPECT_EQ(offsetof(RayTracingInstanceTextureSamplingMetadata, uvOffset), 0u);
    EXPECT_EQ(offsetof(RayTracingInstanceTextureSamplingMetadata, uvScale), 8u);
    EXPECT_EQ(offsetof(RayTracingInstanceTextureSamplingMetadata, uvRotation), 16u);
    EXPECT_EQ(offsetof(RayTracingInstanceTextureSamplingMetadata, uvSet), 20u);
    EXPECT_EQ(offsetof(RayTracingInstanceTextureSamplingMetadata, samplerFlags), 24u);
    EXPECT_EQ(offsetof(RayTracingInstanceTextureSamplingMetadata, reserved), 28u);

    EXPECT_EQ(sizeof(RayTracingInstanceMaterialMetadata), 208u);
    EXPECT_EQ(offsetof(RayTracingInstanceMaterialMetadata, baseColorFactor), 0u);
    EXPECT_EQ(offsetof(RayTracingInstanceMaterialMetadata, emissiveFactor), 16u);
    EXPECT_EQ(offsetof(RayTracingInstanceMaterialMetadata, materialFactors), 32u);
    EXPECT_EQ(offsetof(RayTracingInstanceMaterialMetadata, flags), 48u);
    EXPECT_EQ(offsetof(RayTracingInstanceMaterialMetadata, materialIdLow), 52u);
    EXPECT_EQ(offsetof(RayTracingInstanceMaterialMetadata, materialIdHigh), 56u);
    EXPECT_EQ(offsetof(RayTracingInstanceMaterialMetadata, workflow), 60u);
    EXPECT_EQ(offsetof(RayTracingInstanceMaterialMetadata, baseColorTextureTableIndex), 64u);
    EXPECT_EQ(offsetof(RayTracingInstanceMaterialMetadata, metallicRoughnessTextureTableIndex), 68u);
    EXPECT_EQ(offsetof(RayTracingInstanceMaterialMetadata, normalTextureTableIndex), 72u);
    EXPECT_EQ(offsetof(RayTracingInstanceMaterialMetadata, emissiveTextureTableIndex), 76u);
    EXPECT_EQ(offsetof(RayTracingInstanceMaterialMetadata, baseColorTextureSampling), 80u);
    EXPECT_EQ(offsetof(RayTracingInstanceMaterialMetadata, metallicRoughnessTextureSampling), 112u);
    EXPECT_EQ(offsetof(RayTracingInstanceMaterialMetadata, normalTextureSampling), 144u);
    EXPECT_EQ(offsetof(RayTracingInstanceMaterialMetadata, emissiveTextureSampling), 176u);
}

TEST(GPUResourceManagerValidation, RayTracingCapableMeshUploadCreatesASInputBuffers)
{
    FakeDevice device;
    device.capabilities.supportsRaytracing = true;

    GPUResourceManager manager;
    manager.Initialize(&device);

    auto mesh = CreateMeshResource(112, MeshFactory::CreateTriangle());
    manager.UploadImmediate(mesh.get());

    const auto buffers = manager.GetMeshBuffers(mesh->GetId());
    ASSERT_TRUE(buffers.IsValid());
    ASSERT_NE(buffers.positionBuffer, nullptr);
    ASSERT_NE(buffers.indexBuffer, nullptr);

    EXPECT_TRUE(HasFlag(buffers.positionBuffer->GetUsage(), RHIBufferUsage::Vertex));
    EXPECT_TRUE(HasFlag(buffers.positionBuffer->GetUsage(), RHIBufferUsage::AccelerationStructureInput));
    EXPECT_TRUE(HasFlag(buffers.positionBuffer->GetUsage(), RHIBufferUsage::DeviceAddress));
    EXPECT_TRUE(HasFlag(buffers.positionBuffer->GetUsage(), RHIBufferUsage::ShaderResource));
    EXPECT_TRUE(HasFlag(buffers.indexBuffer->GetUsage(), RHIBufferUsage::Index));
    EXPECT_TRUE(HasFlag(buffers.indexBuffer->GetUsage(), RHIBufferUsage::AccelerationStructureInput));
    EXPECT_TRUE(HasFlag(buffers.indexBuffer->GetUsage(), RHIBufferUsage::DeviceAddress));
    EXPECT_TRUE(HasFlag(buffers.indexBuffer->GetUsage(), RHIBufferUsage::ShaderResource));
    ASSERT_NE(buffers.uvBuffer, nullptr);
    EXPECT_TRUE(HasFlag(buffers.uvBuffer->GetUsage(), RHIBufferUsage::ShaderResource));

    manager.Shutdown();
}

TEST(GPUResourceManagerValidation, RayTracingGeometryValidationRejectsInvalidASInputBuffers)
{
    RHIBufferDesc vertexDesc;
    vertexDesc.size = sizeof(float) * 9;
    vertexDesc.usage = RHIBufferUsage::Vertex |
                       RHIBufferUsage::AccelerationStructureInput |
                       RHIBufferUsage::DeviceAddress;
    vertexDesc.memoryType = RHIMemoryType::Upload;
    vertexDesc.stride = sizeof(float) * 3;
    FakeBuffer validVertexBuffer(vertexDesc);

    RHIRayTracingGeometryDesc geometry;
    geometry.type = RHIRayTracingGeometryType::Triangles;
    geometry.triangles.vertexBuffer = &validVertexBuffer;
    geometry.triangles.vertexStride = sizeof(float) * 3;
    geometry.triangles.vertexFormat = RHIFormat::RGB32_FLOAT;
    geometry.triangles.vertexCount = 3;
    EXPECT_TRUE(ValidateRHIRayTracingGeometryDesc(geometry));

    RHIBufferDesc invalidVertexUsageDesc = vertexDesc;
    invalidVertexUsageDesc.usage = RHIBufferUsage::Vertex;
    FakeBuffer invalidVertexUsageBuffer(invalidVertexUsageDesc);
    geometry.triangles.vertexBuffer = &invalidVertexUsageBuffer;
    RHIRayTracingValidationResult result = ValidateRHIRayTracingGeometryDesc(geometry);
    EXPECT_FALSE(result.valid);
    EXPECT_STREQ(result.message,
                 "triangle geometry vertex buffer requires AccelerationStructureInput and DeviceAddress usage");

    RHIBufferDesc shortVertexDesc = vertexDesc;
    shortVertexDesc.size = sizeof(float) * 3;
    FakeBuffer shortVertexBuffer(shortVertexDesc);
    geometry.triangles.vertexBuffer = &shortVertexBuffer;
    result = ValidateRHIRayTracingGeometryDesc(geometry);
    EXPECT_FALSE(result.valid);
    EXPECT_STREQ(result.message, "triangle geometry vertex range exceeds vertex buffer size");

    geometry.triangles.vertexBuffer = &validVertexBuffer;
    RHIBufferDesc indexDesc;
    indexDesc.size = sizeof(uint32) * 3;
    indexDesc.usage = RHIBufferUsage::Index |
                      RHIBufferUsage::AccelerationStructureInput |
                      RHIBufferUsage::DeviceAddress;
    indexDesc.memoryType = RHIMemoryType::Upload;
    FakeBuffer validIndexBuffer(indexDesc);
    geometry.triangles.indexBuffer = &validIndexBuffer;
    geometry.triangles.indexFormat = RHIFormat::R32_UINT;
    geometry.triangles.indexCount = 3;
    EXPECT_TRUE(ValidateRHIRayTracingGeometryDesc(geometry));

    RHIBufferDesc invalidIndexUsageDesc = indexDesc;
    invalidIndexUsageDesc.usage = RHIBufferUsage::Index;
    FakeBuffer invalidIndexUsageBuffer(invalidIndexUsageDesc);
    geometry.triangles.indexBuffer = &invalidIndexUsageBuffer;
    result = ValidateRHIRayTracingGeometryDesc(geometry);
    EXPECT_FALSE(result.valid);
    EXPECT_STREQ(result.message,
                 "indexed triangle geometry index buffer requires AccelerationStructureInput and DeviceAddress usage");

    RHIBufferDesc shortIndexDesc = indexDesc;
    shortIndexDesc.size = sizeof(uint32);
    FakeBuffer shortIndexBuffer(shortIndexDesc);
    geometry.triangles.indexBuffer = &shortIndexBuffer;
    result = ValidateRHIRayTracingGeometryDesc(geometry);
    EXPECT_FALSE(result.valid);
    EXPECT_STREQ(result.message, "indexed triangle geometry range exceeds index buffer size");

    geometry.triangles.indexBuffer = &validIndexBuffer;
    RHIBufferDesc transformDesc;
    transformDesc.size = sizeof(float) * 12;
    transformDesc.usage = RHIBufferUsage::AccelerationStructureInput | RHIBufferUsage::DeviceAddress;
    transformDesc.memoryType = RHIMemoryType::Upload;
    FakeBuffer validTransformBuffer(transformDesc);
    geometry.triangles.transformBuffer = &validTransformBuffer;
    EXPECT_TRUE(ValidateRHIRayTracingGeometryDesc(geometry));

    RHIBufferDesc shortTransformDesc = transformDesc;
    shortTransformDesc.size = sizeof(float) * 4;
    FakeBuffer shortTransformBuffer(shortTransformDesc);
    geometry.triangles.transformBuffer = &shortTransformBuffer;
    result = ValidateRHIRayTracingGeometryDesc(geometry);
    EXPECT_FALSE(result.valid);
    EXPECT_STREQ(result.message, "triangle geometry transform range exceeds transform buffer size");

    RHIBufferDesc aabbDesc;
    aabbDesc.size = 48;
    aabbDesc.usage = RHIBufferUsage::AccelerationStructureInput | RHIBufferUsage::DeviceAddress;
    aabbDesc.memoryType = RHIMemoryType::Upload;
    FakeBuffer validAABBBuffer(aabbDesc);

    RHIRayTracingGeometryDesc aabbGeometry;
    aabbGeometry.type = RHIRayTracingGeometryType::AABBs;
    aabbGeometry.aabbs.aabbBuffer = &validAABBBuffer;
    aabbGeometry.aabbs.stride = 24;
    aabbGeometry.aabbs.count = 2;
    EXPECT_TRUE(ValidateRHIRayTracingGeometryDesc(aabbGeometry));

    RHIBufferDesc invalidAABBUsageDesc = aabbDesc;
    invalidAABBUsageDesc.usage = RHIBufferUsage::ShaderResource;
    FakeBuffer invalidAABBUsageBuffer(invalidAABBUsageDesc);
    aabbGeometry.aabbs.aabbBuffer = &invalidAABBUsageBuffer;
    result = ValidateRHIRayTracingGeometryDesc(aabbGeometry);
    EXPECT_FALSE(result.valid);
    EXPECT_STREQ(result.message,
                 "AABB geometry buffer requires AccelerationStructureInput and DeviceAddress usage");

    RHIBufferDesc shortAABBDesc = aabbDesc;
    shortAABBDesc.size = 24;
    FakeBuffer shortAABBBuffer(shortAABBDesc);
    aabbGeometry.aabbs.aabbBuffer = &shortAABBBuffer;
    aabbGeometry.aabbs.offset = 24;
    result = ValidateRHIRayTracingGeometryDesc(aabbGeometry);
    EXPECT_FALSE(result.valid);
    EXPECT_STREQ(result.message, "AABB geometry range exceeds AABB buffer size");
}
TEST(GPUResourceManagerValidation, RayTracingScenePlanUsesRasterDrawClassification)
{
    FakeDevice device;
    device.capabilities.supportsRaytracing = true;

    GPUResourceManager manager;
    manager.Initialize(&device);

    auto tangentMesh = MeshFactory::CreateTriangle();
    ASSERT_TRUE(tangentMesh->GenerateTangents());
    auto mesh = CreateMeshResource(120, tangentMesh);
    manager.UploadImmediate(mesh.get());
    ASSERT_TRUE(manager.GetMeshBuffers(mesh->GetId()).IsValid());

    auto maskedMaterial = CreateMaterialResource(220, Material::AlphaMode::Mask);
    maskedMaterial->GetMaterial()->SetAlphaCutoff(0.37f);
    maskedMaterial->GetMaterial()->SetBaseColor(1.0f, 1.0f, 1.0f, 0.42f);
    maskedMaterial->GetMaterial()->SetMetallicFactor(0.31f);
    maskedMaterial->GetMaterial()->SetRoughnessFactor(0.72f);
    maskedMaterial->GetMaterial()->SetEmissiveColor(Vec3(0.1f, 0.2f, 0.3f));
    maskedMaterial->GetMaterial()->SetEmissiveStrength(2.5f);
    maskedMaterial->GetMaterial()->SetDoubleSided(true);
    TextureInfo maskedBaseColor("masked_albedo.png", 0);
    maskedBaseColor.offset = Vec2(0.25f, 0.5f);
    maskedBaseColor.scale = Vec2(2.0f, 0.5f);
    maskedBaseColor.rotation = 0.125f;
    maskedBaseColor.wrapS = TextureInfo::WrapMode::ClampToEdge;
    maskedBaseColor.wrapT = TextureInfo::WrapMode::MirrorRepeat;
    maskedBaseColor.magFilter = TextureInfo::FilterMode::Nearest;
    maskedMaterial->GetMaterial()->SetBaseColorTexture(maskedBaseColor);
    auto transparentMaterial = CreateMaterialResource(221, Material::AlphaMode::Blend);

    RenderScene scene;

    RenderObject opaqueObject;
    opaqueObject.meshId = mesh->GetId();
    opaqueObject.meshResource = mesh.get();
    opaqueObject.entityId = 10;
    opaqueObject.worldMatrix = translate(Mat4Identity(), Vec3(2.0f, 3.0f, 4.0f));
    opaqueObject.layerMask = 0xF3u;
    scene.AddObject(opaqueObject);

    RenderObject maskedObject = opaqueObject;
    maskedObject.entityId = 11;
    maskedObject.layerMask = 0x0Cu;
    maskedObject.castsShadow = false;
    maskedObject.materialIds = {maskedMaterial->GetId()};
    maskedObject.materialResources = {maskedMaterial.get()};
    scene.AddObject(maskedObject);

    RenderObject transparentObject = opaqueObject;
    transparentObject.entityId = 12;
    transparentObject.materialIds = {transparentMaterial->GetId()};
    transparentObject.materialResources = {transparentMaterial.get()};
    scene.AddObject(transparentObject);

    const std::array<uint32_t, 3> visibleObjects = {0, 1, 2};
    RayTracingSceneOptions options;
    options.instanceMask = 0x3Fu;
    RayTracingSceneBuildPlan plan = BuildRayTracingSceneBuildPlan(scene, visibleObjects, manager, options);

    ASSERT_TRUE(plan.HasWork());
    EXPECT_EQ(plan.stats.visibleObjectCount, 3u);
    EXPECT_EQ(plan.blasBuilds.size(), 2u);
    EXPECT_EQ(plan.instances.size(), 2u);
    EXPECT_EQ(plan.stats.alphaTestedInstanceCount, 1u);
    ASSERT_EQ(plan.skips.size(), 1u);
    EXPECT_EQ(plan.skips[0].reason, RayTracingSceneSkipReason::TransparentMaterial);

    ASSERT_EQ(plan.blasBuilds[0].desc.geometries.size(), 1u);
    ASSERT_EQ(plan.blasBuilds[1].desc.geometries.size(), 1u);
    EXPECT_TRUE(HasFlag(plan.blasBuilds[0].desc.geometries[0].flags, RHIRayTracingGeometryFlags::Opaque));
    EXPECT_FALSE(HasFlag(plan.blasBuilds[1].desc.geometries[0].flags, RHIRayTracingGeometryFlags::Opaque));
    EXPECT_EQ(plan.instances[0].desc.flags, RHIRayTracingInstanceFlags::ForceOpaque);
    EXPECT_EQ(plan.instances[1].desc.flags, RHIRayTracingInstanceFlags::ForceNoOpaque);
    EXPECT_EQ(plan.instances[0].desc.instanceContributionToHitGroupIndex, 0u);
    EXPECT_EQ(plan.instances[1].desc.instanceContributionToHitGroupIndex, 0u);
    EXPECT_EQ(plan.instances[0].desc.instanceId, 0u);
    EXPECT_EQ(plan.instances[1].desc.instanceId, 1u);
    EXPECT_EQ(plan.instances[0].desc.instanceMask, 0x33u);
    EXPECT_EQ(plan.instances[1].desc.instanceMask, 0x0Cu);
    EXPECT_TRUE(HasRayTracingMaterialFlag(plan.instances[0].material.flags,
                                          RayTracingMaterialMetadataFlags::ShadowCaster));
    EXPECT_FALSE(plan.instances[0].alphaTest.enabled);
    EXPECT_TRUE(plan.instances[0].alphaTest.hasIndexBuffer);
    EXPECT_TRUE(plan.instances[0].alphaTest.hasUVBuffer);
    EXPECT_TRUE(plan.instances[0].alphaTest.hasNormalBuffer);
    EXPECT_TRUE(plan.instances[0].alphaTest.hasTangentBuffer);
    EXPECT_EQ(plan.instances[0].alphaTest.indexBuffer, manager.GetMeshBuffers(mesh->GetId()).indexBuffer);
    EXPECT_EQ(plan.instances[0].alphaTest.uvBuffer, manager.GetMeshBuffers(mesh->GetId()).uvBuffer);
    EXPECT_EQ(plan.instances[0].alphaTest.normalBuffer, manager.GetMeshBuffers(mesh->GetId()).normalBuffer);
    EXPECT_EQ(plan.instances[0].alphaTest.tangentBuffer, manager.GetMeshBuffers(mesh->GetId()).tangentBuffer);
    EXPECT_TRUE(plan.instances[1].alphaTest.enabled);
    EXPECT_TRUE(plan.instances[1].alphaTest.hasUVBuffer);
    EXPECT_TRUE(plan.instances[1].alphaTest.hasNormalBuffer);
    EXPECT_TRUE(plan.instances[1].alphaTest.hasTangentBuffer);
    EXPECT_EQ(plan.instances[1].alphaTest.uvBuffer, manager.GetMeshBuffers(mesh->GetId()).uvBuffer);
    EXPECT_EQ(plan.instances[1].alphaTest.normalBuffer, manager.GetMeshBuffers(mesh->GetId()).normalBuffer);
    EXPECT_EQ(plan.instances[1].alphaTest.tangentBuffer, manager.GetMeshBuffers(mesh->GetId()).tangentBuffer);
    EXPECT_TRUE(plan.instances[1].alphaTest.hasBaseColorTexture);
    EXPECT_FALSE(plan.instances[1].alphaTest.hasResolvedBaseColorTexture);
    EXPECT_EQ(plan.instances[1].alphaTest.baseColorTextureId, Resource::InvalidResourceId);
    EXPECT_EQ(plan.instances[1].alphaTest.baseColorUVSet, 0u);
    EXPECT_EQ(plan.instances[1].alphaTest.baseColorSamplerFlags & 1u, 1u);
    EXPECT_EQ(plan.instances[1].alphaTest.baseColorSamplerFlags & 2u, 0u);
    EXPECT_EQ(plan.instances[1].alphaTest.baseColorSamplerFlags & 4u, 4u);
    EXPECT_EQ(plan.instances[1].alphaTest.baseColorSamplerFlags & 16u, 16u);
    EXPECT_FLOAT_EQ(plan.instances[1].alphaTest.baseColorUVOffset.x, 0.25f);
    EXPECT_FLOAT_EQ(plan.instances[1].alphaTest.baseColorUVOffset.y, 0.5f);
    EXPECT_FLOAT_EQ(plan.instances[1].alphaTest.baseColorUVScale.x, 2.0f);
    EXPECT_FLOAT_EQ(plan.instances[1].alphaTest.baseColorUVScale.y, 0.5f);
    EXPECT_FLOAT_EQ(plan.instances[1].alphaTest.baseColorUVRotation, 0.125f);
    EXPECT_FLOAT_EQ(plan.instances[1].alphaTest.alphaCutoff, 0.37f);
    EXPECT_FLOAT_EQ(plan.instances[1].alphaTest.baseColorAlpha, 0.42f);
    EXPECT_FLOAT_EQ(plan.instances[1].material.baseColorFactor.a, 0.42f);
    EXPECT_FLOAT_EQ(plan.instances[1].material.metallicFactor, 0.31f);
    EXPECT_FLOAT_EQ(plan.instances[1].material.roughnessFactor, 0.72f);
    EXPECT_FLOAT_EQ(plan.instances[1].material.normalScale, 1.0f);
    EXPECT_FLOAT_EQ(plan.instances[1].material.emissiveColor.x, 0.1f);
    EXPECT_FLOAT_EQ(plan.instances[1].material.emissiveColor.y, 0.2f);
    EXPECT_FLOAT_EQ(plan.instances[1].material.emissiveColor.z, 0.3f);
    EXPECT_FLOAT_EQ(plan.instances[1].material.emissiveStrength, 2.5f);
    EXPECT_FLOAT_EQ(plan.instances[1].material.alphaCutoff, 0.37f);
    EXPECT_EQ(plan.instances[1].material.materialId, maskedMaterial->GetId());
    EXPECT_EQ(plan.instances[1].material.baseColorTextureId, Resource::InvalidResourceId);
    EXPECT_EQ(plan.instances[1].material.metallicRoughnessTextureId, Resource::InvalidResourceId);
    EXPECT_EQ(plan.instances[1].material.normalTextureId, Resource::InvalidResourceId);
    EXPECT_EQ(plan.instances[1].material.emissiveTextureId, Resource::InvalidResourceId);
    EXPECT_EQ(plan.instances[1].material.baseColorTextureSampling.uvSet, 0u);
    EXPECT_EQ(plan.instances[1].material.baseColorTextureSampling.samplerFlags & 1u, 1u);
    EXPECT_EQ(plan.instances[1].material.baseColorTextureSampling.samplerFlags & 4u, 4u);
    EXPECT_EQ(plan.instances[1].material.baseColorTextureSampling.samplerFlags & 16u, 16u);
    EXPECT_FLOAT_EQ(plan.instances[1].material.baseColorTextureSampling.uvOffset.x, 0.25f);
    EXPECT_FLOAT_EQ(plan.instances[1].material.baseColorTextureSampling.uvOffset.y, 0.5f);
    EXPECT_FLOAT_EQ(plan.instances[1].material.baseColorTextureSampling.uvScale.x, 2.0f);
    EXPECT_FLOAT_EQ(plan.instances[1].material.baseColorTextureSampling.uvScale.y, 0.5f);
    EXPECT_FLOAT_EQ(plan.instances[1].material.baseColorTextureSampling.uvRotation, 0.125f);
    EXPECT_TRUE(HasRayTracingMaterialFlag(plan.instances[1].material.flags,
                                          RayTracingMaterialMetadataFlags::AlphaTest));
    EXPECT_TRUE(HasRayTracingMaterialFlag(plan.instances[1].material.flags,
                                          RayTracingMaterialMetadataFlags::DoubleSided));
    EXPECT_TRUE(HasRayTracingMaterialFlag(plan.instances[1].material.flags,
                                          RayTracingMaterialMetadataFlags::HasBaseColorTexture));
    EXPECT_FALSE(HasRayTracingMaterialFlag(plan.instances[1].material.flags,
                                           RayTracingMaterialMetadataFlags::ShadowCaster));
    EXPECT_EQ(plan.instances[0].desc.transform[3], 2.0f);
    EXPECT_EQ(plan.instances[0].desc.transform[7], 3.0f);
    EXPECT_EQ(plan.instances[0].desc.transform[11], 4.0f);

    FakeAccelerationStructure opaqueBlas;
    FakeAccelerationStructure maskedBlas;
    std::array<RHIAccelerationStructure*, 2> blasResources = {&opaqueBlas, &maskedBlas};
    RHITopLevelASDesc tlasDesc = BuildRayTracingTopLevelDesc(
        plan,
        std::span<RHIAccelerationStructure* const>(blasResources.data(), blasResources.size()));

    ASSERT_EQ(tlasDesc.instances.size(), 2u);
    EXPECT_EQ(tlasDesc.instances[0].bottomLevel, &opaqueBlas);
    EXPECT_EQ(tlasDesc.instances[1].bottomLevel, &maskedBlas);
    EXPECT_EQ(tlasDesc.instances[0].instanceId, 0u);
    EXPECT_EQ(tlasDesc.instances[1].instanceId, 1u);
    EXPECT_EQ(tlasDesc.instances[0].instanceMask, 0x33u);
    EXPECT_EQ(tlasDesc.instances[1].instanceMask, 0x0Cu);
    EXPECT_TRUE(ValidateRHITopLevelASDesc(tlasDesc));

    manager.Shutdown();
}

TEST(GPUResourceManagerValidation, RayTracingValidationRejectsNonFiniteTLASInstanceTransforms)
{
    FakeAccelerationStructure blas;

    RHIRayTracingInstanceDesc instance;
    instance.bottomLevel = &blas;
    instance.instanceId = 0;
    instance.instanceMask = 0xFFu;
    instance.transform[3] = std::numeric_limits<float>::quiet_NaN();

    RHITopLevelASDesc desc;
    desc.instances.push_back(instance);

    const RHIRayTracingValidationResult result = ValidateRHITopLevelASDesc(desc);
    EXPECT_FALSE(result.valid);
    EXPECT_STREQ(result.message, "TLAS instance transform must contain only finite values");
}

TEST(GPUResourceManagerValidation, RayTracingValidationRejectsInvalidTLASBottomLevelResources)
{
    FakeAccelerationStructure topLevelAS({RHIAccelerationStructureType::TopLevel, 4096, "FakeTLAS"});

    RHIRayTracingInstanceDesc instance;
    instance.bottomLevel = &topLevelAS;

    RHITopLevelASDesc desc;
    desc.instances.push_back(instance);

    RHIRayTracingValidationResult result = ValidateRHITopLevelASDesc(desc);
    EXPECT_FALSE(result.valid);
    EXPECT_STREQ(result.message, "TLAS instance requires a bottom-level acceleration structure");

    FakeAccelerationStructure zeroAddressBLAS(
        {RHIAccelerationStructureType::BottomLevel, 4096, "ZeroAddressBLAS"},
        0);

    desc.instances.clear();
    instance.bottomLevel = &zeroAddressBLAS;
    desc.instances.push_back(instance);

    result = ValidateRHITopLevelASDesc(desc);
    EXPECT_FALSE(result.valid);
    EXPECT_STREQ(result.message, "TLAS instance requires a non-zero BLAS GPU address");
}

TEST(GPUResourceManagerValidation, RayTracingValidationRejectsInvalidTLASInstanceBuffers)
{
    RHIBufferDesc missingAddressDesc;
    missingAddressDesc.size = sizeof(RHIRayTracingInstanceRecord);
    missingAddressDesc.usage = RHIBufferUsage::ShaderResource;
    missingAddressDesc.memoryType = RHIMemoryType::Upload;
    FakeBuffer missingAddressBuffer(missingAddressDesc);

    RHITopLevelASDesc missingAddressTLAS;
    missingAddressTLAS.instanceBuffer = &missingAddressBuffer;
    missingAddressTLAS.instanceCount = 1;

    RHIRayTracingValidationResult result = ValidateRHITopLevelASDesc(missingAddressTLAS);
    EXPECT_FALSE(result.valid);
    EXPECT_STREQ(result.message, "TLAS instance buffer requires AccelerationStructureInput and DeviceAddress usage");

    RHIBufferDesc undersizedDesc;
    undersizedDesc.size = sizeof(RHIRayTracingInstanceRecord);
    undersizedDesc.usage = RHIBufferUsage::ShaderResource | RHIBufferUsage::AccelerationStructureInput | RHIBufferUsage::DeviceAddress;
    undersizedDesc.memoryType = RHIMemoryType::Upload;
    FakeBuffer undersizedBuffer(undersizedDesc);

    RHITopLevelASDesc undersizedTLAS;
    undersizedTLAS.instanceBuffer = &undersizedBuffer;
    undersizedTLAS.instanceOffset = RVX_RAY_TRACING_INSTANCE_DESC_ALIGNMENT;
    undersizedTLAS.instanceCount = 1;

    result = ValidateRHITopLevelASDesc(undersizedTLAS);
    EXPECT_FALSE(result.valid);
    EXPECT_STREQ(result.message, "TLAS instance buffer range exceeds buffer size");

    RHIBufferDesc validDesc;
    validDesc.size = RVX_RAY_TRACING_INSTANCE_DESC_ALIGNMENT + sizeof(RHIRayTracingInstanceRecord);
    validDesc.usage = RHIBufferUsage::ShaderResource | RHIBufferUsage::AccelerationStructureInput | RHIBufferUsage::DeviceAddress;
    validDesc.memoryType = RHIMemoryType::Upload;
    FakeBuffer validBuffer(validDesc);

    RHITopLevelASDesc validTLAS;
    validTLAS.instanceBuffer = &validBuffer;
    validTLAS.instanceOffset = RVX_RAY_TRACING_INSTANCE_DESC_ALIGNMENT;
    validTLAS.instanceCount = 1;

    EXPECT_TRUE(ValidateRHITopLevelASDesc(validTLAS));
}

TEST(GPUResourceManagerValidation, RayTracingScenePlanSkipsZeroInstanceMasksBeforeBLASWork)
{
    FakeDevice device;
    device.capabilities.supportsRaytracing = true;

    GPUResourceManager manager;
    manager.Initialize(&device);

    auto mesh = CreateMeshResource(133, MeshFactory::CreateTriangle());
    manager.UploadImmediate(mesh.get());
    ASSERT_TRUE(manager.GetMeshBuffers(mesh->GetId()).IsValid());

    RenderScene scene;
    RenderObject maskedOutObject;
    maskedOutObject.meshId = mesh->GetId();
    maskedOutObject.meshResource = mesh.get();
    maskedOutObject.entityId = 60;
    maskedOutObject.layerMask = 0xF0u;
    scene.AddObject(maskedOutObject);

    RenderObject includedObject = maskedOutObject;
    includedObject.entityId = 61;
    includedObject.layerMask = 0x03u;
    scene.AddObject(includedObject);

    const std::array<uint32_t, 2> visibleObjects = {0, 1};
    RayTracingSceneOptions options;
    options.instanceMask = 0x0Fu;
    RayTracingSceneBuildPlan plan = BuildRayTracingSceneBuildPlan(scene, visibleObjects, manager, options);

    ASSERT_TRUE(plan.HasWork());
    EXPECT_EQ(plan.stats.visibleObjectCount, 2u);
    EXPECT_EQ(plan.stats.drawItemCount, 2u);
    EXPECT_EQ(plan.blasBuilds.size(), 1u);
    EXPECT_EQ(plan.instances.size(), 1u);
    EXPECT_EQ(plan.stats.blasBuildCount, 1u);
    EXPECT_EQ(plan.stats.instanceCount, 1u);
    EXPECT_EQ(plan.stats.skippedCount, 1u);
    ASSERT_EQ(plan.skips.size(), 1u);
    EXPECT_EQ(plan.skips[0].reason, RayTracingSceneSkipReason::InstanceMaskZero);
    EXPECT_EQ(plan.skips[0].objectIndex, 0u);
    EXPECT_EQ(plan.skips[0].meshId, mesh->GetId());
    EXPECT_EQ(plan.skips[0].message, "ray tracing instance mask is zero");

    ASSERT_EQ(plan.instances[0].objectIndex, 1u);
    EXPECT_EQ(plan.instances[0].entityId, 61u);
    EXPECT_EQ(plan.instances[0].desc.instanceId, 0u);
    EXPECT_EQ(plan.instances[0].desc.instanceMask, 0x03u);
    EXPECT_EQ(plan.blasBuilds[0].instanceCount, 1u);

    FakeAccelerationStructure blas;
    std::array<RHIAccelerationStructure*, 1> blasResources = {&blas};
    RHITopLevelASDesc tlasDesc = BuildRayTracingTopLevelDesc(
        plan,
        std::span<RHIAccelerationStructure* const>(blasResources.data(), blasResources.size()));

    ASSERT_EQ(tlasDesc.instances.size(), 1u);
    EXPECT_EQ(tlasDesc.instances[0].instanceId, 0u);
    EXPECT_EQ(tlasDesc.instances[0].instanceMask, 0x03u);
    EXPECT_EQ(tlasDesc.instances[0].bottomLevel, &blas);
    EXPECT_TRUE(ValidateRHITopLevelASDesc(tlasDesc));

    manager.Shutdown();
}

TEST(GPUResourceManagerValidation, RayTracingScenePlanSkipsNonFiniteTransformsBeforeBLASWork)
{
    FakeDevice device;
    device.capabilities.supportsRaytracing = true;

    GPUResourceManager manager;
    manager.Initialize(&device);

    auto mesh = CreateMeshResource(134, MeshFactory::CreateTriangle());
    manager.UploadImmediate(mesh.get());
    ASSERT_TRUE(manager.GetMeshBuffers(mesh->GetId()).IsValid());

    RenderScene scene;
    RenderObject invalidObject;
    invalidObject.meshId = mesh->GetId();
    invalidObject.meshResource = mesh.get();
    invalidObject.entityId = 62;
    invalidObject.worldMatrix[3][0] = std::numeric_limits<float>::quiet_NaN();
    scene.AddObject(invalidObject);

    RenderObject includedObject = invalidObject;
    includedObject.entityId = 63;
    includedObject.worldMatrix = translate(Mat4Identity(), Vec3(1.0f, 2.0f, 3.0f));
    scene.AddObject(includedObject);

    const std::array<uint32_t, 2> visibleObjects = {0, 1};
    RayTracingSceneBuildPlan plan = BuildRayTracingSceneBuildPlan(scene, visibleObjects, manager);

    ASSERT_TRUE(plan.HasWork());
    EXPECT_EQ(plan.stats.visibleObjectCount, 2u);
    EXPECT_EQ(plan.stats.drawItemCount, 2u);
    EXPECT_EQ(plan.blasBuilds.size(), 1u);
    EXPECT_EQ(plan.instances.size(), 1u);
    EXPECT_EQ(plan.stats.blasBuildCount, 1u);
    EXPECT_EQ(plan.stats.instanceCount, 1u);
    EXPECT_EQ(plan.stats.skippedCount, 1u);
    ASSERT_EQ(plan.skips.size(), 1u);
    EXPECT_EQ(plan.skips[0].reason, RayTracingSceneSkipReason::InvalidTransform);
    EXPECT_EQ(plan.skips[0].objectIndex, 0u);
    EXPECT_EQ(plan.skips[0].meshId, mesh->GetId());
    EXPECT_EQ(plan.skips[0].message, "ray tracing instance transform is not finite");

    ASSERT_EQ(plan.instances[0].objectIndex, 1u);
    EXPECT_EQ(plan.instances[0].entityId, 63u);
    EXPECT_EQ(plan.instances[0].desc.instanceId, 0u);
    EXPECT_EQ(plan.instances[0].desc.transform[3], 1.0f);
    EXPECT_EQ(plan.instances[0].desc.transform[7], 2.0f);
    EXPECT_EQ(plan.instances[0].desc.transform[11], 3.0f);
    EXPECT_EQ(plan.blasBuilds[0].instanceCount, 1u);

    FakeAccelerationStructure blas;
    std::array<RHIAccelerationStructure*, 1> blasResources = {&blas};
    RHITopLevelASDesc tlasDesc = BuildRayTracingTopLevelDesc(
        plan,
        std::span<RHIAccelerationStructure* const>(blasResources.data(), blasResources.size()));

    ASSERT_EQ(tlasDesc.instances.size(), 1u);
    EXPECT_TRUE(ValidateRHITopLevelASDesc(tlasDesc));

    manager.Shutdown();
}

TEST(GPUResourceManagerValidation, RayTracingSceneManagerRejectsUnsupportedDevices)
{
    FakeDevice device;
    device.capabilities.supportsRaytracing = true;

    GPUResourceManager gpuResources;
    gpuResources.Initialize(&device);

    auto mesh = CreateMeshResource(121, MeshFactory::CreateTriangle());
    gpuResources.UploadImmediate(mesh.get());

    RenderScene scene;
    RenderObject object;
    object.meshId = mesh->GetId();
    object.meshResource = mesh.get();
    object.entityId = 20;
    scene.AddObject(object);

    const std::array<uint32_t, 1> visibleObjects = {0};
    RayTracingSceneBuildPlan plan = BuildRayTracingSceneBuildPlan(scene, visibleObjects, gpuResources);
    ASSERT_TRUE(plan.HasWork());

    device.capabilities.supportsRaytracing = false;

    RayTracingSceneManager rtScene;
    rtScene.Initialize(&device);

    EXPECT_FALSE(rtScene.Prepare(plan));
    EXPECT_FALSE(rtScene.IsSupported());
    EXPECT_FALSE(rtScene.GetStats().prepared);
    EXPECT_STREQ(rtScene.GetStats().fallbackReason, "RHI device does not support ray tracing");
    EXPECT_EQ(device.createdAccelerationStructureCount, 0u);

    rtScene.Shutdown();
    gpuResources.Shutdown();
}

TEST(GPUResourceManagerValidation, RayTracingSceneManagerCreatesAndReusesAccelerationStructures)
{
    FakeDevice device;
    device.capabilities.supportsRaytracing = true;

    GPUResourceManager gpuResources;
    gpuResources.Initialize(&device);

    auto tangentMesh = MeshFactory::CreateTriangle();
    ASSERT_TRUE(tangentMesh->GenerateTangents());
    auto mesh = CreateMeshResource(122, tangentMesh);
    gpuResources.UploadImmediate(mesh.get());
    ASSERT_TRUE(gpuResources.GetMeshBuffers(mesh->GetId()).IsValid());

    auto maskedMaterial = CreateMaterialResource(222, Material::AlphaMode::Mask);
    maskedMaterial->GetMaterial()->SetAlphaCutoff(0.61f);
    maskedMaterial->GetMaterial()->SetBaseColor(1.0f, 1.0f, 1.0f, 0.25f);
    maskedMaterial->GetMaterial()->SetMetallicFactor(0.35f);
    maskedMaterial->GetMaterial()->SetRoughnessFactor(0.83f);
    maskedMaterial->GetMaterial()->SetNormalScale(0.8f);
    maskedMaterial->GetMaterial()->SetEmissiveColor(Vec3(0.1f, 0.25f, 0.5f));
    maskedMaterial->GetMaterial()->SetEmissiveStrength(3.0f);
    maskedMaterial->GetMaterial()->SetDoubleSided(true);
    TextureInfo maskedBaseColor("masked_albedo.png", 0);
    maskedBaseColor.offset = Vec2(0.25f, 0.5f);
    maskedBaseColor.scale = Vec2(2.0f, 0.5f);
    maskedBaseColor.rotation = 0.125f;
    maskedBaseColor.wrapS = TextureInfo::WrapMode::ClampToEdge;
    maskedBaseColor.wrapT = TextureInfo::WrapMode::MirrorRepeat;
    maskedBaseColor.magFilter = TextureInfo::FilterMode::Nearest;
    maskedMaterial->GetMaterial()->SetBaseColorTexture(maskedBaseColor);
    TextureInfo maskedMetallicRoughnessInfo("masked_metallic_roughness.png", 0);
    maskedMetallicRoughnessInfo.offset = Vec2(0.125f, 0.75f);
    maskedMetallicRoughnessInfo.scale = Vec2(1.5f, 0.25f);
    maskedMetallicRoughnessInfo.rotation = 0.375f;
    maskedMetallicRoughnessInfo.wrapS = TextureInfo::WrapMode::MirrorRepeat;
    maskedMetallicRoughnessInfo.wrapT = TextureInfo::WrapMode::ClampToEdge;
    maskedMaterial->GetMaterial()->SetMetallicRoughnessTexture(maskedMetallicRoughnessInfo);
    TextureInfo maskedNormalInfo("masked_normal.png", 0);
    maskedNormalInfo.offset = Vec2(0.375f, 0.625f);
    maskedNormalInfo.scale = Vec2(0.5f, 2.5f);
    maskedNormalInfo.rotation = 0.5f;
    maskedNormalInfo.wrapS = TextureInfo::WrapMode::ClampToEdge;
    maskedNormalInfo.wrapT = TextureInfo::WrapMode::MirrorRepeat;
    maskedNormalInfo.magFilter = TextureInfo::FilterMode::Nearest;
    maskedMaterial->GetMaterial()->SetNormalTexture(maskedNormalInfo);
    TextureInfo maskedEmissiveInfo("masked_emissive.png", 0);
    maskedEmissiveInfo.offset = Vec2(0.5f, 0.125f);
    maskedEmissiveInfo.scale = Vec2(0.75f, 3.0f);
    maskedEmissiveInfo.rotation = 0.25f;
    maskedEmissiveInfo.wrapS = TextureInfo::WrapMode::ClampToBorder;
    maskedEmissiveInfo.wrapT = TextureInfo::WrapMode::ClampToEdge;
    maskedEmissiveInfo.magFilter = TextureInfo::FilterMode::Nearest;
    maskedMaterial->GetMaterial()->SetEmissiveTexture(maskedEmissiveInfo);
    Resource::TextureHandle maskedAlbedoTexture(new Resource::TextureResource());
    maskedAlbedoTexture->SetId(333);
    maskedAlbedoTexture->SetName("MaskedAlbedo");
    Resource::TextureMetadata maskedAlbedoMetadata;
    maskedAlbedoMetadata.width = 1;
    maskedAlbedoMetadata.height = 1;
    maskedAlbedoMetadata.format = Resource::TextureFormat::RGBA8;
    maskedAlbedoTexture->SetData(std::vector<uint8>{255, 255, 255, 64}, maskedAlbedoMetadata);
    maskedMaterial->SetTexture("albedo", maskedAlbedoTexture);
    Resource::TextureHandle maskedMetallicRoughnessTexture(new Resource::TextureResource());
    maskedMetallicRoughnessTexture->SetId(334);
    maskedMetallicRoughnessTexture->SetName("MaskedMetallicRoughness");
    maskedMetallicRoughnessTexture->SetData(std::vector<uint8>{255, 128, 64, 255}, maskedAlbedoMetadata);
    maskedMaterial->SetTexture("metallic_roughness", maskedMetallicRoughnessTexture);
    Resource::TextureHandle maskedNormalTexture(new Resource::TextureResource());
    maskedNormalTexture->SetId(335);
    maskedNormalTexture->SetName("MaskedNormal");
    maskedNormalTexture->SetData(std::vector<uint8>{128, 128, 255, 255}, maskedAlbedoMetadata);
    maskedMaterial->SetTexture("normal", maskedNormalTexture);
    Resource::TextureHandle maskedEmissiveTexture(new Resource::TextureResource());
    maskedEmissiveTexture->SetId(336);
    maskedEmissiveTexture->SetName("MaskedEmissive");
    maskedEmissiveTexture->SetData(std::vector<uint8>{16, 32, 64, 255}, maskedAlbedoMetadata);
    maskedMaterial->SetTexture("emissive", maskedEmissiveTexture);

    RenderScene scene;
    RenderObject opaqueObject;
    opaqueObject.meshId = mesh->GetId();
    opaqueObject.meshResource = mesh.get();
    opaqueObject.entityId = 30;
    opaqueObject.layerMask = 0x81u;
    scene.AddObject(opaqueObject);

    RenderObject maskedObject = opaqueObject;
    maskedObject.entityId = 31;
    maskedObject.layerMask = 0x24u;
    maskedObject.materialIds = {maskedMaterial->GetId()};
    maskedObject.materialResources = {maskedMaterial.get()};
    scene.AddObject(maskedObject);

    const std::array<uint32_t, 2> visibleObjects = {0, 1};
    RayTracingSceneBuildPlan plan = BuildRayTracingSceneBuildPlan(scene, visibleObjects, gpuResources);
    ASSERT_TRUE(plan.HasWork());
    ASSERT_EQ(plan.blasBuilds.size(), 2u);
    ASSERT_EQ(plan.instances.size(), 2u);

    RayTracingSceneManager rtScene;
    rtScene.Initialize(&device);

    ASSERT_TRUE(rtScene.Prepare(plan));
    EXPECT_TRUE(rtScene.GetStats().prepared);
    EXPECT_TRUE(rtScene.GetStats().hasTopLevelAS);
    EXPECT_TRUE(rtScene.GetStats().hasInstanceBuffer);
    EXPECT_TRUE(rtScene.GetStats().hasMaterialMetadataBuffer);
    EXPECT_TRUE(rtScene.GetStats().hasAlphaMetadataBuffer);
    EXPECT_EQ(rtScene.GetStats().createdBLASCount, 2u);
    EXPECT_EQ(rtScene.GetStats().pendingBLASBuildCount, 2u);
    EXPECT_EQ(rtScene.GetStats().instanceCount, 2u);
    EXPECT_EQ(rtScene.GetStats().alphaTestedInstanceCount, 1u);
    ASSERT_NE(rtScene.GetTopLevelAS(), nullptr);
    ASSERT_NE(rtScene.GetInstanceBuffer(), nullptr);
    ASSERT_NE(rtScene.GetInstanceMaterialMetadataBuffer(), nullptr);
    ASSERT_NE(rtScene.GetInstanceAlphaMetadataBuffer(), nullptr);
    ASSERT_NE(rtScene.GetTopLevelScratchBuffer(), nullptr);
    const RayTracingSceneManagerStats& firstPrepareStats = rtScene.GetStats();
    EXPECT_EQ(firstPrepareStats.cachedBLASAccelerationStructureBytes, 8192u);
    EXPECT_EQ(firstPrepareStats.cachedBLASScratchBytes, 2048u);
    EXPECT_EQ(firstPrepareStats.topLevelAccelerationStructureBytes, 5120u);
    EXPECT_EQ(firstPrepareStats.topLevelScratchBytes, 2048u);
    EXPECT_EQ(firstPrepareStats.instanceBufferBytes, rtScene.GetInstanceBuffer()->GetSize());
    EXPECT_EQ(firstPrepareStats.materialMetadataBufferBytes,
              rtScene.GetInstanceMaterialMetadataBuffer()->GetSize());
    EXPECT_EQ(firstPrepareStats.alphaMetadataBufferBytes,
              rtScene.GetInstanceAlphaMetadataBuffer()->GetSize());
    const uint64 firstPrepareTrackedBytes =
        firstPrepareStats.cachedBLASAccelerationStructureBytes +
        firstPrepareStats.cachedBLASScratchBytes +
        firstPrepareStats.topLevelAccelerationStructureBytes +
        firstPrepareStats.topLevelScratchBytes +
        firstPrepareStats.instanceBufferBytes +
        firstPrepareStats.materialMetadataBufferBytes +
        firstPrepareStats.alphaMetadataBufferBytes;
    EXPECT_EQ(firstPrepareStats.totalTrackedResourceBytes, firstPrepareTrackedBytes);
    EXPECT_EQ(rtScene.GetInstanceMaterialMetadataBuffer()->GetStride(),
              static_cast<uint32>(sizeof(RayTracingInstanceMaterialMetadata)));
    EXPECT_TRUE(HasFlag(rtScene.GetInstanceMaterialMetadataBuffer()->GetUsage(), RHIBufferUsage::Structured));
    EXPECT_TRUE(HasFlag(rtScene.GetInstanceMaterialMetadataBuffer()->GetUsage(), RHIBufferUsage::ShaderResource));
    EXPECT_EQ(rtScene.GetInstanceAlphaMetadataBuffer()->GetStride(),
              static_cast<uint32>(sizeof(RayTracingInstanceAlphaMetadata)));
    EXPECT_TRUE(HasFlag(rtScene.GetInstanceAlphaMetadataBuffer()->GetUsage(), RHIBufferUsage::Structured));
    EXPECT_TRUE(HasFlag(rtScene.GetInstanceAlphaMetadataBuffer()->GetUsage(), RHIBufferUsage::ShaderResource));

    const auto* instanceBuffer = static_cast<const FakeBuffer*>(rtScene.GetInstanceBuffer());
    ASSERT_GE(instanceBuffer->GetStorage().size(), 2u * sizeof(RHIRayTracingInstanceRecord));
    RHIRayTracingInstanceRecord opaqueInstanceRecord;
    RHIRayTracingInstanceRecord maskedInstanceRecord;
    std::memcpy(&opaqueInstanceRecord,
                instanceBuffer->GetStorage().data(),
                sizeof(RHIRayTracingInstanceRecord));
    std::memcpy(&maskedInstanceRecord,
                instanceBuffer->GetStorage().data() + sizeof(RHIRayTracingInstanceRecord),
                sizeof(RHIRayTracingInstanceRecord));
    EXPECT_EQ(opaqueInstanceRecord.instanceIdAndMask & 0x00FF'FFFFu, 0u);
    EXPECT_EQ(maskedInstanceRecord.instanceIdAndMask & 0x00FF'FFFFu, 1u);
    EXPECT_EQ((opaqueInstanceRecord.instanceIdAndMask >> 24) & 0xFFu, 0x81u);
    EXPECT_EQ((maskedInstanceRecord.instanceIdAndMask >> 24) & 0xFFu, 0x24u);

    const auto* materialMetadataBuffer =
        static_cast<const FakeBuffer*>(rtScene.GetInstanceMaterialMetadataBuffer());
    ASSERT_GE(materialMetadataBuffer->GetStorage().size(), 2u * sizeof(RayTracingInstanceMaterialMetadata));
    RayTracingInstanceMaterialMetadata opaqueMaterialMetadata;
    RayTracingInstanceMaterialMetadata maskedMaterialMetadata;
    std::memcpy(&opaqueMaterialMetadata,
                materialMetadataBuffer->GetStorage().data(),
                sizeof(RayTracingInstanceMaterialMetadata));
    std::memcpy(&maskedMaterialMetadata,
                materialMetadataBuffer->GetStorage().data() + sizeof(RayTracingInstanceMaterialMetadata),
                sizeof(RayTracingInstanceMaterialMetadata));
    EXPECT_TRUE(HasRayTracingMaterialFlag(opaqueMaterialMetadata.flags,
                                          RayTracingMaterialMetadataFlags::ShadowCaster));
    EXPECT_FLOAT_EQ(maskedMaterialMetadata.baseColorFactor.a, 0.25f);
    EXPECT_FLOAT_EQ(maskedMaterialMetadata.materialFactors.x, 0.35f);
    EXPECT_FLOAT_EQ(maskedMaterialMetadata.materialFactors.y, 0.83f);
    EXPECT_FLOAT_EQ(maskedMaterialMetadata.materialFactors.z, 0.61f);
    EXPECT_FLOAT_EQ(maskedMaterialMetadata.materialFactors.w, 0.8f);
    EXPECT_FLOAT_EQ(maskedMaterialMetadata.emissiveFactor.x, 0.1f);
    EXPECT_FLOAT_EQ(maskedMaterialMetadata.emissiveFactor.y, 0.25f);
    EXPECT_FLOAT_EQ(maskedMaterialMetadata.emissiveFactor.z, 0.5f);
    EXPECT_FLOAT_EQ(maskedMaterialMetadata.emissiveFactor.w, 3.0f);
    EXPECT_EQ(maskedMaterialMetadata.materialIdLow, 222u);
    EXPECT_EQ(maskedMaterialMetadata.materialIdHigh, 0u);
    EXPECT_EQ(maskedMaterialMetadata.workflow, static_cast<uint32>(MaterialWorkflow::MetallicRoughness));
    EXPECT_EQ(maskedMaterialMetadata.baseColorTextureTableIndex, 0u);
    EXPECT_EQ(maskedMaterialMetadata.metallicRoughnessTextureTableIndex, 1u);
    EXPECT_EQ(maskedMaterialMetadata.normalTextureTableIndex, 2u);
    EXPECT_EQ(maskedMaterialMetadata.emissiveTextureTableIndex, 3u);
    EXPECT_FLOAT_EQ(maskedMaterialMetadata.baseColorTextureSampling.uvOffset.x, 0.25f);
    EXPECT_FLOAT_EQ(maskedMaterialMetadata.baseColorTextureSampling.uvOffset.y, 0.5f);
    EXPECT_FLOAT_EQ(maskedMaterialMetadata.baseColorTextureSampling.uvScale.x, 2.0f);
    EXPECT_FLOAT_EQ(maskedMaterialMetadata.baseColorTextureSampling.uvScale.y, 0.5f);
    EXPECT_FLOAT_EQ(maskedMaterialMetadata.baseColorTextureSampling.uvRotation, 0.125f);
    EXPECT_EQ(maskedMaterialMetadata.baseColorTextureSampling.uvSet, 0u);
    EXPECT_EQ(maskedMaterialMetadata.baseColorTextureSampling.samplerFlags & 1u, 1u);
    EXPECT_EQ(maskedMaterialMetadata.baseColorTextureSampling.samplerFlags & 4u, 4u);
    EXPECT_EQ(maskedMaterialMetadata.baseColorTextureSampling.samplerFlags & 16u, 16u);
    EXPECT_FLOAT_EQ(maskedMaterialMetadata.metallicRoughnessTextureSampling.uvOffset.x, 0.125f);
    EXPECT_FLOAT_EQ(maskedMaterialMetadata.metallicRoughnessTextureSampling.uvOffset.y, 0.75f);
    EXPECT_FLOAT_EQ(maskedMaterialMetadata.metallicRoughnessTextureSampling.uvScale.x, 1.5f);
    EXPECT_FLOAT_EQ(maskedMaterialMetadata.metallicRoughnessTextureSampling.uvScale.y, 0.25f);
    EXPECT_FLOAT_EQ(maskedMaterialMetadata.metallicRoughnessTextureSampling.uvRotation, 0.375f);
    EXPECT_EQ(maskedMaterialMetadata.metallicRoughnessTextureSampling.samplerFlags & 2u, 2u);
    EXPECT_EQ(maskedMaterialMetadata.metallicRoughnessTextureSampling.samplerFlags & 8u, 8u);
    EXPECT_FLOAT_EQ(maskedMaterialMetadata.normalTextureSampling.uvOffset.x, 0.375f);
    EXPECT_FLOAT_EQ(maskedMaterialMetadata.normalTextureSampling.uvOffset.y, 0.625f);
    EXPECT_FLOAT_EQ(maskedMaterialMetadata.normalTextureSampling.uvScale.x, 0.5f);
    EXPECT_FLOAT_EQ(maskedMaterialMetadata.normalTextureSampling.uvScale.y, 2.5f);
    EXPECT_FLOAT_EQ(maskedMaterialMetadata.normalTextureSampling.uvRotation, 0.5f);
    EXPECT_EQ(maskedMaterialMetadata.normalTextureSampling.samplerFlags & 1u, 1u);
    EXPECT_EQ(maskedMaterialMetadata.normalTextureSampling.samplerFlags & 4u, 4u);
    EXPECT_EQ(maskedMaterialMetadata.normalTextureSampling.samplerFlags & 16u, 16u);
    EXPECT_FLOAT_EQ(maskedMaterialMetadata.emissiveTextureSampling.uvOffset.x, 0.5f);
    EXPECT_FLOAT_EQ(maskedMaterialMetadata.emissiveTextureSampling.uvOffset.y, 0.125f);
    EXPECT_FLOAT_EQ(maskedMaterialMetadata.emissiveTextureSampling.uvScale.x, 0.75f);
    EXPECT_FLOAT_EQ(maskedMaterialMetadata.emissiveTextureSampling.uvScale.y, 3.0f);
    EXPECT_FLOAT_EQ(maskedMaterialMetadata.emissiveTextureSampling.uvRotation, 0.25f);
    EXPECT_EQ(maskedMaterialMetadata.emissiveTextureSampling.samplerFlags & 1u, 1u);
    EXPECT_EQ(maskedMaterialMetadata.emissiveTextureSampling.samplerFlags & 2u, 2u);
    EXPECT_EQ(maskedMaterialMetadata.emissiveTextureSampling.samplerFlags & 4u, 4u);
    EXPECT_TRUE(HasRayTracingMaterialFlag(maskedMaterialMetadata.flags,
                                          RayTracingMaterialMetadataFlags::AlphaTest));
    EXPECT_TRUE(HasRayTracingMaterialFlag(maskedMaterialMetadata.flags,
                                          RayTracingMaterialMetadataFlags::DoubleSided));
    EXPECT_TRUE(HasRayTracingMaterialFlag(maskedMaterialMetadata.flags,
                                          RayTracingMaterialMetadataFlags::HasBaseColorTexture));
    EXPECT_TRUE(HasRayTracingMaterialFlag(maskedMaterialMetadata.flags,
                                          RayTracingMaterialMetadataFlags::HasMetallicRoughnessTexture));
    EXPECT_TRUE(HasRayTracingMaterialFlag(maskedMaterialMetadata.flags,
                                          RayTracingMaterialMetadataFlags::HasNormalTexture));
    EXPECT_TRUE(HasRayTracingMaterialFlag(maskedMaterialMetadata.flags,
                                          RayTracingMaterialMetadataFlags::HasEmissiveTexture));
    EXPECT_TRUE(HasRayTracingMaterialFlag(maskedMaterialMetadata.flags,
                                          RayTracingMaterialMetadataFlags::ShadowCaster));

    const auto* alphaMetadataBuffer =
        static_cast<const FakeBuffer*>(rtScene.GetInstanceAlphaMetadataBuffer());
    ASSERT_GE(alphaMetadataBuffer->GetStorage().size(), 2u * sizeof(RayTracingInstanceAlphaMetadata));
    RayTracingInstanceAlphaMetadata opaqueAlphaMetadata;
    RayTracingInstanceAlphaMetadata maskedAlphaMetadata;
    std::memcpy(&opaqueAlphaMetadata,
                alphaMetadataBuffer->GetStorage().data(),
                sizeof(RayTracingInstanceAlphaMetadata));
    std::memcpy(&maskedAlphaMetadata,
                alphaMetadataBuffer->GetStorage().data() + sizeof(RayTracingInstanceAlphaMetadata),
                sizeof(RayTracingInstanceAlphaMetadata));
    EXPECT_FALSE((opaqueAlphaMetadata.flags &
                  static_cast<uint32>(RayTracingInstanceAlphaMetadataFlags::AlphaTestEnabled)) != 0u);
    EXPECT_TRUE((opaqueAlphaMetadata.flags &
                 static_cast<uint32>(RayTracingInstanceAlphaMetadataFlags::HasUVBuffer)) != 0u);
    EXPECT_TRUE((opaqueAlphaMetadata.flags &
                 static_cast<uint32>(RayTracingInstanceAlphaMetadataFlags::HasIndexBuffer)) != 0u);
    EXPECT_TRUE((opaqueAlphaMetadata.flags &
                 static_cast<uint32>(RayTracingInstanceAlphaMetadataFlags::HasNormalBuffer)) != 0u);
    EXPECT_TRUE((opaqueAlphaMetadata.flags &
                 static_cast<uint32>(RayTracingInstanceAlphaMetadataFlags::HasTangentBuffer)) != 0u);
    EXPECT_TRUE((opaqueAlphaMetadata.flags &
                 static_cast<uint32>(RayTracingInstanceAlphaMetadataFlags::IndexFormatUInt32)) != 0u);
    EXPECT_EQ(opaqueAlphaMetadata.indexBufferTableIndex, 0u);
    EXPECT_EQ(opaqueAlphaMetadata.uvBufferTableIndex, 0u);
    EXPECT_EQ(opaqueAlphaMetadata.normalBufferTableIndex, 0u);
    EXPECT_EQ(opaqueAlphaMetadata.tangentBufferTableIndex, 0u);
    EXPECT_TRUE((maskedAlphaMetadata.flags &
                 static_cast<uint32>(RayTracingInstanceAlphaMetadataFlags::AlphaTestEnabled)) != 0u);
    EXPECT_TRUE((maskedAlphaMetadata.flags &
                 static_cast<uint32>(RayTracingInstanceAlphaMetadataFlags::HasUVBuffer)) != 0u);
    EXPECT_TRUE((maskedAlphaMetadata.flags &
                 static_cast<uint32>(RayTracingInstanceAlphaMetadataFlags::HasIndexBuffer)) != 0u);
    EXPECT_TRUE((maskedAlphaMetadata.flags &
                 static_cast<uint32>(RayTracingInstanceAlphaMetadataFlags::HasNormalBuffer)) != 0u);
    EXPECT_TRUE((maskedAlphaMetadata.flags &
                 static_cast<uint32>(RayTracingInstanceAlphaMetadataFlags::HasTangentBuffer)) != 0u);
    EXPECT_TRUE((maskedAlphaMetadata.flags &
                 static_cast<uint32>(RayTracingInstanceAlphaMetadataFlags::IndexFormatUInt32)) != 0u);
    EXPECT_FLOAT_EQ(maskedAlphaMetadata.alphaCutoff, 0.61f);
    EXPECT_FLOAT_EQ(maskedAlphaMetadata.baseColorAlpha, 0.25f);
    EXPECT_EQ(maskedAlphaMetadata.baseColorTextureIdLow, 333u);
    EXPECT_EQ(maskedAlphaMetadata.baseColorTextureIdHigh, 0u);
    EXPECT_EQ(maskedAlphaMetadata.baseColorTextureTableIndex, 0u);
    EXPECT_EQ(maskedAlphaMetadata.indexBufferTableIndex, 0u);
    EXPECT_EQ(maskedAlphaMetadata.uvBufferTableIndex, 0u);
    EXPECT_EQ(maskedAlphaMetadata.normalBufferTableIndex, 0u);
    EXPECT_EQ(maskedAlphaMetadata.tangentBufferTableIndex, 0u);
    EXPECT_EQ(maskedAlphaMetadata.indexElementOffset, 0u);
    EXPECT_EQ(maskedAlphaMetadata.baseVertex, 0u);
    EXPECT_EQ(maskedAlphaMetadata.baseColorSamplerFlags & 1u, 1u);
    EXPECT_EQ(maskedAlphaMetadata.baseColorSamplerFlags & 2u, 0u);
    EXPECT_EQ(maskedAlphaMetadata.baseColorSamplerFlags & 4u, 4u);
    EXPECT_EQ(maskedAlphaMetadata.baseColorSamplerFlags & 16u, 16u);
    EXPECT_FLOAT_EQ(maskedAlphaMetadata.baseColorUVOffset.x, 0.25f);
    EXPECT_FLOAT_EQ(maskedAlphaMetadata.baseColorUVOffset.y, 0.5f);
    EXPECT_FLOAT_EQ(maskedAlphaMetadata.baseColorUVScale.x, 2.0f);
    EXPECT_FLOAT_EQ(maskedAlphaMetadata.baseColorUVScale.y, 0.5f);
    EXPECT_FLOAT_EQ(maskedAlphaMetadata.baseColorUVRotation, 0.125f);
    ASSERT_EQ(rtScene.GetInstanceAlphaTextureTable().size(), 1u);
    EXPECT_EQ(rtScene.GetInstanceAlphaTextureTable()[0], maskedAlbedoTexture.GetId());
    ASSERT_EQ(rtScene.GetInstanceMaterialTextureTable().size(), 4u);
    EXPECT_EQ(rtScene.GetInstanceMaterialTextureTable()[0], maskedAlbedoTexture.GetId());
    EXPECT_EQ(rtScene.GetInstanceMaterialTextureTable()[1], maskedMetallicRoughnessTexture.GetId());
    EXPECT_EQ(rtScene.GetInstanceMaterialTextureTable()[2], maskedNormalTexture.GetId());
    EXPECT_EQ(rtScene.GetInstanceMaterialTextureTable()[3], maskedEmissiveTexture.GetId());
    ASSERT_EQ(rtScene.GetInstanceAlphaIndexBufferTable().size(), 1u);
    ASSERT_EQ(rtScene.GetInstanceAlphaUVBufferTable().size(), 1u);
    ASSERT_EQ(rtScene.GetInstanceAlphaNormalBufferTable().size(), 1u);
    ASSERT_EQ(rtScene.GetInstanceAlphaTangentBufferTable().size(), 1u);
    EXPECT_EQ(rtScene.GetInstanceAlphaIndexBufferTable()[0], gpuResources.GetMeshBuffers(mesh->GetId()).indexBuffer);
    EXPECT_EQ(rtScene.GetInstanceAlphaUVBufferTable()[0], gpuResources.GetMeshBuffers(mesh->GetId()).uvBuffer);
    EXPECT_EQ(rtScene.GetInstanceAlphaNormalBufferTable()[0], gpuResources.GetMeshBuffers(mesh->GetId()).normalBuffer);
    EXPECT_EQ(rtScene.GetInstanceAlphaTangentBufferTable()[0], gpuResources.GetMeshBuffers(mesh->GetId()).tangentBuffer);
    EXPECT_EQ(rtScene.GetStats().alphaTextureCount, 1u);
    EXPECT_EQ(rtScene.GetStats().materialTextureCount, 4u);
    EXPECT_EQ(rtScene.GetStats().alphaIndexBufferCount, 1u);
    EXPECT_EQ(rtScene.GetStats().alphaUVBufferCount, 1u);
    EXPECT_EQ(rtScene.GetStats().alphaNormalBufferCount, 1u);
    EXPECT_EQ(rtScene.GetStats().alphaTangentBufferCount, 1u);

    EXPECT_EQ(rtScene.GetTopLevelBuildDesc().instanceBuffer, rtScene.GetInstanceBuffer());
    EXPECT_EQ(rtScene.GetTopLevelBuildDesc().instanceCount, 2u);
    EXPECT_TRUE(rtScene.GetTopLevelBuildDesc().instances.empty());
    EXPECT_EQ(device.createdAccelerationStructureCount, 3u);

    FakeCommandContext ctx;
    rtScene.RecordBuildCommands(ctx);
    EXPECT_EQ(ctx.buildBottomLevelASCount, 2u);
    EXPECT_EQ(ctx.buildTopLevelASCount, 1u);
    EXPECT_EQ(ctx.lastTopLevelDesc.instanceBuffer, rtScene.GetInstanceBuffer());
    EXPECT_EQ(ctx.lastTopLevelDesc.instanceCount, 2u);
    EXPECT_EQ(rtScene.GetStats().recordedBLASBuildCount, 2u);
    EXPECT_TRUE(rtScene.GetStats().recordedTLASBuild);
    EXPECT_EQ(rtScene.GetStats().pendingBLASBuildCount, 0u);

    ASSERT_TRUE(rtScene.Prepare(plan));
    EXPECT_EQ(rtScene.GetStats().createdBLASCount, 0u);
    EXPECT_EQ(rtScene.GetStats().reusedBLASCount, 2u);
    EXPECT_EQ(rtScene.GetStats().pendingBLASBuildCount, 0u);
    EXPECT_EQ(rtScene.GetStats().cachedBLASAccelerationStructureBytes, 8192u);
    EXPECT_EQ(rtScene.GetStats().cachedBLASScratchBytes, 0u);
    EXPECT_EQ(rtScene.GetStats().topLevelAccelerationStructureBytes, 5120u);
    EXPECT_EQ(rtScene.GetStats().topLevelScratchBytes, 2048u);
    EXPECT_EQ(rtScene.GetStats().totalTrackedResourceBytes,
              firstPrepareTrackedBytes - 2048u);
    EXPECT_EQ(device.createdAccelerationStructureCount, 3u);

    FakeCommandContext reuseCtx;
    rtScene.RecordBuildCommands(reuseCtx);
    EXPECT_EQ(reuseCtx.buildBottomLevelASCount, 0u);
    EXPECT_EQ(reuseCtx.buildTopLevelASCount, 1u);
    EXPECT_EQ(reuseCtx.lastTopLevelDesc.instanceBuffer, rtScene.GetInstanceBuffer());

    rtScene.Shutdown();
    gpuResources.Shutdown();
}

TEST(GPUResourceManagerValidation, RayTracingSceneManagerQueuesRetiredBLASScratchWithoutRebuildingCachedAS)
{
    FakeDevice device;
    device.capabilities.supportsRaytracing = true;

    GPUResourceManager gpuResources;
    gpuResources.Initialize(&device);

    auto mesh = CreateMeshResource(131, MeshFactory::CreateTriangle());
    gpuResources.UploadImmediate(mesh.get());
    ASSERT_TRUE(gpuResources.GetMeshBuffers(mesh->GetId()).IsValid());

    RenderScene scene;
    RenderObject object;
    object.meshId = mesh->GetId();
    object.meshResource = mesh.get();
    object.entityId = 54;
    scene.AddObject(object);

    const std::array<uint32_t, 1> visibleObjects = {0};
    RayTracingSceneBuildPlan plan = BuildRayTracingSceneBuildPlan(scene, visibleObjects, gpuResources);
    ASSERT_TRUE(plan.HasWork());
    ASSERT_EQ(plan.blasBuilds.size(), 1u);

    RayTracingSceneManager rtScene;
    rtScene.Initialize(&device);

    ASSERT_TRUE(rtScene.Prepare(plan));
    EXPECT_EQ(rtScene.GetStats().createdBLASCount, 1u);
    EXPECT_EQ(rtScene.GetStats().cachedBLASScratchBytes, 1024u);
    EXPECT_EQ(rtScene.GetStats().releasedBLASScratchCount, 0u);
    EXPECT_EQ(rtScene.GetStats().pendingBLASScratchReleaseCount, 0u);

    FakeCommandContext buildCtx;
    rtScene.RecordBuildCommands(buildCtx);
    EXPECT_EQ(buildCtx.buildBottomLevelASCount, 1u);
    EXPECT_EQ(buildCtx.buildTopLevelASCount, 1u);
    EXPECT_EQ(rtScene.GetStats().recordedBLASBuildCount, 1u);
    EXPECT_EQ(rtScene.GetStats().cachedBLASScratchBytes, 1024u);
    EXPECT_EQ(rtScene.GetStats().pendingBLASScratchReleaseCount, 1u);

    ASSERT_TRUE(rtScene.Prepare(plan));
    EXPECT_EQ(rtScene.GetStats().createdBLASCount, 0u);
    EXPECT_EQ(rtScene.GetStats().reusedBLASCount, 1u);
    EXPECT_EQ(rtScene.GetStats().pendingBLASBuildCount, 0u);
    EXPECT_EQ(rtScene.GetStats().cachedBLASAccelerationStructureBytes, 4096u);
    EXPECT_EQ(rtScene.GetStats().cachedBLASScratchBytes, 0u);
    EXPECT_EQ(rtScene.GetStats().releasedBLASScratchCount, 1u);
    EXPECT_EQ(rtScene.GetStats().releasedBLASScratchBytes, 1024u);
    EXPECT_EQ(rtScene.GetStats().pendingBLASScratchReleaseCount, 0u);

    FakeCommandContext reuseCtx;
    rtScene.RecordBuildCommands(reuseCtx);
    EXPECT_EQ(reuseCtx.buildBottomLevelASCount, 0u);
    EXPECT_EQ(reuseCtx.buildTopLevelASCount, 1u);
    EXPECT_EQ(rtScene.GetStats().recordedBLASBuildCount, 0u);

    rtScene.Shutdown();
    gpuResources.Shutdown();
}

TEST(GPUResourceManagerValidation, RayTracingScenePlanConsumesLoadedGltfAlphaTexture)
{
    const std::filesystem::path gltfPath =
        std::filesystem::temp_directory_path() / "rvx_rt_alpha_mask_texture_asset.gltf";
    const std::string gltf = R"gltf({
  "asset": { "version": "2.0", "generator": "RenderVerseX RT alpha asset gate" },
  "scene": 0,
  "scenes": [ { "name": "RTAlphaAssetScene", "nodes": [ 0 ] } ],
  "nodes": [ { "name": "RTAlphaMaskedTriangle", "mesh": 0 } ],
  "meshes": [
    {
      "name": "RTAlphaMaskedTriangleMesh",
      "primitives": [
        {
          "attributes": { "POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2 },
          "indices": 3,
          "material": 0,
          "mode": 4
        }
      ]
    }
  ],
  "materials": [
    {
      "name": "RTAlphaMaskedMaterial",
      "pbrMetallicRoughness": {
        "baseColorFactor": [ 1.0, 1.0, 1.0, 1.0 ],
        "baseColorTexture": { "index": 0, "texCoord": 0 },
        "metallicFactor": 0.0,
        "roughnessFactor": 0.8
      },
      "alphaMode": "MASK",
      "alphaCutoff": 0.5,
      "doubleSided": true
    }
  ],
  "samplers": [ { "magFilter": 9728, "minFilter": 9728, "wrapS": 33071, "wrapT": 33071 } ],
  "textures": [ { "sampler": 0, "source": 0 } ],
  "images": [
    {
      "uri": "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+/p9sAAAAASUVORK5CYII="
    }
  ],
  "buffers": [
    {
      "byteLength": 102,
      "uri": "data:application/octet-stream;base64,AACAvwAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAAAAAAAAAAAAIA/AAAAAAAAAAAAAIA/AAAAAAAAAAAAAIA/AAAAAAAAAAAAAIA/AAAAAAAAAD8AAIA/AAABAAIA"
    }
  ],
  "bufferViews": [
    { "buffer": 0, "byteOffset": 0, "byteLength": 36, "target": 34962 },
    { "buffer": 0, "byteOffset": 36, "byteLength": 36, "target": 34962 },
    { "buffer": 0, "byteOffset": 72, "byteLength": 24, "target": 34962 },
    { "buffer": 0, "byteOffset": 96, "byteLength": 6, "target": 34963 }
  ],
  "accessors": [
    { "bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3", "min": [ -1.0, 0.0, 0.0 ], "max": [ 1.0, 1.0, 0.0 ] },
    { "bufferView": 1, "componentType": 5126, "count": 3, "type": "VEC3" },
    { "bufferView": 2, "componentType": 5126, "count": 3, "type": "VEC2" },
    { "bufferView": 3, "componentType": 5123, "count": 3, "type": "SCALAR" }
  ]
})gltf";

    {
        std::ofstream stream(gltfPath, std::ios::binary);
        ASSERT_TRUE(stream.is_open());
        stream << gltf;
    }

    Resource::ModelLoader loader(nullptr);
    Resource::ModelHandle model(static_cast<Resource::ModelResource*>(loader.Load(gltfPath.string())));
    std::filesystem::remove(gltfPath);

    ASSERT_TRUE(model.IsValid());
    ASSERT_TRUE(model.IsLoaded());
    ASSERT_EQ(model->GetMeshCount(), 1u);
    ASSERT_EQ(model->GetMaterialCount(), 1u);

    Resource::MeshHandle mesh = model->GetMesh(0);
    Resource::MaterialHandle material = model->GetMaterial(0);
    ASSERT_TRUE(mesh.IsValid());
    ASSERT_TRUE(material.IsValid());
    ASSERT_NE(mesh->GetMesh(), nullptr);
    ASSERT_NE(material->GetMaterial(), nullptr);

    const std::shared_ptr<Material> materialData = material->GetMaterial();
    EXPECT_EQ(materialData->GetAlphaMode(), Material::AlphaMode::Mask);
    EXPECT_FLOAT_EQ(materialData->GetAlphaCutoff(), 0.5f);
    ASSERT_TRUE(materialData->GetBaseColorTexture().has_value());
    EXPECT_EQ(materialData->GetBaseColorTexture()->imageId, 0);
    EXPECT_EQ(materialData->GetBaseColorTexture()->uvSet, 0);
    EXPECT_EQ(materialData->GetBaseColorTexture()->wrapS, TextureInfo::WrapMode::ClampToEdge);
    EXPECT_EQ(materialData->GetBaseColorTexture()->wrapT, TextureInfo::WrapMode::ClampToEdge);
    EXPECT_EQ(materialData->GetBaseColorTexture()->magFilter, TextureInfo::FilterMode::Nearest);

    Resource::TextureHandle albedoTexture = material->GetAlbedoTexture();
    ASSERT_TRUE(albedoTexture.IsValid());
    ASSERT_TRUE(albedoTexture.IsLoaded());
    EXPECT_FALSE(albedoTexture->IsDefaultFallback());
    EXPECT_EQ(albedoTexture->GetUsage(), Resource::TextureUsage::Color);
    EXPECT_TRUE(albedoTexture->IsSRGB());
    EXPECT_EQ(albedoTexture->GetWidth(), 1u);
    EXPECT_EQ(albedoTexture->GetHeight(), 1u);

    FakeDevice device;
    device.capabilities.supportsRaytracing = true;
    device.supportStagedCopy = true;

    GPUResourceManager gpuResources;
    gpuResources.Initialize(&device);
    gpuResources.UploadImmediate(mesh.Get());
    gpuResources.UploadImmediate(albedoTexture.Get());

    const MeshGPUBuffers meshBuffers = gpuResources.GetMeshBuffers(mesh.GetId());
    ASSERT_TRUE(meshBuffers.IsValid());
    ASSERT_NE(meshBuffers.uvBuffer, nullptr);
    ASSERT_TRUE(gpuResources.IsResident(albedoTexture.GetId()));
    ASSERT_NE(gpuResources.GetTexture(albedoTexture.GetId()), nullptr);

    RenderScene scene;
    RenderObject object;
    object.meshId = mesh.GetId();
    object.meshResource = mesh.Get();
    object.entityId = 70;
    object.materialIds = {material.GetId()};
    object.materialResources = {material.Get()};
    scene.AddObject(object);

    const std::array<uint32_t, 1> visibleObjects = {0};
    RayTracingSceneBuildPlan plan = BuildRayTracingSceneBuildPlan(scene, visibleObjects, gpuResources);
    ASSERT_TRUE(plan.HasWork());
    EXPECT_TRUE(plan.skips.empty());
    ASSERT_EQ(plan.blasBuilds.size(), 1u);
    ASSERT_EQ(plan.blasBuilds[0].desc.geometries.size(), 1u);
    ASSERT_EQ(plan.instances.size(), 1u);
    EXPECT_EQ(plan.stats.alphaTestedInstanceCount, 1u);
    EXPECT_FALSE(HasFlag(plan.blasBuilds[0].desc.geometries[0].flags, RHIRayTracingGeometryFlags::Opaque));

    const RayTracingTLASInstance& instance = plan.instances[0];
    EXPECT_EQ(instance.renderMode, MaterialRenderMode::Masked);
    EXPECT_EQ(instance.desc.flags, RHIRayTracingInstanceFlags::ForceNoOpaque);
    EXPECT_TRUE(instance.alphaTest.enabled);
    EXPECT_TRUE(instance.alphaTest.hasIndexBuffer);
    EXPECT_TRUE(instance.alphaTest.hasUVBuffer);
    EXPECT_TRUE(instance.alphaTest.hasBaseColorTexture);
    EXPECT_TRUE(instance.alphaTest.hasResolvedBaseColorTexture);
    EXPECT_EQ(instance.alphaTest.baseColorTextureId, albedoTexture.GetId());
    EXPECT_EQ(instance.alphaTest.indexBuffer, meshBuffers.indexBuffer);
    EXPECT_EQ(instance.alphaTest.uvBuffer, meshBuffers.uvBuffer);
    EXPECT_FLOAT_EQ(instance.alphaTest.alphaCutoff, 0.5f);
    EXPECT_FLOAT_EQ(instance.alphaTest.baseColorAlpha, 1.0f);
    EXPECT_EQ(instance.material.baseColorTextureId, albedoTexture.GetId());
    EXPECT_TRUE(HasRayTracingMaterialFlag(instance.material.flags,
                                          RayTracingMaterialMetadataFlags::AlphaTest));
    EXPECT_TRUE(HasRayTracingMaterialFlag(instance.material.flags,
                                          RayTracingMaterialMetadataFlags::DoubleSided));
    EXPECT_TRUE(HasRayTracingMaterialFlag(instance.material.flags,
                                          RayTracingMaterialMetadataFlags::HasBaseColorTexture));

    RayTracingSceneManager rtScene;
    rtScene.Initialize(&device);
    ASSERT_TRUE(rtScene.Prepare(plan));
    EXPECT_TRUE(rtScene.GetStats().prepared);
    EXPECT_EQ(rtScene.GetStats().alphaTestedInstanceCount, 1u);
    EXPECT_EQ(rtScene.GetStats().alphaTextureCount, 1u);
    EXPECT_EQ(rtScene.GetStats().materialTextureCount, 1u);
    EXPECT_EQ(rtScene.GetStats().alphaIndexBufferCount, 1u);
    EXPECT_EQ(rtScene.GetStats().alphaUVBufferCount, 1u);

    ASSERT_EQ(rtScene.GetInstanceAlphaTextureTable().size(), 1u);
    EXPECT_EQ(rtScene.GetInstanceAlphaTextureTable()[0], albedoTexture.GetId());
    ASSERT_EQ(rtScene.GetInstanceMaterialTextureTable().size(), 1u);
    EXPECT_EQ(rtScene.GetInstanceMaterialTextureTable()[0], albedoTexture.GetId());
    ASSERT_EQ(rtScene.GetInstanceAlphaIndexBufferTable().size(), 1u);
    ASSERT_EQ(rtScene.GetInstanceAlphaUVBufferTable().size(), 1u);
    EXPECT_EQ(rtScene.GetInstanceAlphaIndexBufferTable()[0], meshBuffers.indexBuffer);
    EXPECT_EQ(rtScene.GetInstanceAlphaUVBufferTable()[0], meshBuffers.uvBuffer);

    const auto* alphaMetadataBuffer =
        static_cast<const FakeBuffer*>(rtScene.GetInstanceAlphaMetadataBuffer());
    ASSERT_NE(alphaMetadataBuffer, nullptr);
    ASSERT_GE(alphaMetadataBuffer->GetStorage().size(), sizeof(RayTracingInstanceAlphaMetadata));
    RayTracingInstanceAlphaMetadata alphaMetadata;
    std::memcpy(&alphaMetadata, alphaMetadataBuffer->GetStorage().data(), sizeof(alphaMetadata));
    EXPECT_TRUE((alphaMetadata.flags &
                 static_cast<uint32>(RayTracingInstanceAlphaMetadataFlags::AlphaTestEnabled)) != 0u);
    EXPECT_TRUE((alphaMetadata.flags &
                 static_cast<uint32>(RayTracingInstanceAlphaMetadataFlags::HasBaseColorTexture)) != 0u);
    EXPECT_EQ(alphaMetadata.baseColorTextureIdLow,
              static_cast<uint32>(albedoTexture.GetId() & 0xFFFF'FFFFull));
    EXPECT_EQ(alphaMetadata.baseColorTextureIdHigh,
              static_cast<uint32>((albedoTexture.GetId() >> 32) & 0xFFFF'FFFFull));
    EXPECT_EQ(alphaMetadata.baseColorTextureTableIndex, 0u);
    EXPECT_EQ(alphaMetadata.indexBufferTableIndex, 0u);
    EXPECT_EQ(alphaMetadata.uvBufferTableIndex, 0u);
    EXPECT_FLOAT_EQ(alphaMetadata.alphaCutoff, 0.5f);
    EXPECT_FLOAT_EQ(alphaMetadata.baseColorAlpha, 1.0f);

    rtScene.Shutdown();
    gpuResources.Shutdown();
}

TEST(GPUResourceManagerValidation, RayTracingSceneManagerEvictsUnusedBLASCacheEntries)
{
    FakeDevice device;
    device.capabilities.supportsRaytracing = true;

    GPUResourceManager gpuResources;
    gpuResources.Initialize(&device);

    auto meshA = CreateMeshResource(127, MeshFactory::CreateTriangle());
    auto meshB = CreateMeshResource(128, MeshFactory::CreateTriangle());
    gpuResources.UploadImmediate(meshA.get());
    gpuResources.UploadImmediate(meshB.get());
    ASSERT_TRUE(gpuResources.GetMeshBuffers(meshA->GetId()).IsValid());
    ASSERT_TRUE(gpuResources.GetMeshBuffers(meshB->GetId()).IsValid());

    RenderScene sceneA;
    RenderObject objectA;
    objectA.meshId = meshA->GetId();
    objectA.meshResource = meshA.get();
    objectA.entityId = 50;
    sceneA.AddObject(objectA);

    RenderScene sceneB;
    RenderObject objectB;
    objectB.meshId = meshB->GetId();
    objectB.meshResource = meshB.get();
    objectB.entityId = 51;
    sceneB.AddObject(objectB);

    const std::array<uint32_t, 1> visibleObjects = {0};
    RayTracingSceneBuildPlan planA = BuildRayTracingSceneBuildPlan(sceneA, visibleObjects, gpuResources);
    RayTracingSceneBuildPlan planB = BuildRayTracingSceneBuildPlan(sceneB, visibleObjects, gpuResources);
    ASSERT_TRUE(planA.HasWork());
    ASSERT_TRUE(planB.HasWork());
    ASSERT_EQ(planA.blasBuilds.size(), 1u);
    ASSERT_EQ(planB.blasBuilds.size(), 1u);
    ASSERT_NE(planA.blasBuilds[0].key.meshId, planB.blasBuilds[0].key.meshId);

    RayTracingSceneManager rtScene;
    rtScene.Initialize(&device);
    EXPECT_EQ(rtScene.GetBLASCacheEvictionFrameThreshold(), 300u);
    rtScene.SetBLASCacheEvictionFrameThreshold(0);
    EXPECT_EQ(rtScene.GetBLASCacheEvictionFrameThreshold(), 0u);

    ASSERT_TRUE(rtScene.Prepare(planA));
    EXPECT_EQ(rtScene.GetStats().createdBLASCount, 1u);
    EXPECT_EQ(rtScene.GetStats().reusedBLASCount, 0u);
    EXPECT_EQ(rtScene.GetStats().evictedBLASCount, 0u);
    EXPECT_EQ(rtScene.GetStats().cachedBLASCount, 1u);
    EXPECT_EQ(rtScene.GetStats().cachedBLASAccelerationStructureBytes, 4096u);
    EXPECT_EQ(rtScene.GetStats().cachedBLASScratchBytes, 1024u);
    EXPECT_EQ(rtScene.GetCachedBLASCount(), 1u);
    FakeCommandContext buildA;
    rtScene.RecordBuildCommands(buildA);
    EXPECT_EQ(rtScene.GetStats().pendingBLASBuildCount, 0u);

    ASSERT_TRUE(rtScene.Prepare(planA));
    EXPECT_EQ(rtScene.GetStats().createdBLASCount, 0u);
    EXPECT_EQ(rtScene.GetStats().reusedBLASCount, 1u);
    EXPECT_EQ(rtScene.GetStats().evictedBLASCount, 0u);
    EXPECT_EQ(rtScene.GetStats().cachedBLASCount, 1u);
    EXPECT_EQ(rtScene.GetStats().cachedBLASAccelerationStructureBytes, 4096u);
    EXPECT_EQ(rtScene.GetStats().cachedBLASScratchBytes, 0u);
    EXPECT_EQ(rtScene.GetCachedBLASCount(), 1u);

    ASSERT_TRUE(rtScene.Prepare(planB));
    EXPECT_EQ(rtScene.GetStats().createdBLASCount, 1u);
    EXPECT_EQ(rtScene.GetStats().reusedBLASCount, 0u);
    EXPECT_EQ(rtScene.GetStats().evictedBLASCount, 1u);
    EXPECT_EQ(rtScene.GetStats().cachedBLASCount, 1u);
    EXPECT_EQ(rtScene.GetStats().cachedBLASAccelerationStructureBytes, 4096u);
    EXPECT_EQ(rtScene.GetStats().cachedBLASScratchBytes, 1024u);
    EXPECT_EQ(rtScene.GetCachedBLASCount(), 1u);

    ASSERT_TRUE(rtScene.Prepare(planA));
    EXPECT_EQ(rtScene.GetStats().createdBLASCount, 1u);
    EXPECT_EQ(rtScene.GetStats().reusedBLASCount, 0u);
    EXPECT_EQ(rtScene.GetStats().evictedBLASCount, 1u);
    EXPECT_EQ(rtScene.GetStats().cachedBLASCount, 1u);
    EXPECT_EQ(rtScene.GetStats().cachedBLASAccelerationStructureBytes, 4096u);
    EXPECT_EQ(rtScene.GetStats().cachedBLASScratchBytes, 1024u);
    EXPECT_EQ(rtScene.GetCachedBLASCount(), 1u);

    rtScene.Shutdown();
    gpuResources.Shutdown();
}

TEST(GPUResourceManagerValidation, RayTracingSceneManagerPrunesUnusedBLASUnderTrackedResourceBudget)
{
    FakeDevice device;
    device.capabilities.supportsRaytracing = true;

    GPUResourceManager gpuResources;
    gpuResources.Initialize(&device);

    auto meshA = CreateMeshResource(129, MeshFactory::CreateTriangle());
    auto meshB = CreateMeshResource(130, MeshFactory::CreateTriangle());
    gpuResources.UploadImmediate(meshA.get());
    gpuResources.UploadImmediate(meshB.get());
    ASSERT_TRUE(gpuResources.GetMeshBuffers(meshA->GetId()).IsValid());
    ASSERT_TRUE(gpuResources.GetMeshBuffers(meshB->GetId()).IsValid());

    RenderScene sceneA;
    RenderObject objectA;
    objectA.meshId = meshA->GetId();
    objectA.meshResource = meshA.get();
    objectA.entityId = 52;
    sceneA.AddObject(objectA);

    RenderScene sceneB;
    RenderObject objectB;
    objectB.meshId = meshB->GetId();
    objectB.meshResource = meshB.get();
    objectB.entityId = 53;
    sceneB.AddObject(objectB);

    const std::array<uint32_t, 1> visibleObjects = {0};
    RayTracingSceneBuildPlan planA = BuildRayTracingSceneBuildPlan(sceneA, visibleObjects, gpuResources);
    RayTracingSceneBuildPlan planB = BuildRayTracingSceneBuildPlan(sceneB, visibleObjects, gpuResources);
    ASSERT_TRUE(planA.HasWork());
    ASSERT_TRUE(planB.HasWork());
    ASSERT_EQ(planA.blasBuilds.size(), 1u);
    ASSERT_EQ(planB.blasBuilds.size(), 1u);
    ASSERT_NE(planA.blasBuilds[0].key.meshId, planB.blasBuilds[0].key.meshId);

    RayTracingSceneManager rtScene;
    rtScene.Initialize(&device);
    EXPECT_EQ(rtScene.GetBLASCacheEvictionFrameThreshold(), 300u);
    rtScene.SetTrackedResourceBudget(1u);
    EXPECT_EQ(rtScene.GetTrackedResourceBudget(), 1u);

    ASSERT_TRUE(rtScene.Prepare(planA));
    EXPECT_EQ(rtScene.GetStats().createdBLASCount, 1u);
    EXPECT_EQ(rtScene.GetStats().evictedBLASCount, 0u);
    EXPECT_EQ(rtScene.GetStats().resourceBudgetEvictedBLASCount, 0u);
    EXPECT_FALSE(rtScene.GetStats().resourceBudgetEvictionAttempted);
    EXPECT_TRUE(rtScene.GetStats().resourceBudgetExceeded);
    EXPECT_EQ(rtScene.GetStats().trackedResourceBudget, 1u);
    EXPECT_EQ(rtScene.GetStats().cachedBLASCount, 1u);

    ASSERT_TRUE(rtScene.Prepare(planB));
    EXPECT_EQ(rtScene.GetStats().createdBLASCount, 1u);
    EXPECT_EQ(rtScene.GetStats().reusedBLASCount, 0u);
    EXPECT_EQ(rtScene.GetStats().evictedBLASCount, 1u);
    EXPECT_EQ(rtScene.GetStats().resourceBudgetEvictedBLASCount, 1u);
    EXPECT_TRUE(rtScene.GetStats().resourceBudgetEvictionAttempted);
    EXPECT_TRUE(rtScene.GetStats().resourceBudgetExceeded);
    EXPECT_EQ(rtScene.GetStats().trackedResourceBudget, 1u);
    EXPECT_EQ(rtScene.GetStats().cachedBLASCount, 1u);
    EXPECT_EQ(rtScene.GetStats().cachedBLASAccelerationStructureBytes, 4096u);
    EXPECT_EQ(rtScene.GetStats().cachedBLASScratchBytes, 1024u);
    EXPECT_EQ(rtScene.GetCachedBLASCount(), 1u);

    FakeCommandContext buildB;
    rtScene.RecordBuildCommands(buildB);
    EXPECT_EQ(buildB.buildBottomLevelASCount, 1u);
    EXPECT_EQ(buildB.buildTopLevelASCount, 1u);

    rtScene.Shutdown();
    gpuResources.Shutdown();
}

TEST(GPUResourceManagerValidation, RayTracingSceneManagerPrunesCacheAfterNewBLASExceedsTrackedResourceBudget)
{
    FakeDevice device;
    device.capabilities.supportsRaytracing = true;

    GPUResourceManager gpuResources;
    gpuResources.Initialize(&device);

    auto meshA = CreateMeshResource(135, MeshFactory::CreateTriangle());
    auto meshB = CreateMeshResource(136, MeshFactory::CreateTriangle());
    gpuResources.UploadImmediate(meshA.get());
    gpuResources.UploadImmediate(meshB.get());
    ASSERT_TRUE(gpuResources.GetMeshBuffers(meshA->GetId()).IsValid());
    ASSERT_TRUE(gpuResources.GetMeshBuffers(meshB->GetId()).IsValid());

    RenderScene sceneA;
    RenderObject objectA;
    objectA.meshId = meshA->GetId();
    objectA.meshResource = meshA.get();
    objectA.entityId = 58;
    sceneA.AddObject(objectA);

    RenderScene sceneB;
    RenderObject objectB;
    objectB.meshId = meshB->GetId();
    objectB.meshResource = meshB.get();
    objectB.entityId = 59;
    sceneB.AddObject(objectB);

    const std::array<uint32_t, 1> visibleObjects = {0};
    RayTracingSceneBuildPlan planA = BuildRayTracingSceneBuildPlan(sceneA, visibleObjects, gpuResources);
    RayTracingSceneBuildPlan planB = BuildRayTracingSceneBuildPlan(sceneB, visibleObjects, gpuResources);
    ASSERT_TRUE(planA.HasWork());
    ASSERT_TRUE(planB.HasWork());
    ASSERT_EQ(planA.blasBuilds.size(), 1u);
    ASSERT_EQ(planB.blasBuilds.size(), 1u);
    ASSERT_NE(planA.blasBuilds[0].key.meshId, planB.blasBuilds[0].key.meshId);

    RayTracingSceneManager rtScene;
    rtScene.Initialize(&device);
    rtScene.SetTrackedResourceBudget(12320u);

    ASSERT_TRUE(rtScene.Prepare(planA));
    EXPECT_EQ(rtScene.GetStats().createdBLASCount, 1u);
    EXPECT_EQ(rtScene.GetStats().resourceBudgetEvictedBLASCount, 0u);
    EXPECT_FALSE(rtScene.GetStats().resourceBudgetEvictionAttempted);
    EXPECT_FALSE(rtScene.GetStats().resourceBudgetExceeded);
    EXPECT_EQ(rtScene.GetStats().trackedResourceBudget, 12320u);
    EXPECT_EQ(rtScene.GetStats().totalTrackedResourceBytes, 12320u);
    EXPECT_EQ(rtScene.GetStats().cachedBLASCount, 1u);

    ASSERT_TRUE(rtScene.Prepare(planB));
    EXPECT_EQ(rtScene.GetStats().createdBLASCount, 1u);
    EXPECT_EQ(rtScene.GetStats().reusedBLASCount, 0u);
    EXPECT_EQ(rtScene.GetStats().evictedBLASCount, 1u);
    EXPECT_EQ(rtScene.GetStats().resourceBudgetEvictedBLASCount, 1u);
    EXPECT_TRUE(rtScene.GetStats().resourceBudgetEvictionAttempted);
    EXPECT_FALSE(rtScene.GetStats().resourceBudgetExceeded);
    EXPECT_EQ(rtScene.GetStats().trackedResourceBudget, 12320u);
    EXPECT_EQ(rtScene.GetStats().totalTrackedResourceBytes, 12320u);
    EXPECT_EQ(rtScene.GetStats().cachedBLASCount, 1u);
    EXPECT_EQ(rtScene.GetStats().cachedBLASAccelerationStructureBytes, 4096u);
    EXPECT_EQ(rtScene.GetStats().cachedBLASScratchBytes, 1024u);
    EXPECT_EQ(rtScene.GetCachedBLASCount(), 1u);

    FakeCommandContext buildB;
    rtScene.RecordBuildCommands(buildB);
    EXPECT_EQ(buildB.buildBottomLevelASCount, 1u);
    EXPECT_EQ(buildB.buildTopLevelASCount, 1u);

    rtScene.Shutdown();
    gpuResources.Shutdown();
}

TEST(GPUResourceManagerValidation, RayTracingSceneManagerPrunesCacheAfterTLASGrowthExceedsTrackedResourceBudget)
{
    FakeDevice device;
    device.capabilities.supportsRaytracing = true;

    GPUResourceManager gpuResources;
    gpuResources.Initialize(&device);

    auto meshA = CreateMeshResource(137, MeshFactory::CreateTriangle());
    auto meshB = CreateMeshResource(138, MeshFactory::CreateTriangle());
    gpuResources.UploadImmediate(meshA.get());
    gpuResources.UploadImmediate(meshB.get());
    ASSERT_TRUE(gpuResources.GetMeshBuffers(meshA->GetId()).IsValid());
    ASSERT_TRUE(gpuResources.GetMeshBuffers(meshB->GetId()).IsValid());

    RenderScene sceneA;
    RenderObject objectA;
    objectA.meshId = meshA->GetId();
    objectA.meshResource = meshA.get();
    objectA.entityId = 60;
    sceneA.AddObject(objectA);

    RenderScene sceneBOne;
    RenderObject objectB;
    objectB.meshId = meshB->GetId();
    objectB.meshResource = meshB.get();
    objectB.entityId = 61;
    sceneBOne.AddObject(objectB);

    RenderScene sceneBFour;
    for (uint32 i = 0; i < 4; ++i)
    {
        RenderObject repeatedObject = objectB;
        repeatedObject.entityId = 62 + i;
        sceneBFour.AddObject(repeatedObject);
    }

    const std::array<uint32_t, 1> oneVisibleObject = {0};
    const std::array<uint32_t, 4> fourVisibleObjects = {0, 1, 2, 3};
    RayTracingSceneBuildPlan planA = BuildRayTracingSceneBuildPlan(sceneA, oneVisibleObject, gpuResources);
    RayTracingSceneBuildPlan planBOne = BuildRayTracingSceneBuildPlan(sceneBOne, oneVisibleObject, gpuResources);
    RayTracingSceneBuildPlan planBFour = BuildRayTracingSceneBuildPlan(sceneBFour, fourVisibleObjects, gpuResources);
    ASSERT_TRUE(planA.HasWork());
    ASSERT_TRUE(planBOne.HasWork());
    ASSERT_TRUE(planBFour.HasWork());
    ASSERT_EQ(planA.blasBuilds.size(), 1u);
    ASSERT_EQ(planBOne.blasBuilds.size(), 1u);
    ASSERT_EQ(planBFour.blasBuilds.size(), 1u);
    ASSERT_EQ(planBFour.instances.size(), 4u);
    ASSERT_NE(planA.blasBuilds[0].key.meshId, planBOne.blasBuilds[0].key.meshId);
    EXPECT_EQ(planBOne.blasBuilds[0].key.meshId, planBFour.blasBuilds[0].key.meshId);

    RayTracingSceneManager rtScene;
    rtScene.Initialize(&device);

    ASSERT_TRUE(rtScene.Prepare(planA));
    FakeCommandContext buildA;
    rtScene.RecordBuildCommands(buildA);
    EXPECT_EQ(buildA.buildBottomLevelASCount, 1u);

    ASSERT_TRUE(rtScene.Prepare(planBOne));
    FakeCommandContext buildB;
    rtScene.RecordBuildCommands(buildB);
    EXPECT_EQ(buildB.buildBottomLevelASCount, 1u);

    ASSERT_TRUE(rtScene.Prepare(planBOne));
    ASSERT_EQ(rtScene.GetStats().cachedBLASCount, 2u);
    ASSERT_EQ(rtScene.GetStats().cachedBLASScratchBytes, 0u);
    ASSERT_EQ(rtScene.GetStats().instanceCount, 1u);
    const uint64 oneInstanceTrackedBytes = rtScene.GetStats().totalTrackedResourceBytes;
    const uint64 oneInstanceMaterialBytes = rtScene.GetStats().materialMetadataBufferBytes;
    const uint64 oneInstanceAlphaBytes = rtScene.GetStats().alphaMetadataBufferBytes;
    EXPECT_GT(oneInstanceTrackedBytes, 0u);
    EXPECT_GT(oneInstanceMaterialBytes, 0u);
    EXPECT_GT(oneInstanceAlphaBytes, 0u);

    rtScene.SetTrackedResourceBudget(oneInstanceTrackedBytes + 1024u);
    ASSERT_TRUE(rtScene.Prepare(planBFour));
    EXPECT_EQ(rtScene.GetStats().createdBLASCount, 0u);
    EXPECT_EQ(rtScene.GetStats().reusedBLASCount, 1u);
    EXPECT_EQ(rtScene.GetStats().evictedBLASCount, 1u);
    EXPECT_EQ(rtScene.GetStats().resourceBudgetEvictedBLASCount, 1u);
    EXPECT_TRUE(rtScene.GetStats().resourceBudgetEvictionAttempted);
    EXPECT_FALSE(rtScene.GetStats().resourceBudgetExceeded);
    EXPECT_EQ(rtScene.GetStats().cachedBLASCount, 1u);
    EXPECT_EQ(rtScene.GetStats().cachedBLASAccelerationStructureBytes, 4096u);
    EXPECT_EQ(rtScene.GetStats().cachedBLASScratchBytes, 0u);
    EXPECT_EQ(rtScene.GetStats().instanceCount, 4u);
    EXPECT_GT(rtScene.GetStats().materialMetadataBufferBytes, oneInstanceMaterialBytes);
    EXPECT_GT(rtScene.GetStats().alphaMetadataBufferBytes, oneInstanceAlphaBytes);
    EXPECT_LE(rtScene.GetStats().totalTrackedResourceBytes, rtScene.GetStats().trackedResourceBudget);

    FakeCommandContext reuseB;
    rtScene.RecordBuildCommands(reuseB);
    EXPECT_EQ(reuseB.buildBottomLevelASCount, 0u);
    EXPECT_EQ(reuseB.buildTopLevelASCount, 1u);

    rtScene.Shutdown();
    gpuResources.Shutdown();
}
TEST(GPUResourceManagerValidation, RayTracingSceneManagerResourceBudgetEvictsHugeBLASWithoutByteWrap)
{
    FakeDevice device;
    device.capabilities.supportsRaytracing = true;

    const uint64 hugeAlignedASSize =
        std::numeric_limits<uint64>::max() -
        (std::numeric_limits<uint64>::max() % RVX_RAY_TRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT);
    RHIAccelerationStructureBuildSizes hugeBLASSize;
    hugeBLASSize.accelerationStructureSize = hugeAlignedASSize;
    hugeBLASSize.buildScratchSize = 1024u;
    hugeBLASSize.updateScratchSize = 512u;
    RHIAccelerationStructureBuildSizes normalBLASSize;
    normalBLASSize.accelerationStructureSize = 4096u;
    normalBLASSize.buildScratchSize = 1024u;
    normalBLASSize.updateScratchSize = 512u;
    device.bottomLevelBuildSizeOverrides = {hugeBLASSize, normalBLASSize, normalBLASSize};

    GPUResourceManager gpuResources;
    gpuResources.Initialize(&device);

    auto meshA = CreateMeshResource(132, MeshFactory::CreateTriangle());
    auto meshB = CreateMeshResource(133, MeshFactory::CreateTriangle());
    auto meshC = CreateMeshResource(134, MeshFactory::CreateTriangle());
    gpuResources.UploadImmediate(meshA.get());
    gpuResources.UploadImmediate(meshB.get());
    gpuResources.UploadImmediate(meshC.get());
    ASSERT_TRUE(gpuResources.GetMeshBuffers(meshA->GetId()).IsValid());
    ASSERT_TRUE(gpuResources.GetMeshBuffers(meshB->GetId()).IsValid());
    ASSERT_TRUE(gpuResources.GetMeshBuffers(meshC->GetId()).IsValid());

    RenderScene sceneAB;
    RenderObject objectA;
    objectA.meshId = meshA->GetId();
    objectA.meshResource = meshA.get();
    objectA.entityId = 55;
    sceneAB.AddObject(objectA);
    RenderObject objectB;
    objectB.meshId = meshB->GetId();
    objectB.meshResource = meshB.get();
    objectB.entityId = 56;
    sceneAB.AddObject(objectB);

    RenderScene sceneC;
    RenderObject objectC;
    objectC.meshId = meshC->GetId();
    objectC.meshResource = meshC.get();
    objectC.entityId = 57;
    sceneC.AddObject(objectC);

    const std::array<uint32_t, 2> visibleAB = {0, 1};
    const std::array<uint32_t, 1> visibleC = {0};
    RayTracingSceneBuildPlan planAB = BuildRayTracingSceneBuildPlan(sceneAB, visibleAB, gpuResources);
    RayTracingSceneBuildPlan planC = BuildRayTracingSceneBuildPlan(sceneC, visibleC, gpuResources);
    ASSERT_TRUE(planAB.HasWork());
    ASSERT_TRUE(planC.HasWork());
    ASSERT_EQ(planAB.blasBuilds.size(), 2u);
    ASSERT_EQ(planC.blasBuilds.size(), 1u);

    RayTracingSceneManager rtScene;
    rtScene.Initialize(&device);
    rtScene.SetTrackedResourceBudget(std::numeric_limits<uint64>::max());

    ASSERT_TRUE(rtScene.Prepare(planAB));
    EXPECT_TRUE(rtScene.GetStats().resourceByteAccountingOverflowed);
    EXPECT_TRUE(rtScene.GetStats().resourceBudgetExceeded);
    EXPECT_EQ(rtScene.GetStats().cachedBLASCount, 2u);

    ASSERT_TRUE(rtScene.Prepare(planC));
    EXPECT_EQ(rtScene.GetStats().resourceBudgetEvictedBLASCount, 1u);
    EXPECT_EQ(rtScene.GetStats().evictedBLASCount, 1u);
    EXPECT_TRUE(rtScene.GetStats().resourceBudgetEvictionAttempted);
    EXPECT_EQ(rtScene.GetStats().cachedBLASCount, 2u);
    EXPECT_TRUE(rtScene.GetStats().resourceByteAccountingOverflowed);
    EXPECT_FALSE(rtScene.GetStats().resourceBudgetExceeded);
    EXPECT_LT(rtScene.GetStats().totalTrackedResourceBytes, rtScene.GetStats().trackedResourceBudget);
    EXPECT_EQ(rtScene.GetStats().trackedResourceBudget, std::numeric_limits<uint64>::max());

    rtScene.Shutdown();
    gpuResources.Shutdown();
}

TEST(GPUResourceManagerValidation, RayTracingSceneManagerFlagsTrackedResourceByteOverflowAsBudgetExceeded)
{
    FakeDevice device;
    device.capabilities.supportsRaytracing = true;
    device.overrideTopLevelBuildSizes = true;
    device.topLevelBuildSizesOverride.accelerationStructureSize =
        std::numeric_limits<uint64>::max() -
        (std::numeric_limits<uint64>::max() % RVX_RAY_TRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT);
    device.topLevelBuildSizesOverride.buildScratchSize = 2048u;
    device.topLevelBuildSizesOverride.updateScratchSize = 1024u;

    GPUResourceManager gpuResources;
    gpuResources.Initialize(&device);

    auto mesh = CreateMeshResource(131, MeshFactory::CreateTriangle());
    gpuResources.UploadImmediate(mesh.get());
    ASSERT_TRUE(gpuResources.GetMeshBuffers(mesh->GetId()).IsValid());

    RenderScene scene;
    RenderObject object;
    object.meshId = mesh->GetId();
    object.meshResource = mesh.get();
    object.entityId = 54;
    scene.AddObject(object);

    const std::array<uint32_t, 1> visibleObjects = {0};
    RayTracingSceneBuildPlan plan = BuildRayTracingSceneBuildPlan(scene, visibleObjects, gpuResources);
    ASSERT_TRUE(plan.HasWork());

    RayTracingSceneManager rtScene;
    rtScene.Initialize(&device);
    rtScene.SetTrackedResourceBudget(std::numeric_limits<uint64>::max());

    ASSERT_TRUE(rtScene.Prepare(plan));
    EXPECT_TRUE(rtScene.GetStats().resourceByteAccountingOverflowed);
    EXPECT_TRUE(rtScene.GetStats().resourceBudgetExceeded);
    EXPECT_EQ(rtScene.GetStats().totalTrackedResourceBytes, std::numeric_limits<uint64>::max());
    EXPECT_EQ(rtScene.GetStats().trackedResourceBudget, std::numeric_limits<uint64>::max());

    rtScene.Shutdown();
    gpuResources.Shutdown();
}

TEST(GPUResourceManagerValidation, RayTracingSceneManagerClearsFrameOutputsAfterPrepareFallback)
{
    FakeDevice device;
    device.capabilities.supportsRaytracing = true;

    GPUResourceManager gpuResources;
    gpuResources.Initialize(&device);

    auto mesh = CreateMeshResource(123, MeshFactory::CreateTriangle());
    gpuResources.UploadImmediate(mesh.get());
    ASSERT_TRUE(gpuResources.GetMeshBuffers(mesh->GetId()).IsValid());

    RenderScene scene;
    RenderObject object;
    object.meshId = mesh->GetId();
    object.meshResource = mesh.get();
    object.entityId = 40;
    scene.AddObject(object);

    const std::array<uint32_t, 1> visibleObjects = {0};
    RayTracingSceneBuildPlan plan = BuildRayTracingSceneBuildPlan(scene, visibleObjects, gpuResources);
    ASSERT_TRUE(plan.HasWork());
    ASSERT_EQ(plan.blasBuilds.size(), 1u);
    ASSERT_EQ(plan.instances.size(), 1u);

    RayTracingSceneManager rtScene;
    rtScene.Initialize(&device);

    ASSERT_TRUE(rtScene.Prepare(plan));
    EXPECT_TRUE(rtScene.GetStats().prepared);
    EXPECT_NE(rtScene.GetTopLevelAS(), nullptr);
    EXPECT_NE(rtScene.GetInstanceBuffer(), nullptr);
    EXPECT_NE(rtScene.GetInstanceMaterialMetadataBuffer(), nullptr);
    EXPECT_NE(rtScene.GetInstanceAlphaMetadataBuffer(), nullptr);
    EXPECT_NE(rtScene.GetTopLevelScratchBuffer(), nullptr);
    EXPECT_EQ(rtScene.GetCachedBLASCount(), 1u);
    const uint64 cachedBLASAccelerationStructureBytes =
        rtScene.GetStats().cachedBLASAccelerationStructureBytes;
    const uint64 cachedBLASScratchBytes = rtScene.GetStats().cachedBLASScratchBytes;
    EXPECT_GT(cachedBLASAccelerationStructureBytes, 0u);
    EXPECT_GT(cachedBLASScratchBytes, 0u);
    EXPECT_GT(rtScene.GetStats().topLevelAccelerationStructureBytes, 0u);
    EXPECT_GT(rtScene.GetStats().topLevelScratchBytes, 0u);
    EXPECT_GT(rtScene.GetStats().instanceBufferBytes, 0u);
    EXPECT_GT(rtScene.GetStats().materialMetadataBufferBytes, 0u);
    EXPECT_GT(rtScene.GetStats().alphaMetadataBufferBytes, 0u);
    EXPECT_GT(rtScene.GetStats().totalTrackedResourceBytes,
              cachedBLASAccelerationStructureBytes + cachedBLASScratchBytes);

    RayTracingSceneBuildPlan emptyPlan;
    EXPECT_FALSE(rtScene.Prepare(emptyPlan));

    EXPECT_FALSE(rtScene.GetStats().prepared);
    EXPECT_FALSE(rtScene.GetStats().hasTopLevelAS);
    EXPECT_FALSE(rtScene.GetStats().hasInstanceBuffer);
    EXPECT_FALSE(rtScene.GetStats().hasMaterialMetadataBuffer);
    EXPECT_FALSE(rtScene.GetStats().hasAlphaMetadataBuffer);
    EXPECT_EQ(rtScene.GetStats().pendingBLASBuildCount, 0u);
    EXPECT_EQ(rtScene.GetStats().materialTextureCount, 0u);
    EXPECT_EQ(rtScene.GetStats().alphaTextureCount, 0u);
    EXPECT_EQ(rtScene.GetStats().cachedBLASCount, 1u);
    EXPECT_EQ(rtScene.GetStats().cachedBLASAccelerationStructureBytes,
              cachedBLASAccelerationStructureBytes);
    EXPECT_EQ(rtScene.GetStats().cachedBLASScratchBytes, cachedBLASScratchBytes);
    EXPECT_EQ(rtScene.GetStats().topLevelAccelerationStructureBytes, 0u);
    EXPECT_EQ(rtScene.GetStats().topLevelScratchBytes, 0u);
    EXPECT_EQ(rtScene.GetStats().instanceBufferBytes, 0u);
    EXPECT_EQ(rtScene.GetStats().materialMetadataBufferBytes, 0u);
    EXPECT_EQ(rtScene.GetStats().alphaMetadataBufferBytes, 0u);
    EXPECT_EQ(rtScene.GetStats().totalTrackedResourceBytes,
              cachedBLASAccelerationStructureBytes + cachedBLASScratchBytes);
    EXPECT_STREQ(rtScene.GetStats().fallbackReason, "ray tracing scene plan has no buildable work");
    EXPECT_EQ(rtScene.GetTopLevelAS(), nullptr);
    EXPECT_EQ(rtScene.GetInstanceBuffer(), nullptr);
    EXPECT_EQ(rtScene.GetInstanceMaterialMetadataBuffer(), nullptr);
    EXPECT_EQ(rtScene.GetInstanceAlphaMetadataBuffer(), nullptr);
    EXPECT_EQ(rtScene.GetTopLevelScratchBuffer(), nullptr);
    EXPECT_EQ(rtScene.GetTopLevelBuildDesc().instanceBuffer, nullptr);
    EXPECT_EQ(rtScene.GetTopLevelBuildDesc().instanceCount, 0u);
    EXPECT_TRUE(rtScene.GetTopLevelBuildDesc().instances.empty());
    EXPECT_TRUE(rtScene.GetInstanceMaterialTextureTable().empty());
    EXPECT_TRUE(rtScene.GetInstanceAlphaTextureTable().empty());
    EXPECT_TRUE(rtScene.GetInstanceAlphaIndexBufferTable().empty());
    EXPECT_TRUE(rtScene.GetInstanceAlphaUVBufferTable().empty());
    EXPECT_EQ(rtScene.GetCachedBLASCount(), 1u);

    FakeCommandContext ctx;
    rtScene.RecordBuildCommands(ctx);
    EXPECT_EQ(ctx.buildBottomLevelASCount, 0u);
    EXPECT_EQ(ctx.buildTopLevelASCount, 0u);
    EXPECT_EQ(rtScene.GetStats().recordedBLASBuildCount, 0u);
    EXPECT_FALSE(rtScene.GetStats().recordedTLASBuild);

    rtScene.Shutdown();
    gpuResources.Shutdown();
}

TEST(GPUResourceManagerValidation, RayTracingSceneManagerRejectsNonDenseInstanceIDs)
{
    FakeDevice device;
    device.capabilities.supportsRaytracing = true;

    GPUResourceManager gpuResources;
    gpuResources.Initialize(&device);

    auto mesh = CreateMeshResource(125, MeshFactory::CreateTriangle());
    gpuResources.UploadImmediate(mesh.get());
    ASSERT_TRUE(gpuResources.GetMeshBuffers(mesh->GetId()).IsValid());

    RenderScene scene;
    RenderObject object;
    object.meshId = mesh->GetId();
    object.meshResource = mesh.get();
    object.entityId = 42;
    scene.AddObject(object);

    const std::array<uint32_t, 1> visibleObjects = {0};
    RayTracingSceneBuildPlan plan = BuildRayTracingSceneBuildPlan(scene, visibleObjects, gpuResources);
    ASSERT_TRUE(plan.HasWork());
    ASSERT_EQ(plan.instances.size(), 1u);
    plan.instances[0].desc.instanceId = 7u;

    RayTracingSceneManager rtScene;
    rtScene.Initialize(&device);

    EXPECT_FALSE(rtScene.Prepare(plan));
    EXPECT_FALSE(rtScene.GetStats().prepared);
    EXPECT_STREQ(rtScene.GetStats().fallbackReason, "ray tracing TLAS instance IDs must match metadata order");
    EXPECT_EQ(rtScene.GetTopLevelAS(), nullptr);
    EXPECT_EQ(rtScene.GetInstanceBuffer(), nullptr);
    EXPECT_EQ(rtScene.GetInstanceMaterialMetadataBuffer(), nullptr);
    EXPECT_EQ(rtScene.GetInstanceAlphaMetadataBuffer(), nullptr);
    EXPECT_EQ(rtScene.GetStats().pendingBLASBuildCount, 0u);
    EXPECT_EQ(rtScene.GetCachedBLASCount(), 1u);

    rtScene.Shutdown();
    gpuResources.Shutdown();
}

TEST(GPUResourceManagerValidation, RayTracingSceneManagerRejectsMissingTLASInstanceMapping)
{
    FakeDevice device;
    device.capabilities.supportsRaytracing = true;

    GPUResourceManager gpuResources;
    gpuResources.Initialize(&device);

    auto mesh = CreateMeshResource(126, MeshFactory::CreateTriangle());
    gpuResources.UploadImmediate(mesh.get());
    ASSERT_TRUE(gpuResources.GetMeshBuffers(mesh->GetId()).IsValid());

    RenderScene scene;
    RenderObject object;
    object.meshId = mesh->GetId();
    object.meshResource = mesh.get();
    object.entityId = 43;
    scene.AddObject(object);

    const std::array<uint32_t, 1> visibleObjects = {0};
    RayTracingSceneBuildPlan plan = BuildRayTracingSceneBuildPlan(scene, visibleObjects, gpuResources);
    ASSERT_TRUE(plan.HasWork());
    ASSERT_EQ(plan.blasBuilds.size(), 1u);
    ASSERT_EQ(plan.instances.size(), 1u);
    plan.instances[0].blasIndex = static_cast<uint32>(plan.blasBuilds.size());

    RayTracingSceneManager rtScene;
    rtScene.Initialize(&device);

    EXPECT_FALSE(rtScene.Prepare(plan));
    EXPECT_FALSE(rtScene.GetStats().prepared);
    EXPECT_STREQ(rtScene.GetStats().fallbackReason, "failed to map all ray tracing TLAS instances");
    EXPECT_EQ(rtScene.GetTopLevelAS(), nullptr);
    EXPECT_EQ(rtScene.GetInstanceBuffer(), nullptr);
    EXPECT_EQ(rtScene.GetInstanceMaterialMetadataBuffer(), nullptr);
    EXPECT_EQ(rtScene.GetInstanceAlphaMetadataBuffer(), nullptr);
    EXPECT_EQ(rtScene.GetStats().pendingBLASBuildCount, 0u);
    EXPECT_EQ(rtScene.GetCachedBLASCount(), 1u);

    rtScene.Shutdown();
    gpuResources.Shutdown();
}

TEST(GPUResourceManagerValidation, RayTracingSceneManagerRetriesIncompleteBLASCacheEntryAfterCreationFailure)
{
    FakeDevice device;
    device.capabilities.supportsRaytracing = true;

    GPUResourceManager gpuResources;
    gpuResources.Initialize(&device);

    auto mesh = CreateMeshResource(124, MeshFactory::CreateTriangle());
    gpuResources.UploadImmediate(mesh.get());
    ASSERT_TRUE(gpuResources.GetMeshBuffers(mesh->GetId()).IsValid());

    RenderScene scene;
    RenderObject object;
    object.meshId = mesh->GetId();
    object.meshResource = mesh.get();
    object.entityId = 41;
    scene.AddObject(object);

    const std::array<uint32_t, 1> visibleObjects = {0};
    RayTracingSceneBuildPlan plan = BuildRayTracingSceneBuildPlan(scene, visibleObjects, gpuResources);
    ASSERT_TRUE(plan.HasWork());

    RayTracingSceneManager rtScene;
    rtScene.Initialize(&device);

    device.failAccelerationStructureCreation = true;
    EXPECT_FALSE(rtScene.Prepare(plan));
    EXPECT_FALSE(rtScene.GetStats().prepared);
    EXPECT_EQ(rtScene.GetStats().cachedBLASCount, 1u);
    EXPECT_EQ(rtScene.GetCachedBLASCount(), 1u);
    EXPECT_EQ(rtScene.GetTopLevelAS(), nullptr);
    EXPECT_EQ(rtScene.GetInstanceBuffer(), nullptr);
    EXPECT_STREQ(rtScene.GetStats().fallbackReason, "failed to create bottom-level acceleration structure");
    EXPECT_EQ(device.createdAccelerationStructureCount, 1u);

    device.failAccelerationStructureCreation = false;
    ASSERT_TRUE(rtScene.Prepare(plan));
    EXPECT_TRUE(rtScene.GetStats().prepared);
    EXPECT_TRUE(rtScene.GetStats().hasTopLevelAS);
    EXPECT_EQ(rtScene.GetStats().cachedBLASCount, 1u);
    EXPECT_EQ(rtScene.GetStats().createdBLASCount, 1u);
    EXPECT_EQ(rtScene.GetStats().reusedBLASCount, 0u);
    EXPECT_EQ(rtScene.GetStats().pendingBLASBuildCount, 1u);
    EXPECT_NE(rtScene.GetTopLevelAS(), nullptr);
    EXPECT_NE(rtScene.GetInstanceBuffer(), nullptr);
    EXPECT_NE(rtScene.GetInstanceMaterialMetadataBuffer(), nullptr);
    EXPECT_NE(rtScene.GetInstanceAlphaMetadataBuffer(), nullptr);
    EXPECT_EQ(device.createdAccelerationStructureCount, 3u);

    FakeCommandContext ctx;
    rtScene.RecordBuildCommands(ctx);
    EXPECT_EQ(ctx.buildBottomLevelASCount, 1u);
    EXPECT_EQ(ctx.buildTopLevelASCount, 1u);
    EXPECT_EQ(ctx.lastTopLevelDesc.instanceBuffer, rtScene.GetInstanceBuffer());
    EXPECT_EQ(ctx.lastTopLevelDesc.instanceCount, 1u);
    EXPECT_EQ(rtScene.GetStats().recordedBLASBuildCount, 1u);
    EXPECT_TRUE(rtScene.GetStats().recordedTLASBuild);
    EXPECT_EQ(rtScene.GetStats().pendingBLASBuildCount, 0u);

    rtScene.Shutdown();
    gpuResources.Shutdown();
}

TEST(GPUResourceManagerValidation, RayTracingSceneManagerRetriesIncompleteBLASCacheEntryAfterScratchCreationFailure)
{
    FakeDevice device;
    device.capabilities.supportsRaytracing = true;

    GPUResourceManager gpuResources;
    gpuResources.Initialize(&device);

    auto mesh = CreateMeshResource(132, MeshFactory::CreateTriangle());
    gpuResources.UploadImmediate(mesh.get());
    ASSERT_TRUE(gpuResources.GetMeshBuffers(mesh->GetId()).IsValid());

    RenderScene scene;
    RenderObject object;
    object.meshId = mesh->GetId();
    object.meshResource = mesh.get();
    object.entityId = 55;
    scene.AddObject(object);

    const std::array<uint32_t, 1> visibleObjects = {0};
    RayTracingSceneBuildPlan plan = BuildRayTracingSceneBuildPlan(scene, visibleObjects, gpuResources);
    ASSERT_TRUE(plan.HasWork());

    RayTracingSceneManager rtScene;
    rtScene.Initialize(&device);

    device.failBufferCreation = true;
    EXPECT_FALSE(rtScene.Prepare(plan));
    EXPECT_FALSE(rtScene.GetStats().prepared);
    EXPECT_EQ(rtScene.GetStats().cachedBLASCount, 1u);
    EXPECT_EQ(rtScene.GetCachedBLASCount(), 1u);
    EXPECT_EQ(rtScene.GetStats().cachedBLASAccelerationStructureBytes, 4096u);
    EXPECT_EQ(rtScene.GetStats().cachedBLASScratchBytes, 0u);
    EXPECT_EQ(rtScene.GetStats().pendingBLASBuildCount, 0u);
    EXPECT_STREQ(rtScene.GetStats().fallbackReason, "failed to create bottom-level acceleration structure");
    EXPECT_EQ(device.createdAccelerationStructureCount, 1u);

    device.failBufferCreation = false;
    ASSERT_TRUE(rtScene.Prepare(plan));
    EXPECT_TRUE(rtScene.GetStats().prepared);
    EXPECT_TRUE(rtScene.GetStats().hasTopLevelAS);
    EXPECT_EQ(rtScene.GetStats().cachedBLASCount, 1u);
    EXPECT_EQ(rtScene.GetStats().createdBLASCount, 0u);
    EXPECT_EQ(rtScene.GetStats().reusedBLASCount, 0u);
    EXPECT_EQ(rtScene.GetStats().pendingBLASBuildCount, 1u);
    EXPECT_EQ(rtScene.GetStats().cachedBLASAccelerationStructureBytes, 4096u);
    EXPECT_EQ(rtScene.GetStats().cachedBLASScratchBytes, 1024u);
    EXPECT_EQ(device.createdAccelerationStructureCount, 2u);

    FakeCommandContext ctx;
    rtScene.RecordBuildCommands(ctx);
    EXPECT_EQ(ctx.buildBottomLevelASCount, 1u);
    EXPECT_EQ(ctx.buildTopLevelASCount, 1u);
    EXPECT_EQ(rtScene.GetStats().recordedBLASBuildCount, 1u);
    EXPECT_TRUE(rtScene.GetStats().recordedTLASBuild);
    EXPECT_EQ(rtScene.GetStats().pendingBLASBuildCount, 0u);

    rtScene.Shutdown();
    gpuResources.Shutdown();
}

TEST(GPUResourceManagerValidation, MeshBuffersExposeAttributeAvailability)
{
    FakeDevice device;
    GPUResourceManager manager;
    manager.Initialize(&device);

    auto tangentMesh = MeshFactory::CreateTriangle();
    ASSERT_TRUE(tangentMesh->GenerateTangents());
    auto mesh = CreateMeshResource(110, tangentMesh);

    manager.UploadImmediate(mesh.get());

    const auto buffers = manager.GetMeshBuffers(mesh->GetId());
    EXPECT_TRUE(buffers.IsValid());
    EXPECT_TRUE(buffers.hasNormals);
    EXPECT_TRUE(buffers.hasUVs);
    EXPECT_TRUE(buffers.hasTangents);
    EXPECT_TRUE(buffers.HasNormalMapTangentBasis());

    auto skinnedMesh = MeshFactory::CreateTriangle();
    skinnedMesh->SetBoneData({IVec4(0, 0, 0, 0),
                              IVec4(1, 0, 0, 0),
                              IVec4(1, 0, 0, 0)},
                             {Vec4(1.0f, 0.0f, 0.0f, 0.0f),
                              Vec4(1.0f, 0.0f, 0.0f, 0.0f),
                              Vec4(1.0f, 0.0f, 0.0f, 0.0f)});
    auto skinnedResource = CreateMeshResource(210, skinnedMesh);

    manager.UploadImmediate(skinnedResource.get());

    const auto skinnedBuffers = manager.GetMeshBuffers(skinnedResource->GetId());
    EXPECT_TRUE(skinnedBuffers.IsValid());
    ASSERT_NE(skinnedBuffers.boneIndicesBuffer, nullptr);
    ASSERT_NE(skinnedBuffers.boneWeightsBuffer, nullptr);
    EXPECT_TRUE(skinnedBuffers.hasBoneIndices);
    EXPECT_TRUE(skinnedBuffers.hasBoneWeights);
    EXPECT_TRUE(skinnedBuffers.HasSkinningVertexData());
    EXPECT_EQ(skinnedBuffers.boneIndicesBuffer->GetStride(), sizeof(IVec4));
    EXPECT_EQ(skinnedBuffers.boneWeightsBuffer->GetStride(), sizeof(Vec4));

    auto positionOnly = CreatePositionOnlyMesh();
    positionOnly->SetIndices(std::vector<uint32_t>{0, 1, 2});
    auto positionOnlyResource = CreateMeshResource(111, positionOnly);

    manager.UploadImmediate(positionOnlyResource.get());

    const auto positionOnlyBuffers = manager.GetMeshBuffers(positionOnlyResource->GetId());
    EXPECT_TRUE(positionOnlyBuffers.IsValid());
    EXPECT_FALSE(positionOnlyBuffers.hasNormals);
    EXPECT_FALSE(positionOnlyBuffers.hasUVs);
    EXPECT_FALSE(positionOnlyBuffers.hasTangents);
    EXPECT_FALSE(positionOnlyBuffers.hasBoneIndices);
    EXPECT_FALSE(positionOnlyBuffers.hasBoneWeights);
    EXPECT_FALSE(positionOnlyBuffers.HasNormalMapTangentBasis());
    EXPECT_FALSE(positionOnlyBuffers.HasSkinningVertexData());

    manager.Shutdown();
}

TEST(GPUResourceManagerValidation, StagedMeshUploadImmediateWaitsForFenceCompletion)
{
    FakeDevice device;
    device.supportStagedCopy = true;

    GPUResourceManager manager;
    manager.Initialize(&device);

    auto mesh = CreateMeshResource(104, MeshFactory::CreateTriangle());

    manager.UploadImmediate(mesh.get());

    EXPECT_EQ(manager.GetResourceState(mesh->GetId()), GPUResourceState::GPUReady);
    EXPECT_TRUE(manager.IsResident(mesh->GetId()));
    EXPECT_TRUE(manager.IsGPUReady(mesh->GetId()));
    EXPECT_TRUE(manager.GetMeshBuffers(mesh->GetId()).IsValid());
    EXPECT_EQ(device.waitIdleCount, 0u);
    {
        const auto stats = manager.GetStats();
        EXPECT_EQ(stats.residentMeshCount, 1ull);
        EXPECT_EQ(stats.uploadingCount, 0ull);
    }

    manager.Shutdown();
}

TEST(GPUResourceManagerValidation, TransitionTextureTransitionsResidentTextureOnce)
{
    FakeDevice device;
    device.supportStagedCopy = true;
    device.completeSubmittedFenceImmediately = true;

    GPUResourceManager manager;
    manager.Initialize(&device);

    auto texture = CreateTextureResource(105);
    manager.UploadImmediate(texture.get());
    manager.ProcessPendingUploads();

    EXPECT_EQ(manager.GetResourceState(texture->GetId()), GPUResourceState::GPUReady);
    EXPECT_TRUE(manager.IsResident(texture->GetId()));

    FakeCommandContext ctx;
    EXPECT_TRUE(manager.TransitionTexture(texture->GetId(), ctx, RHIResourceState::ShaderResource));
    EXPECT_EQ(ctx.textureBarrierCount, 1u);
    EXPECT_EQ(ctx.lastTextureBarrier.texture, manager.GetTexture(texture->GetId()));
    EXPECT_EQ(ctx.lastTextureBarrier.stateBefore, RHIResourceState::Common);
    EXPECT_EQ(ctx.lastTextureBarrier.stateAfter, RHIResourceState::ShaderResource);

    EXPECT_TRUE(manager.TransitionTexture(texture->GetId(), ctx, RHIResourceState::ShaderResource));
    EXPECT_EQ(ctx.textureBarrierCount, 1u);

    EXPECT_FALSE(manager.TransitionTexture(Resource::InvalidResourceId, ctx, RHIResourceState::ShaderResource));

    manager.Shutdown();
}

TEST(GPUResourceManagerValidation, RGB8TextureUploadExpandsToRGBA8)
{
    FakeDevice device;
    device.supportStagedCopy = true;
    device.completeSubmittedFenceImmediately = true;

    GPUResourceManager manager;
    manager.Initialize(&device);

    auto texture = CreateTextureResource(
        107,
        Resource::TextureFormat::RGB8,
        {255, 0, 0},
        1,
        1);

    manager.UploadImmediate(texture.get());

    RHITexture* gpuTexture = manager.GetTexture(texture->GetId());
    ASSERT_NE(nullptr, gpuTexture);
    EXPECT_EQ(gpuTexture->GetFormat(), RHIFormat::RGBA8_UNORM);
    EXPECT_EQ(manager.GetResourceState(texture->GetId()), GPUResourceState::GPUReady);
    EXPECT_EQ(manager.GetStats().usedMemory, 4ull);

    manager.Shutdown();
}

TEST(GPUResourceManagerValidation, MismatchedTextureDataSizeFailsWithoutCreatingTexture)
{
    FakeDevice device;
    device.supportStagedCopy = true;

    GPUResourceManager manager;
    manager.Initialize(&device);

    auto texture = CreateTextureResource(
        108,
        Resource::TextureFormat::RGBA8,
        {255, 255, 255},
        1,
        1);

    manager.UploadImmediate(texture.get());

    EXPECT_EQ(manager.GetResourceState(texture->GetId()), GPUResourceState::Failed);
    EXPECT_FALSE(manager.IsResident(texture->GetId()));
    EXPECT_EQ(manager.GetTexture(texture->GetId()), nullptr);
    EXPECT_EQ(device.createdTextureCount, 0u);

    manager.Shutdown();
}

TEST(GPUResourceManagerValidation, CubemapTextureUploadsAsSingleRHICube)
{
    FakeDevice device;
    device.supportStagedCopy = true;
    device.completeSubmittedFenceImmediately = true;

    GPUResourceManager manager;
    manager.Initialize(&device);

    Resource::TextureMetadata metadata;
    metadata.width = 1;
    metadata.height = 1;
    metadata.depth = 1;
    metadata.mipLevels = 1;
    metadata.arrayLayers = 6;
    metadata.format = Resource::TextureFormat::RGBA32F;
    metadata.isCubemap = true;
    metadata.isSRGB = false;

    std::vector<uint8> pixels(6 * 16, 42);
    auto texture = CreateTextureResourceWithMetadata(120, metadata, pixels);

    manager.UploadImmediate(texture.get());

    EXPECT_EQ(manager.GetResourceState(texture->GetId()), GPUResourceState::GPUReady);
    EXPECT_TRUE(manager.IsResident(texture->GetId()));
    EXPECT_EQ(device.createdTextureCount, 1u);
    EXPECT_EQ(device.lastCreatedTextureDesc.dimension, RHITextureDimension::TextureCube);
    EXPECT_EQ(device.lastCreatedTextureDesc.arraySize, 1u);
    EXPECT_EQ(device.lastCreatedTextureDesc.mipLevels, 1u);
    EXPECT_EQ(device.lastCreatedTextureDesc.format, RHIFormat::RGBA32_FLOAT);
    ASSERT_NE(nullptr, device.lastCommandContext);
    ASSERT_EQ(device.lastCommandContext->bufferTextureCopyDescs.size(), 6u);
    for (uint32 physicalLayer = 0; physicalLayer < 6; ++physicalLayer)
    {
        EXPECT_EQ(device.lastCommandContext->bufferTextureCopyDescs[physicalLayer].textureSubresource,
                  EncodeTextureSubresource(0, physicalLayer, 1));
    }

    manager.Shutdown();
}

TEST(GPUResourceManagerValidation, TextureArrayMetadataUploadsAsTexture2DArray)
{
    FakeDevice device;
    device.supportStagedCopy = true;
    device.completeSubmittedFenceImmediately = true;

    GPUResourceManager manager;
    manager.Initialize(&device);

    Resource::TextureMetadata metadata;
    metadata.width = 1;
    metadata.height = 1;
    metadata.depth = 1;
    metadata.mipLevels = 1;
    metadata.arrayLayers = 2;
    metadata.format = Resource::TextureFormat::RGBA8;
    metadata.isArray = true;
    metadata.isSRGB = false;

    auto texture = CreateTextureResourceWithMetadata(122, metadata, {1, 2, 3, 4, 5, 6, 7, 8});
    manager.UploadImmediate(texture.get());

    EXPECT_EQ(manager.GetResourceState(texture->GetId()), GPUResourceState::GPUReady);
    EXPECT_EQ(device.lastCreatedTextureDesc.dimension, RHITextureDimension::Texture2D);
    EXPECT_EQ(device.lastCreatedTextureDesc.arraySize, 2u);
    ASSERT_NE(nullptr, device.lastCommandContext);
    ASSERT_EQ(device.lastCommandContext->bufferTextureCopyDescs.size(), 2u);
    EXPECT_EQ(device.lastCommandContext->bufferTextureCopyDescs[0].textureSubresource, 0u);
    EXPECT_EQ(device.lastCommandContext->bufferTextureCopyDescs[1].textureSubresource, 1u);

    manager.Shutdown();
}

TEST(GPUResourceManagerValidation, Ordinary2DMipChainUploadsAllMipSubresources)
{
    FakeDevice device;
    device.supportStagedCopy = true;

    GPUResourceManager manager;
    manager.Initialize(&device);

    Resource::TextureMetadata metadata;
    metadata.width = 2;
    metadata.height = 2;
    metadata.depth = 1;
    metadata.mipLevels = 2;
    metadata.arrayLayers = 1;
    metadata.format = Resource::TextureFormat::RGBA8;
    metadata.isSRGB = false;

    const std::vector<uint8> pixels = {
        10, 11, 12, 13,     20, 21, 22, 23,
        30, 31, 32, 33,     40, 41, 42, 43,
        90, 91, 92, 93,
    };
    auto texture = CreateTextureResourceWithMetadata(123, metadata, pixels);
    manager.UploadImmediate(texture.get());

    ASSERT_EQ(manager.GetResourceState(texture->GetId()), GPUResourceState::GPUReady);
    EXPECT_EQ(device.lastCreatedTextureDesc.dimension, RHITextureDimension::Texture2D);
    EXPECT_EQ(device.lastCreatedTextureDesc.arraySize, 1u);
    EXPECT_EQ(device.lastCreatedTextureDesc.mipLevels, 2u);
    ASSERT_NE(nullptr, device.lastCommandContext);
    ASSERT_NE(nullptr, device.lastStagingBuffer);
    ASSERT_EQ(device.lastCommandContext->bufferTextureCopyDescs.size(), 2u);

    const auto& mip0 = device.lastCommandContext->bufferTextureCopyDescs[0];
    const auto& mip1 = device.lastCommandContext->bufferTextureCopyDescs[1];
    EXPECT_EQ(mip0.textureSubresource, EncodeTextureSubresource(0, 0, 2));
    EXPECT_EQ(mip1.textureSubresource, EncodeTextureSubresource(1, 0, 2));
    EXPECT_EQ(mip0.textureRegion.width, 2u);
    EXPECT_EQ(mip0.textureRegion.height, 2u);
    EXPECT_EQ(mip1.textureRegion.width, 1u);
    EXPECT_EQ(mip1.textureRegion.height, 1u);

    const auto& storage = device.lastStagingBuffer->GetStorage();
    EXPECT_EQ(storage[static_cast<size_t>(mip0.bufferOffset)], 10u);
    EXPECT_EQ(storage[static_cast<size_t>(mip1.bufferOffset)], 90u);

    manager.Shutdown();
}

TEST(GPUResourceManagerValidation, BlockCompressedTextureUploadsAsCompressedRHITexture)
{
    FakeDevice device;
    device.supportStagedCopy = true;
    device.completeSubmittedFenceImmediately = true;

    GPUResourceManager manager;
    manager.Initialize(&device);

    Resource::TextureMetadata metadata;
    metadata.width = 4;
    metadata.height = 4;
    metadata.depth = 1;
    metadata.mipLevels = 3;
    metadata.arrayLayers = 1;
    metadata.format = Resource::TextureFormat::BC1;
    metadata.isSRGB = true;

    const std::vector<uint8> blocks = {
        1, 2, 3, 4, 5, 6, 7, 8,
        9, 10, 11, 12, 13, 14, 15, 16,
        17, 18, 19, 20, 21, 22, 23, 24
    };
    auto texture = std::make_unique<Resource::TextureResource>();
    texture->SetId(124);
    texture->SetName("TestTexture");
    texture->SetData(blocks, metadata);
    manager.UploadImmediate(texture.get());

    ASSERT_EQ(manager.GetResourceState(texture->GetId()), GPUResourceState::GPUReady);
    EXPECT_TRUE(manager.IsResident(texture->GetId()));
    ASSERT_NE(nullptr, manager.GetTexture(texture->GetId()));
    EXPECT_EQ(manager.GetTexture(texture->GetId())->GetFormat(), RHIFormat::BC1_UNORM_SRGB);
    EXPECT_EQ(manager.GetStats().usedMemory, 24ull);
    EXPECT_EQ(device.lastCreatedTextureDesc.format, RHIFormat::BC1_UNORM_SRGB);
    EXPECT_EQ(device.lastCreatedTextureDesc.mipLevels, 3u);
    ASSERT_NE(nullptr, device.lastCommandContext);
    ASSERT_NE(nullptr, device.lastStagingBuffer);
    ASSERT_EQ(device.lastCommandContext->bufferTextureCopyDescs.size(), 3u);

    const auto& mip0 = device.lastCommandContext->bufferTextureCopyDescs[0];
    const auto& mip1 = device.lastCommandContext->bufferTextureCopyDescs[1];
    const auto& mip2 = device.lastCommandContext->bufferTextureCopyDescs[2];
    EXPECT_EQ(mip0.bufferRowPitch, 256u);
    EXPECT_EQ(mip0.bufferImageHeight, 1u);
    EXPECT_EQ(mip0.textureRegion.width, 4u);
    EXPECT_EQ(mip0.textureRegion.height, 4u);
    EXPECT_EQ(mip1.bufferOffset, 512ull);
    EXPECT_EQ(mip1.textureRegion.width, 2u);
    EXPECT_EQ(mip1.textureRegion.height, 2u);
    EXPECT_EQ(mip2.bufferOffset, 1024ull);
    EXPECT_EQ(mip2.textureRegion.width, 1u);
    EXPECT_EQ(mip2.textureRegion.height, 1u);

    const auto& storage = device.lastStagingBuffer->GetStorage();
    EXPECT_EQ(storage[static_cast<size_t>(mip0.bufferOffset)], 1u);
    EXPECT_EQ(storage[static_cast<size_t>(mip1.bufferOffset)], 9u);
    EXPECT_EQ(storage[static_cast<size_t>(mip2.bufferOffset)], 17u);

    manager.Shutdown();
}

TEST(GPUResourceManagerValidation, BlockCompressedTextureRejectsMismatchedPayloadSize)
{
    FakeDevice device;
    device.supportStagedCopy = true;

    GPUResourceManager manager;
    manager.Initialize(&device);

    Resource::TextureMetadata metadata;
    metadata.width = 4;
    metadata.height = 4;
    metadata.depth = 1;
    metadata.mipLevels = 1;
    metadata.arrayLayers = 1;
    metadata.format = Resource::TextureFormat::BC3;
    metadata.isSRGB = true;

    auto texture = CreateTextureResourceWithMetadata(125, metadata, {1, 2, 3, 4, 5, 6, 7, 8});
    manager.UploadImmediate(texture.get());

    EXPECT_EQ(manager.GetResourceState(texture->GetId()), GPUResourceState::Failed);
    EXPECT_FALSE(manager.IsResident(texture->GetId()));
    EXPECT_EQ(manager.GetTexture(texture->GetId()), nullptr);
    EXPECT_EQ(device.createdTextureCount, 0u);

    manager.Shutdown();
}

TEST(GPUResourceManagerValidation, BC7TextureDataUploadsWithBlockLayout)
{
    FakeDevice device;
    device.supportStagedCopy = true;

    GPUResourceManager manager;
    manager.Initialize(&device);

    Resource::TextureMetadata metadata;
    metadata.width = 4;
    metadata.height = 4;
    metadata.depth = 1;
    metadata.mipLevels = 1;
    metadata.arrayLayers = 1;
    metadata.format = Resource::TextureFormat::BC7;
    metadata.isSRGB = true;

    const std::vector<uint8> block = {
        0x40, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
        0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF
    };
    auto texture = std::make_unique<Resource::TextureResource>();
    texture->SetId(126);
    texture->SetName("TestTexture");
    texture->SetData(block, metadata);
    manager.UploadImmediate(texture.get());

    ASSERT_EQ(manager.GetResourceState(texture->GetId()), GPUResourceState::GPUReady);
    EXPECT_TRUE(manager.IsResident(texture->GetId()));
    ASSERT_NE(nullptr, manager.GetTexture(texture->GetId()));
    EXPECT_EQ(manager.GetTexture(texture->GetId())->GetFormat(), RHIFormat::BC7_UNORM_SRGB);
    EXPECT_EQ(manager.GetStats().usedMemory, 16ull);
    EXPECT_EQ(device.lastCreatedTextureDesc.format, RHIFormat::BC7_UNORM_SRGB);
    ASSERT_NE(nullptr, device.lastCommandContext);
    ASSERT_NE(nullptr, device.lastStagingBuffer);
    ASSERT_EQ(device.lastCommandContext->bufferTextureCopyDescs.size(), 1u);

    const auto& copy = device.lastCommandContext->bufferTextureCopyDescs[0];
    EXPECT_EQ(copy.bufferRowPitch, 256u);
    EXPECT_EQ(copy.bufferImageHeight, 1u);
    EXPECT_EQ(copy.textureRegion.width, 4u);
    EXPECT_EQ(copy.textureRegion.height, 4u);

    const auto& storage = device.lastStagingBuffer->GetStorage();
    EXPECT_EQ(storage[static_cast<size_t>(copy.bufferOffset)], 0x40u);
    EXPECT_EQ(storage[static_cast<size_t>(copy.bufferOffset) + 15u], 0xFFu);

    manager.Shutdown();
}

TEST(GPUResourceManagerValidation, MippedCubemapDataIsRepackedToRHIFlatOrder)
{
    FakeDevice device;
    device.supportStagedCopy = true;

    GPUResourceManager manager;
    manager.Initialize(&device);

    Resource::TextureMetadata metadata;
    metadata.width = 2;
    metadata.height = 2;
    metadata.depth = 1;
    metadata.mipLevels = 2;
    metadata.arrayLayers = 6;
    metadata.format = Resource::TextureFormat::RGBA8;
    metadata.isCubemap = true;
    metadata.isSRGB = false;

    std::vector<uint8> pixels;
    pixels.reserve(120);
    for (uint32 mipLevel = 0; mipLevel < 2; ++mipLevel)
    {
        const uint32 mipSize = mipLevel == 0 ? 16u : 4u;
        for (uint32 face = 0; face < 6; ++face)
        {
            pixels.insert(pixels.end(), mipSize, static_cast<uint8>((mipLevel == 0 ? 10 : 100) + face));
        }
    }

    auto texture = CreateTextureResourceWithMetadata(121, metadata, pixels);
    manager.UploadImmediate(texture.get());

    ASSERT_EQ(manager.GetResourceState(texture->GetId()), GPUResourceState::GPUReady);
    ASSERT_NE(nullptr, device.lastCommandContext);
    ASSERT_NE(nullptr, device.lastStagingBuffer);
    ASSERT_EQ(device.lastCommandContext->bufferTextureCopyDescs.size(), 12u);

    const auto& storage = device.lastStagingBuffer->GetStorage();
    const auto& face0Mip0 = device.lastCommandContext->bufferTextureCopyDescs[0];
    const auto& face0Mip1 = device.lastCommandContext->bufferTextureCopyDescs[1];
    const auto& face1Mip0 = device.lastCommandContext->bufferTextureCopyDescs[2];
    const auto& face1Mip1 = device.lastCommandContext->bufferTextureCopyDescs[3];

    EXPECT_EQ(face0Mip0.textureSubresource, EncodeTextureSubresource(0, 0, 2));
    EXPECT_EQ(face0Mip1.textureSubresource, EncodeTextureSubresource(1, 0, 2));
    EXPECT_EQ(face1Mip0.textureSubresource, EncodeTextureSubresource(0, 1, 2));
    EXPECT_EQ(face1Mip1.textureSubresource, EncodeTextureSubresource(1, 1, 2));

    EXPECT_EQ(storage[static_cast<size_t>(face0Mip0.bufferOffset)], 10u);
    EXPECT_EQ(storage[static_cast<size_t>(face0Mip1.bufferOffset)], 100u);
    EXPECT_EQ(storage[static_cast<size_t>(face1Mip0.bufferOffset)], 11u);
    EXPECT_EQ(storage[static_cast<size_t>(face1Mip1.bufferOffset)], 101u);

    manager.Shutdown();
}

TEST(GPUResourceManagerValidation, UnsupportedTextureLayoutsFailWithoutCreatingTexture)
{
    struct UnsupportedCase
    {
        const char* name = nullptr;
        Resource::TextureMetadata metadata;
        std::vector<uint8> pixels;
    };

    auto makeMetadata = []()
    {
        Resource::TextureMetadata metadata;
        metadata.width = 1;
        metadata.height = 1;
        metadata.depth = 1;
        metadata.mipLevels = 1;
        metadata.arrayLayers = 1;
        metadata.format = Resource::TextureFormat::RGBA8;
        metadata.isSRGB = false;
        return metadata;
    };

    std::vector<UnsupportedCase> cases;
    {
        auto metadata = makeMetadata();
        metadata.isCubemap = true;
        metadata.arrayLayers = 5;
        cases.push_back({"cubemap", metadata, {1, 2, 3, 4}});
    }
    {
        auto metadata = makeMetadata();
        metadata.depth = 2;
        cases.push_back({"3d", metadata, {1, 2, 3, 4, 5, 6, 7, 8}});
    }
    {
        auto metadata = makeMetadata();
        metadata.format = Resource::TextureFormat::RGB8;
        metadata.mipLevels = 2;
        cases.push_back({"rgb8-mip-chain", metadata, {1, 2, 3, 4}});
    }
    {
        auto metadata = makeMetadata();
        metadata.arrayLayers = 2;
        cases.push_back({"array-layers-without-array-flag", metadata, {1, 2, 3, 4, 5, 6, 7, 8}});
    }
    for (size_t caseIndex = 0; caseIndex < cases.size(); ++caseIndex)
    {
        SCOPED_TRACE(cases[caseIndex].name);

        FakeDevice device;
        device.supportStagedCopy = true;

        GPUResourceManager manager;
        manager.Initialize(&device);

        const Resource::ResourceId id = static_cast<Resource::ResourceId>(200 + caseIndex);
        auto texture = CreateTextureResourceWithMetadata(
            id,
            cases[caseIndex].metadata,
            cases[caseIndex].pixels);

        manager.UploadImmediate(texture.get());

        EXPECT_EQ(manager.GetResourceState(texture->GetId()), GPUResourceState::Failed);
        EXPECT_FALSE(manager.IsResident(texture->GetId()));
        EXPECT_EQ(manager.GetTexture(texture->GetId()), nullptr);
        EXPECT_EQ(device.createdTextureCount, 0u);

        manager.Shutdown();
    }
}

TEST(GPUResourceManagerValidation, ResidentTextureImmediateUploadIgnoresInvalidSameIdReplacement)
{
    FakeDevice device;
    device.supportStagedCopy = true;
    device.completeSubmittedFenceImmediately = true;

    GPUResourceManager manager;
    manager.Initialize(&device);

    uint32 invalidatedCount = 0;
    manager.SetTextureInvalidatedCallback(
        [&invalidatedCount](RHITexture*)
        {
            ++invalidatedCount;
        });

    auto original = CreateTextureResource(111);
    manager.UploadImmediate(original.get());

    RHITexture* originalTexture = manager.GetTexture(original->GetId());
    ASSERT_NE(nullptr, originalTexture);
    const size_t originalMemory = manager.GetStats().usedMemory;
    ASSERT_GT(originalMemory, 0ull);
    ASSERT_EQ(device.createdTextureCount, 1u);

    auto replacement = CreateTextureResource(
        111,
        Resource::TextureFormat::RGBA8,
        {1, 2, 3},
        1,
        1);
    manager.UploadImmediate(replacement.get());

    EXPECT_EQ(manager.GetResourceState(original->GetId()), GPUResourceState::GPUReady);
    EXPECT_EQ(manager.GetTexture(original->GetId()), originalTexture);
    EXPECT_TRUE(manager.IsResident(original->GetId()));
    EXPECT_EQ(manager.GetStats().usedMemory, originalMemory);
    EXPECT_EQ(device.createdTextureCount, 1u);
    EXPECT_EQ(invalidatedCount, 0u);

    manager.Shutdown();
}

TEST(GPUResourceManagerValidation, ResidentTextureImmediateUploadIsNoOpForSameIdReplacement)
{
    FakeDevice device;
    device.supportStagedCopy = true;
    device.completeSubmittedFenceImmediately = true;

    GPUResourceManager manager;
    manager.Initialize(&device);

    uint32 invalidatedCount = 0;
    manager.SetTextureInvalidatedCallback(
        [&invalidatedCount](RHITexture*)
        {
            ++invalidatedCount;
        });

    auto original = CreateTextureResource(112);
    manager.UploadImmediate(original.get());

    RHITexture* originalTexture = manager.GetTexture(original->GetId());
    ASSERT_NE(nullptr, originalTexture);
    EXPECT_EQ(manager.GetStats().usedMemory, 4ull);
    ASSERT_EQ(device.createdTextureCount, 1u);

    auto replacement = CreateTextureResource(
        112,
        Resource::TextureFormat::RGBA8,
        {9, 8, 7, 6},
        1,
        1);
    manager.UploadImmediate(replacement.get());

    RHITexture* replacementTexture = manager.GetTexture(original->GetId());
    ASSERT_NE(nullptr, replacementTexture);
    EXPECT_EQ(originalTexture, replacementTexture);
    EXPECT_TRUE(manager.IsResident(original->GetId()));
    EXPECT_EQ(manager.GetResourceState(original->GetId()), GPUResourceState::GPUReady);
    EXPECT_EQ(manager.GetStats().usedMemory, 4ull);
    EXPECT_EQ(device.createdTextureCount, 1u);
    EXPECT_EQ(invalidatedCount, 0u);

    manager.Shutdown();
}

TEST(GPUResourceManagerValidation, TextureEvictionNotifiesViewCachesBeforeRelease)
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
    ASSERT_NE(nullptr, residentTexture);
    EXPECT_EQ(invalidatedCount, 0u);

    manager.EvictUnused(10, 1);

    EXPECT_EQ(invalidatedCount, 1u);
    EXPECT_EQ(invalidatedTexture, residentTexture);
    EXPECT_TRUE(manager.GetTexture(texture->GetId()) == nullptr);
    EXPECT_FALSE(manager.IsResident(texture->GetId()));
    EXPECT_EQ(manager.GetResourceState(texture->GetId()), GPUResourceState::Unloaded);

    manager.Shutdown();
}

TEST(GPUResourceManagerValidation, UnsupportedTextureUploadMarksResourceFailed)
{
    FakeDevice device;
    device.supportStagedCopy = false;

    GPUResourceManager manager;
    manager.Initialize(&device);

    auto texture = CreateTextureResource(106);
    manager.UploadImmediate(texture.get());

    EXPECT_EQ(manager.GetResourceState(texture->GetId()), GPUResourceState::Failed);
    EXPECT_FALSE(manager.IsResident(texture->GetId()));
    EXPECT_FALSE(manager.IsGPUReady(texture->GetId()));
    EXPECT_TRUE(manager.GetTexture(texture->GetId()) == nullptr);
    EXPECT_EQ(device.createdTextureCount, 0u);
    EXPECT_EQ(device.submittedCommandContextCount, 0u);

    const auto stats = manager.GetStats();
    EXPECT_EQ(stats.failedUploadCount, 1ull);
    EXPECT_EQ(stats.residentTextureCount, 0ull);

    manager.Shutdown();
}
